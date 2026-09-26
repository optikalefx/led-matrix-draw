// Prototype: run a Lua animation script live on the 16x16 panel and report speed/memory.
// Lua 5.4.7 (official source, vendored in src/lua) is built with LUA_32BITS.

#include <FastLED.h>
#include "src/lua/lua.hpp"

#define LED_PIN     13
#define GRID        16
#define NUM_LEDS    (GRID * GRID)
#define BRIGHTNESS  40
#define MAX_MILLIAMPS 500

#define LUA_MEM_LIMIT   (64 * 1024)   // bytes the script may allocate
#define LUA_FRAME_BUDGET 300000       // VM instructions per draw() before we stop it

CRGB leds[NUM_LEDS];
lua_State *L = nullptr;
size_t luaBytes = 0;
uint32_t instrThisFrame = 0;

// ---------- panel ----------

uint16_t xyToIndex(int x, int y) {
  return x * GRID + ((x & 1) ? (GRID - 1 - y) : y);
}

void setXY(int x, int y, uint32_t c) {
  if (x >= 0 && x < GRID && y >= 0 && y < GRID) leds[xyToIndex(x, y)] = CRGB(c);
}

// ---------- Lua sandbox ----------

void *luaAlloc(void *, void *ptr, size_t osize, size_t nsize) {
  if (ptr == nullptr) osize = 0;
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

void countHook(lua_State *L, lua_Debug *) {
  instrThisFrame += 1000;
  if (instrThisFrame > LUA_FRAME_BUDGET) luaL_error(L, "draw() is too slow");
}

int roundArg(lua_State *L, int i) {
  return (int)floorf((float)luaL_checknumber(L, i) + 0.5f);
}

uint32_t colorArg(lua_State *L, int i) {
  return (uint32_t)luaL_checkinteger(L, i) & 0xFFFFFF;
}

int l_rgb(lua_State *L) {
  int r = constrain((int)luaL_checknumber(L, 1), 0, 255);
  int g = constrain((int)luaL_checknumber(L, 2), 0, 255);
  int b = constrain((int)luaL_checknumber(L, 3), 0, 255);
  lua_pushinteger(L, (r << 16) | (g << 8) | b);
  return 1;
}

int l_hsv(lua_State *L) {
  float h = (float)luaL_checknumber(L, 1);
  float s = (float)luaL_optnumber(L, 2, 1);
  float v = (float)luaL_optnumber(L, 3, 1);
  CHSV hsv((uint8_t)((h - floorf(h)) * 255), (uint8_t)(constrain(s, 0, 1) * 255),
           (uint8_t)(constrain(v, 0, 1) * 255));
  CRGB c;
  hsv2rgb_spectrum(hsv, c);
  lua_pushinteger(L, (c.r << 16) | (c.g << 8) | c.b);
  return 1;
}

int l_blend(lua_State *L) {
  uint32_t a = colorArg(L, 1), b = colorArg(L, 2);
  float t = constrain((float)luaL_checknumber(L, 3), 0.0f, 1.0f);
  int r = ((a >> 16) & 255) + (((int)((b >> 16) & 255) - (int)((a >> 16) & 255)) * t);
  int g = ((a >> 8) & 255) + (((int)((b >> 8) & 255) - (int)((a >> 8) & 255)) * t);
  int bl = (a & 255) + (((int)(b & 255) - (int)(a & 255)) * t);
  lua_pushinteger(L, (r << 16) | (g << 8) | bl);
  return 1;
}

int l_set(lua_State *L) {
  setXY(roundArg(L, 1), roundArg(L, 2), colorArg(L, 3));
  return 0;
}

int l_get(lua_State *L) {
  int x = roundArg(L, 1), y = roundArg(L, 2);
  uint32_t c = 0;
  if (x >= 0 && x < GRID && y >= 0 && y < GRID) {
    CRGB p = leds[xyToIndex(x, y)];
    c = (p.r << 16) | (p.g << 8) | p.b;
  }
  lua_pushinteger(L, c);
  return 1;
}

int l_fill(lua_State *L) {
  fill_solid(leds, NUM_LEDS, CRGB(colorArg(L, 1)));
  return 0;
}

int l_rect(lua_State *L) {
  int x = roundArg(L, 1), y = roundArg(L, 2), w = roundArg(L, 3), h = roundArg(L, 4);
  uint32_t c = colorArg(L, 5);
  for (int yy = y; yy < y + h; yy++)
    for (int xx = x; xx < x + w; xx++) setXY(xx, yy, c);
  return 0;
}

int l_line(lua_State *L) {
  float x0 = luaL_checknumber(L, 1), y0 = luaL_checknumber(L, 2);
  float x1 = luaL_checknumber(L, 3), y1 = luaL_checknumber(L, 4);
  uint32_t c = colorArg(L, 5);
  int steps = (int)fmaxf(fabsf(x1 - x0), fabsf(y1 - y0)) + 1;
  for (int k = 0; k <= steps; k++) {
    float a = (float)k / steps;
    setXY((int)floorf(x0 + (x1 - x0) * a + 0.5f), (int)floorf(y0 + (y1 - y0) * a + 0.5f), c);
  }
  return 0;
}

int l_circle(lua_State *L) {
  float cx = luaL_checknumber(L, 1), cy = luaL_checknumber(L, 2), r = luaL_checknumber(L, 3);
  uint32_t c = colorArg(L, 4);
  bool filled = lua_isnoneornil(L, 5) ? true : lua_toboolean(L, 5);
  for (int y = 0; y < GRID; y++)
    for (int x = 0; x < GRID; x++) {
      float d = hypotf(x - cx, y - cy);
      if (filled ? d <= r + 0.3f : fabsf(d - r) <= 0.5f) setXY(x, y, c);
    }
  return 0;
}

void openSandbox() {
  luaBytes = 0;
  L = lua_newstate(luaAlloc, nullptr);
  // Only pure-computation libraries: no io, os, package, debug or coroutine
  luaL_requiref(L, "_G", luaopen_base, 1);
  luaL_requiref(L, LUA_MATHLIBNAME, luaopen_math, 1);
  luaL_requiref(L, LUA_STRLIBNAME, luaopen_string, 1);
  luaL_requiref(L, LUA_TABLIBNAME, luaopen_table, 1);
  lua_settop(L, 0);
  // Base library functions that could load code from files or bytecode
  for (const char *name : {"dofile", "loadfile", "load", "require"}) {
    lua_pushnil(L);
    lua_setglobal(L, name);
  }
  const luaL_Reg api[] = {
    {"rgb", l_rgb}, {"hsv", l_hsv}, {"blend", l_blend}, {"set", l_set}, {"get", l_get},
    {"fill", l_fill}, {"rect", l_rect}, {"line", l_line}, {"circle", l_circle}, {nullptr, nullptr},
  };
  for (const luaL_Reg *f = api; f->name; f++) lua_register(L, f->name, f->func);
  lua_pushinteger(L, GRID);
  lua_setglobal(L, "WIDTH");
  lua_pushinteger(L, GRID);
  lua_setglobal(L, "HEIGHT");
  lua_sethook(L, countHook, LUA_MASKCOUNT, 1000);
}

// A deliberately busy demo: bouncing ball with a fading trail, plus a 40-star parallax field
const char *DEMO = R"LUA(
local stars = {}
for i = 1, 40 do
  stars[i] = { x = math.random() * 16, y = math.random() * 16, speed = 2 + math.random() * 8 }
end
local trail = {}

function draw(t)
  for _, s in ipairs(stars) do
    local x = (s.x + t * s.speed) % 16
    local b = math.floor(40 + s.speed * 18)
    set(x, s.y, rgb(b, b, b))
  end

  local bx = 7.5 + math.sin(t * 1.3) * 6
  local by = 13 - math.abs(math.sin(t * 3)) * 11
  table.insert(trail, 1, { bx, by })
  if #trail > 6 then table.remove(trail) end
  for i = #trail, 2, -1 do
    local p = trail[i]
    circle(p[1], p[2], 1.2, blend(hsv(t * 0.2), 0, i / 7))
  end
  circle(bx, by, 2, hsv(t * 0.2))
end
)LUA";

bool loadScript(const char *src) {
  instrThisFrame = 0;
  if (luaL_loadbufferx(L, src, strlen(src), "=animation", "t") != LUA_OK || lua_pcall(L, 0, 0, 0) != LUA_OK) {
    Serial.printf("script error: %s\n", lua_tostring(L, -1));
    lua_pop(L, 1);
    return false;
  }
  lua_getglobal(L, "draw");
  bool ok = lua_isfunction(L, -1);
  lua_pop(L, 1);
  if (!ok) Serial.println("script error: no draw(t) function");
  return ok;
}

bool drawFrame(float t) {
  instrThisFrame = 0;
  lua_getglobal(L, "draw");
  lua_pushnumber(L, t);
  if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
    Serial.printf("draw error: %s\n", lua_tostring(L, -1));
    lua_pop(L, 1);
    return false;
  }
  return true;
}

void setup() {
  Serial.begin(115200);
  FastLED.addLeds<WS2812B, LED_PIN, GRB>(leds, NUM_LEDS);
  FastLED.setBrightness(BRIGHTNESS);
  FastLED.setMaxPowerInVoltsAndMilliamps(5, MAX_MILLIAMPS);

  uint32_t heapBefore = ESP.getFreeHeap();
  openSandbox();
  bool ok = loadScript(DEMO);
  Serial.printf("lua ready=%d, script uses %u bytes, heap %u -> %u\n", ok, (unsigned)luaBytes,
                (unsigned)heapBefore, (unsigned)ESP.getFreeHeap());
}

void loop() {
  static uint32_t frames = 0, drawMicros = 0, worstMicros = 0, lastReport = 0;
  static uint32_t start = millis();

  FastLED.clear();
  uint32_t t0 = micros();
  drawFrame((millis() - start) / 1000.0f);
  uint32_t took = micros() - t0;
  FastLED.show();

  frames++;
  drawMicros += took;
  worstMicros = max(worstMicros, took);
  if (millis() - lastReport >= 2000) {
    lua_gc(L, LUA_GCCOLLECT, 0);
    Serial.printf("fps=%.1f draw avg=%uus worst=%uus lua mem=%u bytes heap free=%u\n",
                  frames * 1000.0f / (millis() - lastReport), (unsigned)(drawMicros / frames),
                  (unsigned)worstMicros, (unsigned)luaBytes, (unsigned)ESP.getFreeHeap());
    frames = drawMicros = worstMicros = 0;
    lastReport = millis();
  }
  delay(20);   // same ~50fps cadence as the main firmware
}
