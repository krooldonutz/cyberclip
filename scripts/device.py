#!/usr/bin/env python3
"""Talk to a Cyberclip board over USB serial, for testing from the command line.

Usage (from the repository root):
  python scripts/device.py ports
  python scripts/device.py [--port COM8] hello
  python scripts/device.py [--port COM8] status
  python scripts/device.py [--port COM8] wifi
  python scripts/device.py [--port COM8] backlight <0-255>
  python scripts/device.py [--port COM8] watch [--interval 10] [--duration 600] [--csv log.csv]
  python scripts/device.py [--port COM8] flash

Needs pyserial. PlatformIO's own Python has it:
  ~/.platformio/penv/Scripts/python.exe scripts/device.py status

Speaks the same framed protocol as the web app (firmware/protocol.h): magic
"CC", version, command, sequence u16, length u32, payload, CRC-16/CCITT-FALSE.
Only read-only commands and the backlight are exposed; nothing here clears
stored media or WiFi settings.
"""

import argparse
import csv
import struct
import subprocess
import sys
import time
from pathlib import Path

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    sys.exit('pyserial is missing. Run this with ~/.platformio/penv/Scripts/python.exe '
             'or "pip install pyserial".')

REPOSITORY_ROOT = Path(__file__).resolve().parent.parent
FIRMWARE_DIRECTORY = REPOSITORY_ROOT / 'firmware'
ESPRESSIF_VID = 0x303A
PROTOCOL_VERSION = 2
BAUD = 921600

HELLO, SET_BACKLIGHT, GET_STATUS, GET_WIFI_STATUS = 0x01, 0x20, 0x22, 0x41
HELLO_RESPONSE, STATUS_RESPONSE, WIFI_STATUS_RESPONSE = 0x81, 0xA2, 0xA3
ACK, NACK = 0xF0, 0xF1
WIFI_STATES = {0: 'off', 1: 'connecting', 2: 'connected', 3: 'failed'}


def crc16(data, crc=0xFFFF):
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) if crc & 0x8000 else (crc << 1)
            crc &= 0xFFFF
    return crc


def encode(command, sequence, payload=b''):
    body = struct.pack('<BBHI', PROTOCOL_VERSION, command, sequence, len(payload)) + payload
    return b'CC' + body + struct.pack('<H', crc16(body))


class Device:
    def __init__(self, port):
        self.port = port
        self.serial = serial.Serial(port, BAUD, timeout=0.2)
        self.sequence = 1
        time.sleep(0.2)
        self.serial.reset_input_buffer()

    def close(self):
        self.serial.close()

    def request(self, command, payload=b'', expected=None, timeout=3.0):
        sequence = self.sequence
        self.sequence = (self.sequence + 1) & 0xFFFF or 1
        self.serial.write(encode(command, sequence, payload))
        deadline = time.monotonic() + timeout
        buffer = bytearray()
        while time.monotonic() < deadline:
            buffer += self.serial.read(256)
            while True:
                start = buffer.find(b'CC')
                if start < 0:
                    del buffer[:-1]
                    break
                del buffer[:start]
                if len(buffer) < 10:
                    break
                _, reply, reply_sequence, length = struct.unpack_from('<BBHI', buffer, 2)
                if length > 4096:
                    del buffer[:2]
                    continue
                if len(buffer) < 12 + length:
                    break
                body = bytes(buffer[2:10 + length])
                (received_crc,) = struct.unpack_from('<H', buffer, 10 + length)
                del buffer[:12 + length]
                if received_crc != crc16(body) or reply_sequence != sequence:
                    continue
                data = body[8:]
                if reply == NACK:
                    raise RuntimeError(f'Device rejected command 0x{command:02x} (error {data[-1] if data else "?"})')
                if expected is None or reply == expected:
                    return data
        raise TimeoutError(f'No reply to command 0x{command:02x} on {self.port}')


def find_port():
    ports = [p for p in list_ports.comports() if p.vid == ESPRESSIF_VID]
    if not ports:
        sys.exit('No Cyberclip board found. Is it plugged in over USB and awake?')
    return ports[0].device


def hello(device):
    data = device.request(HELLO, expected=HELLO_RESPONSE)
    width, height, chunk, max_frame = struct.unpack_from('<HHHI', data, 0)
    major, minor, patch = data[12], data[13], data[14]
    stored = struct.unpack_from('<I', data, 16)[0] if len(data) >= 20 else 0
    return {
        'name': data[20:].decode('utf-8', 'replace'),
        'firmware': f'{major}.{minor}.{patch}',
        'display': f'{width}x{height}',
        'max_chunk': chunk,
        'max_frame_bytes': max_frame,
        'stored_bytes': stored,
    }


def status(device):
    data = device.request(GET_STATUS, expected=STATUS_RESPONSE)
    result = {
        'transfer_active': bool(data[0]),
        'backlight': data[1],
        'free_heap': struct.unpack_from('<I', data, 12)[0],
    }
    if len(data) >= 25:
        millivolts, on_battery, cpu_mhz, uptime_ms = struct.unpack_from('<HBHI', data, 16)
        result.update(battery_mv=millivolts, on_battery=bool(on_battery),
                      cpu_mhz=cpu_mhz, uptime_s=round(uptime_ms / 1000, 1))
    else:
        result['note'] = 'firmware is too old to report battery, CPU clock and uptime'
    return result


def wifi(device):
    data = device.request(GET_WIFI_STATUS, expected=WIFI_STATUS_RESPONSE)
    hostname_length = data[5]
    return {
        'state': WIFI_STATES.get(data[0], data[0]),
        'ip': '.'.join(str(b) for b in data[1:5]),
        'hostname': data[6:6 + hostname_length].decode('utf-8', 'replace'),
    }


def print_dict(values):
    for key, value in values.items():
        print(f'{key:16} {value}')


def watch(device, interval, duration, csv_path):
    writer = None
    handle = None
    if csv_path:
        handle = open(csv_path, 'w', newline='')
    started = time.monotonic()
    try:
        while duration <= 0 or time.monotonic() - started < duration:
            row = {'time': time.strftime('%H:%M:%S'), **status(device)}
            print('  '.join(f'{k}={v}' for k, v in row.items()), flush=True)
            if handle:
                if writer is None:
                    writer = csv.DictWriter(handle, fieldnames=list(row))
                    writer.writeheader()
                writer.writerow(row)
                handle.flush()
            time.sleep(interval)
    finally:
        if handle:
            handle.close()


DOWNLOAD_PID = 0x1001  # ESP32-S3 built-in USB-Serial/JTAG, used by the ROM bootloader
# A 1200-baud reboot sets FORCE_DOWNLOAD_BOOT, which survives a reset; it has to
# be cleared or the board boots straight back into download mode (src/flasher.js).
RTC_CNTL_OPTION1_REG = 0x6000812C
RTC_CNTL_FORCE_DOWNLOAD_BOOT = 0x1


def platformio_tool(name):
    scripts = 'Scripts' if sys.platform == 'win32' else 'bin'
    return Path.home() / '.platformio' / 'penv' / scripts / name


def esptool(port, *arguments):
    script = Path.home() / '.platformio' / 'packages' / 'tool-esptoolpy' / 'esptool.py'
    command = [sys.executable, str(script), '--chip', 'esp32s3', '--port', port, *arguments]
    print(' '.join(command), flush=True)
    return subprocess.call(command)


def wait_for_download_port(timeout=20):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        for p in list_ports.comports():
            if p.vid == ESPRESSIF_VID and p.pid == DOWNLOAD_PID:
                time.sleep(0.5)
                return p.device
        time.sleep(0.25)
    return None


def flash(port):
    """Builds and installs the firmware the way the web installer reaches
    download mode. The firmware runs TinyUSB on the native USB port, which
    esptool's DTR/RTS reset can't handle: the board re-enumerates on another
    port mid-connect. Instead, a 1200-baud open reboots it into ROM download
    mode on the USB-Serial/JTAG port, and esptool flashes that port."""
    current = next((p for p in list_ports.comports() if p.device == port), None)
    rebooted = False
    if current is None or current.pid != DOWNLOAD_PID:
        print(f'Restarting the board on {port} into download mode', flush=True)
        try:
            serial.Serial(port, 1200).close()
        except serial.SerialException:
            pass  # it may reboot before the open settles
        rebooted = True
        download_port = wait_for_download_port()
        if not download_port:
            print('The board did not come back in download mode. Unplug it (and any battery), '
                  'hold BOOT while plugging USB back in, release BOOT, then run flash again.')
            return 1
    else:
        download_port = port
    print(f'Flashing over {download_port}', flush=True)

    # Full install (bootloader, partition table, app), so a new board or one
    # with another partition layout boots too. Stored media survives when the
    # layout already matches.
    result = subprocess.call(
        [str(platformio_tool('pio')), 'run', '-e', 'lilygo_t_display_s3', '-t', 'upload',
         '--upload-port', download_port], cwd=FIRMWARE_DIRECTORY)
    if result != 0:
        return result
    if rebooted:
        # Clear the forced download boot set by the 1200-baud reboot, then reset.
        port_now = wait_for_download_port(timeout=5)
        if port_now:
            esptool(port_now, '--before', 'no_reset', '--after', 'hard_reset',
                    'write_mem', hex(RTC_CNTL_OPTION1_REG), '0',
                    hex(RTC_CNTL_FORCE_DOWNLOAD_BOOT))
    print('Flashed. Waiting for the board to boot the new firmware...', flush=True)
    deadline = time.monotonic() + 20
    while time.monotonic() < deadline:
        if any(p.vid == ESPRESSIF_VID and p.pid != DOWNLOAD_PID for p in list_ports.comports()):
            time.sleep(1)
            return 0
        time.sleep(0.25)
    print('The firmware was written, but the board has not reappeared. Press RST to restart it.')
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('--port', help='serial port (default: first Espressif USB device)')
    commands = parser.add_subparsers(dest='command', required=True)
    commands.add_parser('ports', help='list serial ports')
    commands.add_parser('hello', help='firmware version and display info')
    commands.add_parser('status', help='battery, CPU clock, heap, uptime')
    commands.add_parser('wifi', help='WiFi state and address')
    backlight = commands.add_parser('backlight', help='set backlight 0-255')
    backlight.add_argument('level', type=int)
    watcher = commands.add_parser('watch', help='poll status repeatedly')
    watcher.add_argument('--interval', type=float, default=10)
    watcher.add_argument('--duration', type=float, default=0, help='seconds, 0 = until Ctrl+C')
    watcher.add_argument('--csv', help='also write rows to this CSV file')
    commands.add_parser('flash', help='build and flash firmware over USB')
    args = parser.parse_args()

    if args.command == 'ports':
        for p in list_ports.comports():
            tag = '  <- Cyberclip (Espressif)' if p.vid == ESPRESSIF_VID else ''
            print(f'{p.device:8} {p.description}{tag}')
        return 0

    if args.command == 'flash':
        port = args.port or next(
            (p.device for p in list_ports.comports() if p.vid == ESPRESSIF_VID), None)
        if not port:
            print('No board on USB. Waiting 2 minutes for it in download mode: unplug it '
                  '(and any battery), hold BOOT while plugging USB back in, release BOOT.',
                  flush=True)
            port = wait_for_download_port(timeout=120)
            if not port:
                sys.exit('Still no board.')
        return flash(port)
    port = args.port or find_port()

    device = Device(port)
    try:
        if args.command == 'hello':
            print_dict(hello(device))
        elif args.command == 'status':
            print_dict(status(device))
        elif args.command == 'wifi':
            print_dict(wifi(device))
        elif args.command == 'backlight':
            if not 0 <= args.level <= 255:
                sys.exit('backlight must be 0-255')
            device.request(SET_BACKLIGHT, bytes([args.level]), expected=ACK)
            print(f'backlight set to {args.level}')
        elif args.command == 'watch':
            watch(device, args.interval, args.duration, args.csv)
    except KeyboardInterrupt:
        pass
    finally:
        device.close()
    return 0


if __name__ == '__main__':
    sys.exit(main())
