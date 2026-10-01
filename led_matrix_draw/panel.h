// The 16x16 WS2812B panel, shared by the main sketch and the Lua animation engine.
#pragma once

#include <FastLED.h>

#define GRID        16
#define NUM_LEDS    (GRID * GRID)

extern CRGB leds[NUM_LEDS];

// Panel is column-serpentine: LED 0 top-left, first 16 run down the left column,
// the next 16 run back up the second column, and so on.
// The panel is mounted upside down, so logical coordinates are rotated 180°.
#define PANEL_ROTATE_180 1

inline uint16_t xyToIndex(int x, int y) {
#if PANEL_ROTATE_180
  x = GRID - 1 - x;
  y = GRID - 1 - y;
#endif
  return x * GRID + ((x & 1) ? (GRID - 1 - y) : y);
}

inline void setXY(int x, int y, CRGB c) {
  if (x >= 0 && x < GRID && y >= 0 && y < GRID) leds[xyToIndex(x, y)] = c;
}
