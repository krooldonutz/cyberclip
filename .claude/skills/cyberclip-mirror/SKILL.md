---
name: cyberclip-mirror
description: Mirror the PC screen (a monitor or a region) live onto the Cyberclip board over USB or its WiFi hotspot with scripts/mirror.py - start, stop, pick a monitor or region, tune frame rate and quality, and troubleshoot. Use when the user asks to mirror, stream or cast their screen to the clip, use it as a tiny second monitor, or asks why the mirror is slow, blurry or stuck.
---

# Cyberclip screen mirror

`scripts/mirror.py` is a hidden developer feature (not in the web app). It captures the screen, shrinks it to the 320x170 panel and streams it with the `MIRROR_FRAME` command; nothing is saved on the board. How it works is in ARCHITECTURE.md ("PC screen mirror").

## Running it

Needs Pillow and pyserial in the Python that runs it (`python -m pip install --user pillow pyserial`; already installed for the user's `python`). Run it in the user's Terminal panel (`run_in_terminal`), not Bash: it runs until Ctrl+C and they should be able to see and stop it.

```bash
python scripts/mirror.py                      # USB, primary monitor, letterboxed
python scripts/mirror.py --list               # monitors and their numbers
python scripts/mirror.py --monitor 2
python scripts/mirror.py --region 0,0,1280,680   # X,Y,W,H in desktop pixels
python scripts/mirror.py --fit fill           # crop to fill (or stretch)
python scripts/mirror.py --rotate 3           # clip mounted upside down
python scripts/mirror.py --wifi               # over the hotspot (see below)
```

Other options: `--fps 25` (target; quality drops while the picture moves too much to hold it, and a sharp frame follows 0.3 s after it stops), `--quality 70` (still-picture JPEG quality), `--threshold 6` (colour change a pixel needs to count as changed; 0 sends every difference), `--no-cursor`, `--keep` (leave the last frame on screen when stopping; otherwise Ctrl+C brings the saved item back).

The status line shows updates per second, how many were partial (changed rectangle only), send time, capture time and KB per update. A still screen sends almost nothing, so a low number there is normal; send time is the real headroom.

To stop it: Ctrl+C in that tab, or `stop_terminal_tab`.

## WiFi

The board never joins a home network. The user holds BOOT to turn the hotspot on, joins it from the PC with the password in the join QR code, then runs `--wifi` (192.168.4.1). Only one WiFi client at a time: close the phone page first. WiFi mode has not been measured on the real board yet; say so if asked about its speed.

## Rules

- Only one program can hold the COM port. While the mirror runs, `scripts/device.py` and the web app cannot connect over USB; stop the mirror first.
- Never send the board a second request before it has answered the first. Over USB that crashes it, and five crashes in a row leave it in ROM download mode (see the cyberclip-device skill to recover). mirror.py never pipelines; keep it that way in any change.
- Needs firmware with `MIRROR_FRAME` (0x14). On older firmware the script falls back to the slow frame commands (whole frames only, around 6 fps); flash with the cyberclip-device skill.
- The display can't be seen from here. Ask the user to judge sharpness and colours.

## Measured on the board (USB, 2560x1440 monitor)

Cursor or typing about 39 updates/s, a 160x90 video window about 35, scrolling half the screen about 30, full-screen motion about 28 (at lowered quality). USB carries about 120-160 KB/s and a whole-frame decode takes about 25 ms at 240 MHz.

## Troubleshooting

- "No Cyberclip found over USB": the board may be on battery with USB off; press a button on it, or check `python scripts/device.py ports`.
- "No answer from the board": something else holds the port (web app, serial monitor, another mirror), or the board is in download mode (PID 1001 in `device.py ports`); use the cyberclip-device skill.
- "Frame dropped: ... error 12": the board could not patch its screen (an overlay or saved item took over); the script sends a whole frame next, nothing to do.
- Repeated drops or "the board keeps failing": stop, read `python scripts/device.py status` (`reset_reason`, `crashes_in_a_row`, `usb_rx_dropped`) and report the numbers.
