// The 16x16 WS2812B panel, shared by the main sketch and the Lua animation engine.
#pragma once

#include <FastLED.h>

#define GRID        16
#define NUM_LEDS    (GRID * GRID)

extern CRGB leds[NUM_LEDS];

// Panel is column-serpentine: LED 0 top-left, first 16 run down the left column,
// the next 16 run back up the second column, and so on.
inline uint16_t xyToIndex(int x, int y) {
  return x * GRID + ((x & 1) ? (GRID - 1 - y) : y);
}

inline void setXY(int x, int y, CRGB c) {
  if (x >= 0 && x < GRID && y >= 0 && y < GRID) leds[xyToIndex(x, y)] = c;
}
