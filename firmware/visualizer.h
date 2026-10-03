#pragma once

#include <math.h>
#include <stdint.h>

// Animated wave for the now-playing screen. AMS does not provide audio
// levels, so the bars follow a sum of a sine and a cosine wave whose speeds,
// spatial frequencies, and phases are randomized for each song.
namespace cyberclip {
namespace visualizer {

constexpr float kTwoPi = 6.28318530718f;

struct WaveParams {
  float speed1 = 3.0f;    // radians per second
  float speed2 = 5.0f;
  float cycles1 = 1.0f;   // wave cycles across the bars
  float cycles2 = 2.0f;
  float phase1 = 0.0f;    // radians
  float phase2 = 0.0f;
};

// Maps a random 32-bit value onto [low, high).
inline float randomRange(uint32_t random, float low, float high) {
  return low + (high - low) * (random / 4294967296.0f);
}

// Builds wave parameters from six random values (e.g. esp_random()).
inline WaveParams randomWaveParams(const uint32_t random[6]) {
  WaveParams params;
  params.speed1 = randomRange(random[0], 2.0f, 5.0f);
  params.speed2 = randomRange(random[1], 3.0f, 7.0f);
  params.cycles1 = randomRange(random[2], 0.5f, 2.0f);
  params.cycles2 = randomRange(random[3], 1.0f, 3.0f);
  params.phase1 = randomRange(random[4], 0.0f, kTwoPi);
  params.phase2 = randomRange(random[5], 0.0f, kTwoPi);
  return params;
}

// Bar height as a fraction in [0.1, 1.0] at time *seconds* for the bar at
// *position* in [0, 1] across the wave.
inline float waveLevel(const WaveParams &params, float seconds,
                       float position) {
  const float a = sinf(params.speed1 * seconds +
                       params.cycles1 * kTwoPi * position + params.phase1);
  const float b = cosf(params.speed2 * seconds +
                       params.cycles2 * kTwoPi * position + params.phase2);
  return 0.55f + 0.3f * a + 0.15f * b;
}

// HSV (hue in degrees, saturation and value in [0, 1]) to RGB565.
inline uint16_t hsvToRgb565(float hue, float saturation, float value) {
  hue = fmodf(hue, 360.0f);
  if (hue < 0) hue += 360.0f;
  const float chroma = value * saturation;
  const float x = chroma * (1 - fabsf(fmodf(hue / 60.0f, 2.0f) - 1));
  const float m = value - chroma;
  float r = 0, g = 0, b = 0;
  if (hue < 60) {
    r = chroma; g = x;
  } else if (hue < 120) {
    r = x; g = chroma;
  } else if (hue < 180) {
    g = chroma; b = x;
  } else if (hue < 240) {
    g = x; b = chroma;
  } else if (hue < 300) {
    r = x; b = chroma;
  } else {
    r = chroma; b = x;
  }
  const uint16_t red = static_cast<uint16_t>((r + m) * 31.0f + 0.5f);
  const uint16_t green = static_cast<uint16_t>((g + m) * 63.0f + 0.5f);
  const uint16_t blue = static_cast<uint16_t>((b + m) * 31.0f + 0.5f);
  return static_cast<uint16_t>((red << 11) | (green << 5) | blue);
}

// A bright color that stays readable on black, from two random values.
inline uint16_t randomSongColor(uint32_t hueRandom, uint32_t saturationRandom) {
  return hsvToRgb565(randomRange(hueRandom, 0.0f, 360.0f),
                     randomRange(saturationRandom, 0.55f, 0.9f), 1.0f);
}

}  // namespace visualizer
}  // namespace cyberclip
