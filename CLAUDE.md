# Cyberclip

## Verifying changes

Run all of these before reporting a change as done:

- Web app: `npm test` (Vitest).
- Firmware unit tests: `cd firmware && ~/.platformio/penv/Scripts/pio.exe test -e native`.
  Needs `g++` on PATH (WinLibs, installed via winget). A shell started before the
  install may not see it; refresh PATH from the user/machine environment first.
- Firmware compile check: `cd firmware && ~/.platformio/penv/Scripts/pio.exe run -e lilygo_t_display_s3`.
  Run the board build after `pio test`, since the native test run can clear `.pio/build`.
- Screen-mirror build, when firmware or the mirror changed: `cd firmware && ~/.platformio/penv/Scripts/pio.exe run -e lilygo_t_display_s3_mirror`.

Nothing here flashes the board; on-device behavior (battery life, display) still needs a manual check.

## Build flags

- `CYBERCLIP_BLE_MEDIA` (firmware/platformio.ini) is 0: Bluetooth and the iPhone
  now-playing view are compiled out to save battery. It is build-time only, with no runtime switch.

- `CYBERCLIP_SCREEN_MIRROR` is 1 only in the `lilygo_t_display_s3_mirror` env: the PC
  screen mirror (`MIRROR_FRAME`, `scripts/mirror.py`) is left out of released firmware.
- `CYBERCLIP_TEST_HOTSPOT_PASSWORD` (developer builds only, passed through
  `PLATFORMIO_BUILD_FLAGS`, never committed) replaces the hotspot password with a fixed one
  so a PC can join for `mirror.py --wifi`.

## The real board

The board is usually plugged in over USB (COM8). Use the `cyberclip-device` skill
(`.claude/skills/cyberclip-device/SKILL.md`, driving `scripts/device.py`) to read its status or flash it.
