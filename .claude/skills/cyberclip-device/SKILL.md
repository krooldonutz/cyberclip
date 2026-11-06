---
name: cyberclip-device
description: Talk to the Cyberclip board plugged in over USB - read firmware version, battery voltage, power source, CPU clock, heap, uptime and hotspot state, set the backlight, log status over time, and flash newly built firmware. Use when testing a firmware change on the real device, checking battery/power behaviour, or when the user asks what the board is doing.
---

# Cyberclip device access

All device access goes through `scripts/device.py`, run with PlatformIO's Python (it has pyserial):

```bash
PY=~/.platformio/penv/Scripts/python.exe
$PY scripts/device.py ports        # find the board (Espressif VID 303A, usually COM8)
$PY scripts/device.py hello        # firmware version, display, storage
$PY scripts/device.py status       # battery_mv, on_battery, cpu_mhz, free_heap, uptime_s, backlight
$PY scripts/device.py hotspot      # hotspot setting, running, IP, SSID
$PY scripts/device.py backlight 64 # 0-255
$PY scripts/device.py watch --interval 10 --duration 300 --csv <scratchpad>/power.csv
$PY scripts/device.py flash        # build + flash lilygo_t_display_s3 over USB
```

`--port COMx` overrides auto-detection. Only one program can hold the port: close the web app's connection (or any serial monitor) first, or the script fails to open it.

## Testing a firmware change on the board

1. Run the PC checks first (see CLAUDE.md): `pio test -e native`, then `pio run -e lilygo_t_display_s3`.
2. `hello` to record the version currently on the board.
3. `flash`. It reboots the board into ROM download mode first (1200-baud open, like the web installer), because esptool's normal DTR/RTS reset can't handle this firmware's USB mode: the port vanishes mid-connect. Then it does a full install (bootloader, partitions, app), so new boards work too. Stored media and hotspot settings survive when the partition layout already matches. Run it in the background; it can take a minute or two. The COM number changes after flashing (e.g. COM10 in download mode, COM11 afterwards); re-run `ports`.
4. `hello` again to confirm the new version is running, then `status` / `hotspot` to check behaviour.
5. Report what you saw, with the numbers.

Flash when the user asked you to test or install a change. Ask first if they only asked a question, or if the board might be in use by them.

## When the board won't flash or vanishes

- Download mode shows up as VID 303A **PID 1001** ("USB JTAG/serial debug unit"); the running firmware is PID 0002.
- If Windows shows "Unknown USB Device (Device Descriptor Request Failed)" and no 303A port, the board is wedged. Ask the user to unplug USB **and any battery**, hold BOOT while plugging USB back in, then release BOOT. `flash` waits 2 minutes for that when no board is found, so start it first, in the background.
- Don't run `pio run -t upload` against the running firmware's port (PID 0002); that is what wedges it.

## What this can and can't tell you

- `status` fields after `free_heap` (battery, power source, CPU clock, uptime) need firmware 2.0.25+. Older firmware says so in a `note` line.
- WiFi turns fully off after 60 s with no phone or app connected over WiFi, on USB too, so `hotspot` often reads `running False`; that is expected.
- While USB is plugged in the board is **not** on battery: `on_battery` is false, the voltage reads the USB/charger rail, and the CPU stays at 160 MHz. So real battery drain and the battery-only power savings (80 MHz clock, 20 ms idle sleep, WiFi turning off after 2 minutes without a command) cannot be measured over USB. Say this plainly rather than presenting USB readings as battery results.
- For a real battery-life check, the user has to run the board unplugged and note how long it lasts (or plug in afterwards and read `uptime_s` before it sleeps). Offer that as a manual step.
- The display can't be seen from here. Ask the user to look at the screen when the change is visual.

## Don't

- Don't add commands that clear stored media (`CLEAR_STORED`) or change the hotspot setting, without the user asking for that specifically.
- Don't leave `watch` running in the foreground for long; use `--duration`, or run it in the background.
