#!/usr/bin/env python3
"""Mirror part of a PC screen onto the Cyberclip, live (hidden developer tool).

Not linked from the web app. Captures a monitor or a screen region, scales it
to the 320x170 panel, JPEG-encodes it and streams it as MIRROR_FRAME parts
(see firmware/protocol.h). Nothing is saved on the board. Ctrl+C stops and
puts the saved item back on screen (--keep leaves the last frame).

Over USB (the board plugged in; auto-detected, or --port COM8):
  python scripts/mirror.py
Over WiFi (hold BOOT to turn the hotspot on, join it from the PC with the
password in the join QR code, then):
  python scripts/mirror.py --wifi

Options:
  --list                 list monitors and exit
  --monitor N            monitor to mirror (1 = primary, default)
  --region X,Y,W,H       mirror this desktop rectangle instead (pixels)
  --fit fit|fill|stretch letterbox (default), crop to fill, or stretch
  --fps 25               target frame rate; quality drops while the
                         picture moves too much to keep it, then sharpens
  --quality 70           JPEG quality of a still picture
  --rotate 1             1 = landscape (default), 3 = landscape upside down
  --no-cursor            don't draw the mouse pointer
  --threshold 6          colour change (0-255) a pixel needs to count as
                         changed; 0 sends every difference

Only one request is ever in flight: the board's USB stack crashes when a
second packet arrives before it has answered the first. Speed comes from
sending only the rectangle that changed, in as few requests as possible
(MIRROR_FRAME, about 4 KB each), from the board decoding while the next
frame arrives, and from capturing on another thread.

Needs Pillow, plus pyserial for USB:  pip install pillow pyserial
For a true "second monitor", add a virtual display in Windows (any virtual
display driver) and mirror that monitor with --monitor.
"""

import argparse
import base64
import io
import os
import socket
import struct
import sys
import threading
import time

try:
    from PIL import Image, ImageChops, ImageDraw, ImageGrab
except ImportError:
    sys.exit('Pillow is missing. Run "pip install pillow pyserial".')

ESPRESSIF_VID = 0x303A
PROTOCOL_VERSION = 2
BAUD = 921600
HOTSPOT_HOST = '192.168.4.1'

HELLO, BEGIN_FRAME, FRAME_CHUNK, COMMIT_FRAME = 0x01, 0x10, 0x11, 0x12
CANCEL_TRANSFER, MIRROR_FRAME, PLAY_STORED = 0x13, 0x14, 0x32
NACK = 0xF1
UNSUPPORTED_COMMAND, NEEDS_FULL_FRAME = 2, 12
CODEC_JPEG = 1
MAX_WIRE_PAYLOAD = 4096
MIRROR_HEADER = 19
MIRROR_PART_BYTES = MAX_WIRE_PAYLOAD - MIRROR_HEADER
# Resend an unchanged screen this often: on battery the board turns WiFi off
# after 2 minutes without a command.
KEEPALIVE_S = 10.0
REPLY_TIMEOUT_S = 3.0
# Changed rectangles are aligned to JPEG blocks (16 px with 4:2:0 chroma).
BLOCK = 16
# A pixel counts as changed when a colour channel moves by more than this
# (0-255), so capture noise and faint flicker don't cost a frame.
DEFAULT_THRESHOLD = 6
# A change bigger than this share of the frame is sent as a whole frame.
PATCH_MAX_SHARE = 0.85
# While the picture moves faster than USB can carry sharp frames, JPEG
# quality drops (down to this) to keep the frame rate; once it stands still
# for SHARPEN_AFTER_S, a sharp whole frame replaces the soft one.
MIN_QUALITY = 20
SHARPEN_AFTER_S = 0.3
# Send a whole frame at least this often, in case anything drifted.
FULL_REFRESH_S = 3.0
# Give up after this many failed frames in a row.
MAX_FAILURES = 5


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


class ReplyParser:
    """Splits the board's byte stream into (command, sequence, payload)."""

    def __init__(self):
        self.buffer = bytearray()

    def feed(self, data):
        self.buffer += data
        replies = []
        while True:
            start = self.buffer.find(b'CC')
            if start < 0:
                del self.buffer[:-1]
                return replies
            del self.buffer[:start]
            if len(self.buffer) < 10:
                return replies
            _, command, sequence, length = struct.unpack_from('<BBHI', self.buffer, 2)
            if length > MAX_WIRE_PAYLOAD:
                del self.buffer[:2]
                continue
            if len(self.buffer) < 12 + length:
                return replies
            body = bytes(self.buffer[2:10 + length])
            (crc,) = struct.unpack_from('<H', self.buffer, 10 + length)
            if crc != crc16(body):
                del self.buffer[:2]
                continue
            del self.buffer[:12 + length]
            replies.append((command, sequence, body[8:]))


# --- Transports --------------------------------------------------------------

class SerialLink:
    def __init__(self, port):
        try:
            import serial
            from serial.tools import list_ports
        except ImportError:
            sys.exit('pyserial is missing. Run "pip install pyserial".')
        if port is None:
            ports = [p.device for p in list_ports.comports()
                     if p.vid == ESPRESSIF_VID and p.pid != 0x1001]
            if not ports:
                sys.exit('No Cyberclip found over USB. Plug it in (press a button if it '
                         'runs on battery), or use --wifi.')
            port = ports[0]
        self.name = port
        # Default DTR/RTS handling, the same as scripts/device.py.
        self.serial = serial.Serial(port, BAUD, timeout=0.05, write_timeout=5)
        time.sleep(0.1)
        self.serial.reset_input_buffer()

    def send(self, data):
        self.serial.write(data)

    def read(self):
        return self.serial.read(max(1, self.serial.in_waiting))

    def flush_input(self):
        self.serial.reset_input_buffer()

    def close(self):
        self.serial.close()


class WebSocketLink:
    """Just enough of a WebSocket client for the board's /ws endpoint."""

    def __init__(self, host):
        self.name = f'ws://{host}/ws'
        try:
            self.sock = socket.create_connection((host, 80), timeout=5)
        except OSError as error:
            sys.exit(f'Cannot reach {host}: {error}. Is the hotspot on (hold BOOT) '
                     'and this PC joined to it?')
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        key = base64.b64encode(os.urandom(16)).decode()
        self.sock.sendall((
            f'GET /ws HTTP/1.1\r\nHost: {host}\r\nUpgrade: websocket\r\n'
            f'Connection: Upgrade\r\nSec-WebSocket-Key: {key}\r\n'
            'Sec-WebSocket-Version: 13\r\n\r\n').encode())
        response = b''
        while b'\r\n\r\n' not in response:
            chunk = self.sock.recv(1024)
            if not chunk:
                sys.exit('The board closed the WebSocket handshake.')
            response += chunk
        head, rest = response.split(b'\r\n\r\n', 1)
        self.rx = bytearray(rest)
        if b' 101 ' not in head.split(b'\r\n', 1)[0]:
            sys.exit(f'WebSocket refused: {head.splitlines()[0].decode(errors="replace")}')
        self.sock.settimeout(0.05)
        # The board authorizes a hotspot client by an empty first message.
        self.send(b'')

    def _frame(self, opcode, payload):
        header = bytearray([0x80 | opcode])
        length = len(payload)
        if length < 126:
            header.append(0x80 | length)
        elif length < 65536:
            header += bytes([0x80 | 126]) + struct.pack('>H', length)
        else:
            header += bytes([0x80 | 127]) + struct.pack('>Q', length)
        mask = os.urandom(4)
        masked = (int.from_bytes(payload, 'little') ^
                  int.from_bytes(mask * (length // 4 + 1), 'little')
                  & ((1 << (8 * length)) - 1)).to_bytes(length, 'little')
        return bytes(header) + mask + masked

    def send(self, data):
        self.sock.sendall(self._frame(0x2, data))

    def read(self):
        try:
            chunk = self.sock.recv(8192)
            if not chunk:
                raise ConnectionError('The board closed the WiFi connection.')
            self.rx += chunk
        except socket.timeout:
            pass
        out = bytearray()
        while len(self.rx) >= 2:
            opcode = self.rx[0] & 0x0F
            length = self.rx[1] & 0x7F
            offset = 2
            if length == 126:
                if len(self.rx) < 4:
                    break
                (length,) = struct.unpack_from('>H', self.rx, 2)
                offset = 4
            elif length == 127:
                if len(self.rx) < 10:
                    break
                (length,) = struct.unpack_from('>Q', self.rx, 2)
                offset = 10
            if len(self.rx) < offset + length:
                break
            payload = bytes(self.rx[offset:offset + length])
            del self.rx[:offset + length]
            if opcode == 0x8:
                raise ConnectionError('The board closed the WiFi connection '
                                      '(another app may be connected).')
            if opcode == 0x9:
                self.sock.sendall(self._frame(0xA, payload))
            elif opcode in (0x0, 0x2):
                out += payload
        return bytes(out)

    def flush_input(self):
        self.rx.clear()

    def close(self):
        try:
            self.sock.sendall(self._frame(0x8, b''))
        except OSError:
            pass
        self.sock.close()


# --- Board session -------------------------------------------------------------

class BoardError(Exception):
    def __init__(self, code):
        super().__init__(f'board rejected a request (error {code})')
        self.code = code


class Board:
    def __init__(self, link):
        self.link = link
        self.parser = ReplyParser()
        self.sequence = 0
        self.transfer_id = 0
        self.mirror_command = True
        data = self.request(HELLO)
        self.width, self.height, self.max_chunk, self.max_frame = \
            struct.unpack_from('<HHHI', data, 0)
        self.firmware = f'{data[12]}.{data[13]}.{data[14]}'

    def request(self, command, payload=b''):
        """Sends one request and waits for its reply. Never more than one
        request is in flight."""
        self.sequence = self.sequence % 0xFFFF + 1
        sequence = self.sequence
        self.link.send(encode(command, sequence, payload))
        deadline = time.monotonic() + REPLY_TIMEOUT_S
        while time.monotonic() < deadline:
            for reply, reply_sequence, data in self.parser.feed(self.link.read()):
                if reply_sequence != sequence:
                    continue
                if reply == NACK:
                    raise BoardError(data[-1] if data else 0)
                return data
        raise TimeoutError(f'The board stopped answering on {self.link.name}.')

    def resync(self):
        """After a timeout: let the board's parser time out (1 s), drop
        whatever was half-received, and start the next frame from scratch."""
        time.sleep(1.2)
        self.link.flush_input()
        self.parser = ReplyParser()

    def show_jpeg(self, jpeg, rect, frame_size, rotation):
        """Shows *jpeg* at *rect* (x, y, width, height) of the frame. Raises
        BoardError(NEEDS_FULL_FRAME) when the board can't patch its screen."""
        self.transfer_id = self.transfer_id % 0xFFFF + 1
        if self.mirror_command:
            try:
                for offset in range(0, len(jpeg), MIRROR_PART_BYTES):
                    self.request(MIRROR_FRAME, struct.pack(
                        '<HIIBHHHH', self.transfer_id, offset, len(jpeg), rotation, *rect)
                        + jpeg[offset:offset + MIRROR_PART_BYTES])
                return
            except BoardError as error:
                if error.code != UNSUPPORTED_COMMAND:
                    raise
                print('Older firmware: using the slower frame commands.', file=sys.stderr)
                self.mirror_command = False
        if tuple(rect) != (0, 0) + tuple(frame_size):
            raise BoardError(NEEDS_FULL_FRAME)
        width, height = frame_size
        tid = self.transfer_id
        self.request(BEGIN_FRAME, struct.pack('<HHHBBI', tid, width, height,
                                              CODEC_JPEG, rotation, len(jpeg)))
        try:
            for offset in range(0, len(jpeg), self.max_chunk):
                self.request(FRAME_CHUNK, struct.pack('<HI', tid, offset)
                             + jpeg[offset:offset + self.max_chunk])
            self.request(COMMIT_FRAME, struct.pack('<H', tid))
        except BoardError:
            try:
                self.request(CANCEL_TRANSFER, struct.pack('<H', tid))
            except BoardError:
                pass
            raise


# --- Screen capture ----------------------------------------------------------

def windows_setup():
    if sys.platform != 'win32':
        return
    import ctypes
    try:
        ctypes.windll.shcore.SetProcessDpiAwareness(2)  # per-monitor: real pixels
    except (AttributeError, OSError):
        try:
            ctypes.windll.user32.SetProcessDPIAware()
        except (AttributeError, OSError):
            pass


def list_monitors():
    """Desktop rectangles (left, top, right, bottom), primary first."""
    if sys.platform != 'win32':
        return [None]
    import ctypes
    from ctypes import wintypes

    class MONITORINFO(ctypes.Structure):
        _fields_ = [('cbSize', wintypes.DWORD), ('rcMonitor', wintypes.RECT),
                    ('rcWork', wintypes.RECT), ('dwFlags', wintypes.DWORD)]

    monitors = []
    callback_type = ctypes.WINFUNCTYPE(ctypes.c_int, wintypes.HMONITOR, wintypes.HDC,
                                       ctypes.POINTER(wintypes.RECT), wintypes.LPARAM)

    def callback(handle, _dc, _rect, _data):
        info = MONITORINFO()
        info.cbSize = ctypes.sizeof(MONITORINFO)
        ctypes.windll.user32.GetMonitorInfoW(handle, ctypes.byref(info))
        r = info.rcMonitor
        monitors.append(((r.left, r.top, r.right, r.bottom), bool(info.dwFlags & 1)))
        return 1

    ctypes.windll.user32.EnumDisplayMonitors(None, None, callback_type(callback), 0)
    monitors.sort(key=lambda m: not m[1])  # primary first, otherwise in OS order
    return [rect for rect, _ in monitors] or [None]


def cursor_position():
    if sys.platform != 'win32':
        return None
    import ctypes
    from ctypes import wintypes
    point = wintypes.POINT()
    if not ctypes.windll.user32.GetCursorPos(ctypes.byref(point)):
        return None
    return point.x, point.y


def fit_rect(source_size, target_size, mode):
    """Where the source lands in the target: (x, y, width, height)."""
    sw, sh = source_size
    tw, th = target_size
    if mode == 'stretch':
        return 0, 0, tw, th
    ratio = (max if mode == 'fill' else min)(tw / sw, th / sh)
    w, h = max(1, round(sw * ratio)), max(1, round(sh * ratio))
    return (tw - w) // 2, (th - h) // 2, w, h


class GdiCapture:
    """Windows: lets GDI shrink the screen straight into a panel-sized bitmap,
    which is several times faster than grabbing the full screen first."""

    def __init__(self, bbox, size, mode):
        import ctypes
        from ctypes import wintypes
        self.ctypes = ctypes
        self.user32 = ctypes.windll.user32
        self.gdi32 = ctypes.windll.gdi32
        self.gdi32.CreateDIBSection.restype = wintypes.HBITMAP
        self.gdi32.CreateCompatibleDC.restype = wintypes.HDC
        self.gdi32.CreateCompatibleDC.argtypes = [wintypes.HDC]
        self.user32.GetDC.restype = wintypes.HDC
        self.user32.GetDC.argtypes = [wintypes.HWND]
        self.gdi32.SelectObject.argtypes = [wintypes.HDC, wintypes.HGDIOBJ]
        self.gdi32.StretchBlt.argtypes = [wintypes.HDC] + [ctypes.c_int] * 4 + \
            [wintypes.HDC] + [ctypes.c_int] * 4 + [wintypes.DWORD]
        self.gdi32.PatBlt.argtypes = [wintypes.HDC] + [ctypes.c_int] * 4 + [wintypes.DWORD]
        self.gdi32.SetStretchBltMode.argtypes = [wintypes.HDC, ctypes.c_int]
        self.gdi32.SetBrushOrgEx.argtypes = [wintypes.HDC, ctypes.c_int, ctypes.c_int,
                                             ctypes.c_void_p]
        self.gdi32.CreateDIBSection.argtypes = [wintypes.HDC, ctypes.c_void_p, wintypes.UINT,
                                                ctypes.POINTER(ctypes.c_void_p),
                                                wintypes.HANDLE, wintypes.DWORD]
        if bbox is None:
            bbox = (0, 0, self.user32.GetSystemMetrics(0), self.user32.GetSystemMetrics(1))
        self.bbox = bbox
        self.size = size
        self.screen_dc = self.user32.GetDC(None)
        self.dc = self.gdi32.CreateCompatibleDC(self.screen_dc)

        class BITMAPINFOHEADER(ctypes.Structure):
            _fields_ = [('biSize', wintypes.DWORD), ('biWidth', wintypes.LONG),
                        ('biHeight', wintypes.LONG), ('biPlanes', wintypes.WORD),
                        ('biBitCount', wintypes.WORD), ('biCompression', wintypes.DWORD),
                        ('biSizeImage', wintypes.DWORD), ('biXPelsPerMeter', wintypes.LONG),
                        ('biYPelsPerMeter', wintypes.LONG), ('biClrUsed', wintypes.DWORD),
                        ('biClrImportant', wintypes.DWORD)]

        header = BITMAPINFOHEADER()
        header.biSize = ctypes.sizeof(BITMAPINFOHEADER)
        header.biWidth, header.biHeight = size[0], -size[1]  # top-down
        header.biPlanes, header.biBitCount = 1, 32
        self.bits = ctypes.c_void_p()
        self.bitmap = self.gdi32.CreateDIBSection(self.dc, ctypes.byref(header), 0,
                                                  ctypes.byref(self.bits), None, 0)
        if not self.bitmap:
            raise OSError('CreateDIBSection failed')
        self.gdi32.SelectObject(self.dc, self.bitmap)
        self.gdi32.SetStretchBltMode(self.dc, 4)  # HALFTONE: averages, no shimmer
        self.gdi32.SetBrushOrgEx(self.dc, 0, 0, None)
        source = (bbox[2] - bbox[0], bbox[3] - bbox[1])
        self.dest = fit_rect(source, size, mode)
        if mode == 'fill':
            # Crop the source instead of drawing outside the bitmap.
            x, y, w, h = self.dest
            scale = w / source[0]
            cw, ch = round(size[0] / scale), round(size[1] / scale)
            cx = bbox[0] + (source[0] - cw) // 2
            cy = bbox[1] + (source[1] - ch) // 2
            self.source = (cx, cy, cw, ch)
            self.dest = (0, 0, size[0], size[1])
        else:
            self.source = (bbox[0], bbox[1], source[0], source[1])

    def grab(self):
        x, y, w, h = self.dest
        if (w, h) != self.size:
            self.gdi32.PatBlt(self.dc, 0, 0, self.size[0], self.size[1], 0x00000042)  # black
        sx, sy, sw, sh = self.source
        self.gdi32.StretchBlt(self.dc, x, y, w, h, self.screen_dc, sx, sy, sw, sh,
                              0x00CC0020)  # SRCCOPY
        data = self.ctypes.string_at(self.bits, self.size[0] * self.size[1] * 4)
        return Image.frombuffer('RGB', self.size, data, 'raw', 'BGRX', 0, 1)

    def to_frame(self, point):
        """Desktop point -> frame pixel, or None if outside what is shown."""
        sx, sy, sw, sh = self.source
        x, y, w, h = self.dest
        fx = x + (point[0] - sx) * w / sw
        fy = y + (point[1] - sy) * h / sh
        if x <= fx < x + w and y <= fy < y + h:
            return fx, fy
        return None


class PilCapture:
    """Other systems: Pillow's screen grab, then a resize."""

    def __init__(self, bbox, size, mode):
        self.bbox, self.size, self.mode = bbox, size, mode

    def grab(self):
        image = ImageGrab.grab(bbox=self.bbox, all_screens=True).convert('RGB')
        x, y, w, h = fit_rect(image.size, self.size, self.mode)
        image = image.resize((w, h), Image.BILINEAR, reducing_gap=2.0)
        canvas = Image.new('RGB', self.size)
        canvas.paste(image, (x, y))
        return canvas

    def to_frame(self, point):
        return None


def draw_cursor(image, position):
    x, y = position
    arrow = [(x, y), (x, y + 11), (x + 3, y + 8), (x + 8, y + 8)]
    ImageDraw.Draw(image).polygon(arrow, fill='white', outline='black')


class Producer(threading.Thread):
    """Captures frames on its own thread, keeping only the newest, so the next
    frame is ready the moment the board has taken the last one."""

    def __init__(self, capture, fps, cursor):
        super().__init__(daemon=True)
        self.capture, self.cursor = capture, cursor
        self.interval = 1.0 / fps
        self.condition = threading.Condition()
        self.latest = None
        self.capture_ms = 0.0
        self.error = None

    def run(self):
        try:
            while True:
                started = time.monotonic()
                image = self.capture.grab()
                if self.cursor:
                    point = cursor_position()
                    point = point and self.capture.to_frame(point)
                    if point:
                        draw_cursor(image, point)
                with self.condition:
                    self.latest = image
                    self.condition.notify()
                elapsed = time.monotonic() - started
                self.capture_ms = self.capture_ms * 0.9 + elapsed * 100
                time.sleep(max(0.0, self.interval - elapsed))
        except Exception as error:  # surfaced by the main loop
            self.error = error
            with self.condition:
                self.condition.notify()

    def take(self, timeout):
        with self.condition:
            if self.latest is None and self.error is None:
                self.condition.wait(timeout)
            if self.error:
                raise self.error
            latest, self.latest = self.latest, None
            return latest


def changed_rect(before, after, threshold=0):
    """The block-aligned rectangle (x, y, width, height) holding every pixel
    where some colour channel differs by more than *threshold*, or None when
    none does."""
    difference = ImageChops.difference(before, after)
    if threshold:
        r, g, b = difference.split()
        difference = ImageChops.lighter(ImageChops.lighter(r, g), b).point(
            lambda v: 255 if v > threshold else 0)
    box = difference.getbbox()
    if not box:
        return None
    width, height = after.size
    x0 = box[0] // BLOCK * BLOCK
    y0 = box[1] // BLOCK * BLOCK
    x1 = min(width, -(-box[2] // BLOCK) * BLOCK)
    y1 = min(height, -(-box[3] // BLOCK) * BLOCK)
    return x0, y0, x1 - x0, y1 - y0


def encode_jpeg(image, quality):
    buffer = io.BytesIO()
    image.save(buffer, 'JPEG', quality=quality)
    return buffer.getvalue()


def encode_within(image, quality, budget):
    """JPEG at *quality*, or at the highest lower quality (in steps) that
    fits *budget* bytes. Returns (jpeg, quality used)."""
    jpeg = encode_jpeg(image, quality)
    while len(jpeg) > budget and quality > MIN_QUALITY:
        # Size falls roughly in proportion; step at least 5.
        quality = max(MIN_QUALITY, min(quality - 5, int(quality * budget / len(jpeg))))
        jpeg = encode_jpeg(image, quality)
    return jpeg, quality


class Streamer:
    """Decides what to send for each captured image: nothing, the changed
    rectangle, or a whole frame, and at what JPEG quality."""

    def __init__(self, board, size, rotation, quality, fps, always=False,
                 threshold=DEFAULT_THRESHOLD):
        self.board, self.size, self.rotation = board, size, rotation
        self.quality, self.always, self.threshold = quality, always, threshold
        self.full = (0, 0) + tuple(size)
        self.frame_s = 1.0 / max(1.0, fps)
        self.rate = 100_000.0  # Bytes per second the link carries (measured).
        self.soft = False      # The board shows a frame below self.quality.
        self.shown = None      # What the board shows, as far as we know.
        self.last_full = self.last_sent = 0.0
        self.failures = 0
        self.frames = self.patches = self.sent_bytes = 0
        self.send_s = 0.0

    def _choose(self, image, now):
        if self.shown is None or self.always or now - self.last_full >= FULL_REFRESH_S:
            return self.full
        rect = changed_rect(self.shown, image, self.threshold)
        if rect is None:
            idle = now - self.last_sent
            if idle >= KEEPALIVE_S or (self.soft and idle >= SHARPEN_AFTER_S):
                return self.full
            return None
        if rect[2] * rect[3] > PATCH_MAX_SHARE * self.size[0] * self.size[1]:
            return self.full
        return rect

    def _show(self, image, rect, still):
        part = image if rect == self.full else image.crop(
            (rect[0], rect[1], rect[0] + rect[2], rect[1] + rect[3]))
        # A still picture gets full quality; a moving one what fits one frame
        # time at the measured rate, less a margin for the round trips.
        budget = 1e9 if still else self.rate * self.frame_s * 0.85
        jpeg, used = encode_within(part, self.quality, budget)
        self.board.show_jpeg(jpeg, rect, self.size, self.rotation)
        return jpeg, used

    def send(self, image):
        """Sends what *image* needs. Returns False if nothing was sent."""
        now = time.monotonic()
        rect = self._choose(image, now)
        if rect is None:
            return False
        still = self.shown is not None and \
            changed_rect(self.shown, image, self.threshold) is None
        try:
            try:
                jpeg, used = self._show(image, rect, still)
            except BoardError as error:
                if error.code != NEEDS_FULL_FRAME or rect == self.full:
                    raise
                rect = self.full
                jpeg, used = self._show(image, rect, False)
        except (BoardError, TimeoutError) as error:
            self.shown = None  # Unknown now: start again from a whole frame.
            self.failures += 1
            print(f'\nFrame dropped: {error}', file=sys.stderr)
            if self.failures >= MAX_FAILURES:
                raise TimeoutError('the board keeps failing; stopping')
            if isinstance(error, TimeoutError):
                self.board.resync()
            return False
        elapsed = time.monotonic() - now
        if len(jpeg) > 2000:  # Small ones are mostly round trips.
            self.rate = self.rate * 0.8 + 0.2 * len(jpeg) / max(elapsed, 1e-3)
        if rect == self.full:
            self.soft = used < self.quality
            self.last_full = now
        else:
            self.soft = self.soft or used < self.quality
            self.patches += 1
        if rect == self.full:
            self.shown = image
        else:
            # Only the rectangle changed on the board. Changes below the
            # threshold elsewhere add up until they cross it.
            self.shown = self.shown.copy()
            self.shown.paste(image.crop((rect[0], rect[1], rect[0] + rect[2],
                                         rect[1] + rect[3])), rect[:2])
        self.last_sent = now
        self.failures = 0
        self.frames += 1
        self.sent_bytes += len(jpeg)
        self.send_s += elapsed
        return True

    def stats(self, seconds):
        """A status line for the last *seconds*, and resets the counters."""
        frames = max(1, self.frames)
        line = (f'{self.frames / seconds:5.1f} fps ({self.patches} of {self.frames} '
                f'partial)  send {1000 * self.send_s / frames:5.1f} ms  '
                f'{self.sent_bytes / frames / 1024:4.1f} KB/frame')
        self.frames = self.patches = self.sent_bytes = 0
        self.send_s = 0.0
        return line


def parse_region(text):
    try:
        x, y, w, h = (int(v) for v in text.split(','))
    except ValueError:
        raise argparse.ArgumentTypeError('use X,Y,W,H, e.g. 0,0,1280,680')
    if w <= 0 or h <= 0:
        raise argparse.ArgumentTypeError('width and height must be positive')
    return x, y, x + w, y + h


def main():
    parser = argparse.ArgumentParser(description='Mirror the PC screen onto the Cyberclip.')
    transport = parser.add_mutually_exclusive_group()
    transport.add_argument('--port', help='USB serial port (default: auto-detect)')
    transport.add_argument('--wifi', nargs='?', const=HOTSPOT_HOST, metavar='HOST',
                           help=f'stream over the board hotspot (default {HOTSPOT_HOST})')
    parser.add_argument('--list', action='store_true', help='list monitors and exit')
    parser.add_argument('--monitor', type=int, default=1)
    parser.add_argument('--region', type=parse_region)
    parser.add_argument('--fit', choices=('fit', 'fill', 'stretch'), default='fit')
    parser.add_argument('--fps', type=float, default=25)
    parser.add_argument('--quality', type=int, default=70)
    parser.add_argument('--rotate', type=int, choices=(1, 3), default=1)
    parser.add_argument('--no-cursor', action='store_true')
    parser.add_argument('--threshold', type=int, default=DEFAULT_THRESHOLD)
    parser.add_argument('--keep', action='store_true',
                        help='leave the last frame on screen when stopping')
    parser.add_argument('--duration', type=float, default=0, help=argparse.SUPPRESS)
    # Send every captured frame, changed or not (for measuring the frame rate).
    parser.add_argument('--always', action='store_true', help=argparse.SUPPRESS)
    args = parser.parse_args()

    windows_setup()
    monitors = list_monitors()
    if args.list:
        for number, rect in enumerate(monitors, 1):
            if rect is None:
                print(f'{number}: whole screen')
            else:
                l, t, r, b = rect
                print(f'{number}: {r - l}x{b - t} at {l},{t}' + (' (primary)' if number == 1 else ''))
        return
    if args.region:
        bbox = args.region
    elif 1 <= args.monitor <= len(monitors):
        bbox = monitors[args.monitor - 1]
    else:
        sys.exit(f'No monitor {args.monitor}; --list shows {len(monitors)}.')

    link = WebSocketLink(args.wifi) if args.wifi else SerialLink(args.port)
    try:
        board = Board(link)
    except (TimeoutError, BoardError, ConnectionError) as error:
        link.close()
        sys.exit(f'No answer from the board on {link.name}: {error}. '
                 'Close the web app connection first.')
    size = (board.height, board.width)  # landscape; the panel is 170x320 portrait
    capture = (GdiCapture if sys.platform == 'win32' else PilCapture)(bbox, size, args.fit)
    quality = max(10, min(95, args.quality))
    producer = Producer(capture, max(1.0, args.fps), not args.no_cursor)
    producer.start()
    print(f'Mirroring to Cyberclip {board.firmware} on {link.name}, {size[0]}x{size[1]}. '
          'Ctrl+C to stop.')

    streamer = Streamer(board, size, args.rotate, quality, args.fps, args.always,
                        max(0, min(255, args.threshold)))
    started_at = stats_at = time.monotonic()
    try:
        while not args.duration or time.monotonic() - started_at < args.duration:
            image = producer.take(timeout=1.0)
            if image is not None:
                streamer.send(image)
            now = time.monotonic()
            if now - stats_at >= 2:
                print(f'\r{streamer.stats(now - stats_at)}  '
                      f'capture {producer.capture_ms:4.1f} ms   ', end='', flush=True)
                stats_at = now
    except KeyboardInterrupt:
        pass
    except (TimeoutError, ConnectionError, OSError) as error:
        print(f'\nStopped: {error}', file=sys.stderr)
    finally:
        print()
        if not args.keep:
            try:
                board.request(PLAY_STORED)  # Back to the saved item, if any.
            except Exception:
                pass
        link.close()


if __name__ == '__main__':
    main()
