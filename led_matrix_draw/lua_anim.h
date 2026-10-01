// Runs Lua animation scripts (from the relay's code mode) live on the panel.
//
// Load a script from the main loop while the display task isn't drawing it, then call
// luaAnimDraw() from the display task each frame. Scripts are sandboxed: only math, string
// and table libraries, no file or code loading, capped memory and instructions per frame.
#pragma once

#include <Arduino.h>

// Replaces any running script. On failure, error describes why and nothing is running.
bool luaAnimLoad(const char *source, size_t length, String &error);

// Clears leds[] and draws the script's frame for time t (seconds since it started).
bool luaAnimDraw(float t, String &error);

// Frees the script and its memory.
void luaAnimClose();

size_t luaAnimMemoryUsed();
