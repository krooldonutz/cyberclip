#pragma once

#include <stdint.h>

// Crash-loop guard. The product has no reachable BOOT or RST button, so
// firmware that crashes on every boot could never be reinstalled. The count
// of crashes in a row lives in RTC memory that survives a software reset:
//
//   - a few crashes in a row start the firmware in safe mode (no saved media,
//     no WiFi), which keeps USB working so the web installer can reach it;
//   - if safe mode crashes too, the board restarts into ROM download mode,
//     where the web installer flashes it directly.
//
// A boot that did not follow a crash (power on, deep-sleep wake, the
// installer's restarts) and a boot that stays up for kBootStableMs both
// clear the count.
namespace cyberclip {

constexpr uint32_t kBootGuardMagic = 0xC1B0076Du;
constexpr uint32_t kCrashesBeforeSafeMode = 3;
constexpr uint32_t kCrashesBeforeDownloadMode = 5;
constexpr uint32_t kBootStableMs = 15000;

struct BootGuardState {
  uint32_t magic;
  uint32_t crashes;
};

enum class BootAction : uint8_t { kNormal, kSafeMode, kDownloadMode };

// *crashed* is true when the last reset was a panic or a watchdog.
inline BootAction bootGuardOnBoot(BootGuardState &state, bool crashed) {
  // RTC memory holds garbage after power is first applied.
  if (state.magic != kBootGuardMagic) {
    state.magic = kBootGuardMagic;
    state.crashes = 0;
  }
  if (!crashed) {
    state.crashes = 0;
    return BootAction::kNormal;
  }
  state.crashes += 1;
  if (state.crashes >= kCrashesBeforeDownloadMode) {
    // Start over once new firmware is installed.
    state.crashes = 0;
    return BootAction::kDownloadMode;
  }
  if (state.crashes >= kCrashesBeforeSafeMode) return BootAction::kSafeMode;
  return BootAction::kNormal;
}

inline void bootGuardMarkStable(BootGuardState &state) { state.crashes = 0; }

}  // namespace cyberclip
