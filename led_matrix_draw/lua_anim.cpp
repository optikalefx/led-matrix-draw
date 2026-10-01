#include "lua_anim.h"
#include "panel.h"
#include "src/lua/lua.hpp"

#define LUA_MEM_LIMIT      (64 * 1024)   // bytes a script may allocate
#define LUA_SETUP_BUDGET   3000000       // VM instructions for the script's top-level code
#define LUA_FRAME_BUDGET   300000        // VM instructions per draw(t)

static lua_State *L = nullptr;
static size_t luaBytes = 0;
static uint32_t instructions = 0;
static uint32_t budget = LUA_FRAME_BUDGET;

// ---------- sandbox ----------

static void *luaAlloc(void *, void *ptr, size_t osize, size_t nsize) {
  if (ptr == nullptr) osize = 0;   // osize is a type tag for new blocks
  if (nsize == 0) {
    free(ptr);
    luaBytes -= osize;
    return nullptr;
  }
  if (luaBytes - osize + nsize > LUA_MEM_LIMIT) return nullptr;   // Lua raises "not enough memory"
  void *p = realloc(ptr, nsize);
  if (p) luaBytes = luaBytes - osize + nsize;
  return p;
}

static void countHook(lua_State *L, lua_Debug *) {
  instructions += 1000;
  if (instructions > budget) luaL_error(L, "too slow");
}

// ---------- drawing API ----------

static int roundArg(lua_State *L, int i) {
  return (int)floorf((float)luaL_checknumber(L, i) + 0.5f);
}

static uint32_t colorArg(lua_State *L, int i) {
  return (uint32_t)luaL_checkinteger(L, i) & 0xFFFFFF;
}

static void put(int x, int y, uint32_t c) {
  setXY(x, y, CRGB(c));
}

static int l_rgb(lua_State *L) {
  int r = constrain((int)luaL_checknumber(L, 1), 0, 255);
  int g = constrain((int)luaL_checknumber(L, 2), 0, 255);
  int b = constrain((int)luaL_checknumber(L, 3), 0, 255);
  lua_pushinteger(L, (r << 16) | (g << 8) | b);
  return 1;
}

static int l_hsv(lua_State *L) {
  float h = (float)luaL_checknumber(L, 1);
  float s = constrain((float)luaL_optnumber(L, 2, 1), 0.0f, 1.0f);
  float v = constrain((float)luaL_optnumber(L, 3, 1), 0.0f, 1.0f);
  CRGB c;
  hsv2rgb_spectrum(CHSV((uint8_t)((h - floorf(h)) * 255), (uint8_t)(s * 255), (uint8_t)(v * 255)), c);
  lua_pushinteger(L, (c.r << 16) | (c.g << 8) | c.b);
  return 1;
}

static int l_blend(lua_State *L) {
  uint32_t a = colorArg(L, 1), b = colorArg(L, 2);
  float t = constrain((float)luaL_checknumber(L, 3), 0.0f, 1.0f);
  uint32_t out = 0;
  for (int shift = 16; shift >= 0; shift -= 8) {
    int x = (a >> shift) & 255, y = (b >> shift) & 255;
    out |= (uint32_t)(x + (y - x) * t) << shift;
  }
  lua_pushinteger(L, out);
  return 1;
}

static int l_set(lua_State *L) {
  put(roundArg(L, 1), roundArg(L, 2), colorArg(L, 3));
  return 0;
}

static int l_get(lua_State *L) {
  int x = roundArg(L, 1), y = roundArg(L, 2);
  uint32_t c = 0;
  if (x >= 0 && x < GRID && y >= 0 && y < GRID) {
    CRGB p = leds[xyToIndex(x, y)];
    c = (p.r << 16) | (p.g << 8) | p.b;
  }
  lua_pushinteger(L, c);
  return 1;
}

static int l_fill(lua_State *L) {
  fill_solid(leds, NUM_LEDS, CRGB(colorArg(L, 1)));
  return 0;
}

static int l_rect(lua_State *L) {
  int x = roundArg(L, 1), y = roundArg(L, 2), w = roundArg(L, 3), h = roundArg(L, 4);
  uint32_t c = colorArg(L, 5);
  for (int yy = max(y, 0); yy < min(y + h, GRID); yy++)
    for (int xx = max(x, 0); xx < min(x + w, GRID); xx++) put(xx, yy, c);
  return 0;
}

static int l_line(lua_State *L) {
  float x0 = luaL_checknumber(L, 1), y0 = luaL_checknumber(L, 2);
  float x1 = luaL_checknumber(L, 3), y1 = luaL_checknumber(L, 4);
  uint32_t c = colorArg(L, 5);
  int steps = min((int)fmaxf(fabsf(x1 - x0), fabsf(y1 - y0)) + 1, 64);
  for (int k = 0; k <= steps; k++) {
    float a = (float)k / steps;
    put((int)floorf(x0 + (x1 - x0) * a + 0.5f), (int)floorf(y0 + (y1 - y0) * a + 0.5f), c);
  }
  return 0;
}

static int l_circle(lua_State *L) {
  float cx = luaL_checknumber(L, 1), cy = luaL_checknumber(L, 2), r = luaL_checknumber(L, 3);
  uint32_t c = colorArg(L, 4);
  bool filled = lua_isnoneornil(L, 5) ? true : lua_toboolean(L, 5);
  for (int y = 0; y < GRID; y++)
    for (int x = 0; x < GRID; x++) {
      float d = hypotf(x - cx, y - cy);
      if (filled ? d <= r + 0.3f : fabsf(d - r) <= 0.5f) put(x, y, c);
    }
  return 0;
}

// sprite(rows, palette, x, y [, flip]): characters missing from the palette are transparent
static int l_sprite(lua_State *L) {
  luaL_checktype(L, 1, LUA_TTABLE);
  luaL_checktype(L, 2, LUA_TTABLE);
  int x = (int)floorf((float)luaL_optnumber(L, 3, 0) + 0.5f);
  int y = (int)floorf((float)luaL_optnumber(L, 4, 0) + 0.5f);
  bool flip = lua_toboolean(L, 5);

  int32_t colors[256];
  for (int i = 0; i < 256; i++) colors[i] = -1;
  lua_pushnil(L);
  while (lua_next(L, 2)) {
    size_t len;
    const char *key = lua_type(L, -2) == LUA_TSTRING ? lua_tolstring(L, -2, &len) : nullptr;
    if (key && len == 1 && lua_isinteger(L, -1)) colors[(uint8_t)key[0]] = lua_tointeger(L, -1) & 0xFFFFFF;
    lua_pop(L, 1);
  }

  lua_Integer rows = luaL_len(L, 1);
  for (lua_Integer r = 1; r <= rows && r <= GRID * 2; r++) {
    lua_geti(L, 1, r);
    size_t w;
    const char *row = lua_tolstring(L, -1, &w);
    if (row) {
      for (size_t i = 0; i < w; i++) {
        int32_t c = colors[(uint8_t)row[i]];
        if (c >= 0) put(x + (flip ? (int)(w - 1 - i) : (int)i), y + (int)r - 1, (uint32_t)c);
      }
    }
    lua_pop(L, 1);
  }
  return 0;
}

// ---------- lifecycle ----------

static bool check(int status, const char *what, String &error) {
  if (status == LUA_OK) return true;
  error = String(what) + ": " + (lua_tostring(L, -1) ? lua_tostring(L, -1) : "unknown error");
  lua_pop(L, 1);
  return false;
}

void luaAnimClose() {
  if (L) lua_close(L);
  L = nullptr;
  luaBytes = 0;
}

bool luaAnimLoad(const char *source, size_t length, String &error) {
  luaAnimClose();
  L = lua_newstate(luaAlloc, nullptr);
  if (!L) {
    error = "not enough memory for Lua";
    return false;
  }

  // Only pure-computation libraries: no io, os, package, debug or coroutine
  luaL_requiref(L, "_G", luaopen_base, 1);
  luaL_requiref(L, LUA_MATHLIBNAME, luaopen_math, 1);
  luaL_requiref(L, LUA_STRLIBNAME, luaopen_string, 1);
  luaL_requiref(L, LUA_TABLIBNAME, luaopen_table, 1);
  lua_settop(L, 0);
  for (const char *name : {"dofile", "loadfile", "load", "require", "collectgarbage"}) {
    lua_pushnil(L);
    lua_setglobal(L, name);
  }
  lua_register(L, "print", [](lua_State *) { return 0; });

  const luaL_Reg api[] = {
    {"rgb", l_rgb}, {"hsv", l_hsv}, {"blend", l_blend}, {"set", l_set}, {"get", l_get},
    {"fill", l_fill}, {"rect", l_rect}, {"line", l_line}, {"circle", l_circle},
    {"sprite", l_sprite}, {nullptr, nullptr},
  };
  for (const luaL_Reg *f = api; f->name; f++) lua_register(L, f->name, f->func);
  lua_pushinteger(L, GRID);
  lua_setglobal(L, "WIDTH");
  lua_pushinteger(L, GRID);
  lua_setglobal(L, "HEIGHT");
  lua_sethook(L, countHook, LUA_MASKCOUNT, 1000);

  // Text only ("t"): refuse precompiled bytecode
  instructions = 0;
  budget = LUA_SETUP_BUDGET;
  if (!check(luaL_loadbufferx(L, source, length, "=animation", "t"), "syntax", error) ||
      !check(lua_pcall(L, 0, 0, 0), "setup", error)) {
    luaAnimClose();
    return false;
  }

  lua_getglobal(L, "draw");
  bool hasDraw = lua_isfunction(L, -1);
  lua_pop(L, 1);
  if (!hasDraw) {
    error = "the script has no draw(t) function";
    luaAnimClose();
    return false;
  }
  return true;
}

bool luaAnimDraw(float t, String &error) {
  if (!L) {
    error = "no script loaded";
    return false;
  }
  FastLED.clear();
  instructions = 0;
  budget = LUA_FRAME_BUDGET;
  lua_getglobal(L, "draw");
  lua_pushnumber(L, t);
  return check(lua_pcall(L, 1, 0, 0), "draw", error);
}

size_t luaAnimMemoryUsed() {
  return luaBytes;
}
