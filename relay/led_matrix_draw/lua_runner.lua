-- Test-runs an animation script the way the ESP32 will, and renders preview frames.
--
-- Usage: lua5.4 lua_runner.lua FRAMES FPS < script.lua
-- Prints one JSON object: {"frames": "<hex>", "fps": n, "max_instructions": n, "memory_kb": n}
-- or {"error": "..."}.
--
-- The drawing API mirrors led_matrix_draw.ino exactly: colors are 0xRRGGBB integers,
-- coordinates are rounded to the nearest pixel, off-canvas writes are ignored.

local GRID = 16
local FRAME_BUDGET = 150000     -- VM instructions per draw(); the ESP32 allows 300000
local SETUP_BUDGET = 1500000    -- top-level code runs once; the ESP32 allows 3000000
local MEMORY_LIMIT_KB = 48      -- the ESP32 allows 64 KB

local nframes = tonumber(arg[1]) or 60
local default_fps = tonumber(arg[2]) or 30
local src = io.read("a")

local px = {}
local function clear()
  for i = 0, GRID * GRID - 1 do px[i] = 0 end
end

local function round(v) return math.floor(v + 0.5) end

local function color(c)
  if math.type(c) ~= "integer" then
    c = tonumber(c)
    if not c or c ~= math.floor(c) then error("color must be an integer from rgb()/hsv()", 4) end
    c = math.floor(c)
  end
  return c & 0xFFFFFF
end

local function put(x, y, c)
  if x >= 0 and x < GRID and y >= 0 and y < GRID then px[y * GRID + x] = c end
end

local api = {}

function api.rgb(r, g, b)
  local function ch(v) return math.max(0, math.min(255, math.floor(v))) end
  return (ch(r) << 16) | (ch(g) << 8) | ch(b)
end

-- FastLED hsv2rgb_spectrum equivalent: plain HSV with 8-bit steps
function api.hsv(h, s, v)
  s = math.max(0, math.min(1, s or 1)); v = math.max(0, math.min(1, v or 1))
  h = (h - math.floor(h)) * 6
  local i = math.floor(h)
  local f = h - i
  local p, q, t = v * (1 - s), v * (1 - s * f), v * (1 - s * (1 - f))
  local r, g, b
  if i == 0 then r, g, b = v, t, p elseif i == 1 then r, g, b = q, v, p
  elseif i == 2 then r, g, b = p, v, t elseif i == 3 then r, g, b = p, q, v
  elseif i == 4 then r, g, b = t, p, v else r, g, b = v, p, q end
  return api.rgb(r * 255, g * 255, b * 255)
end

function api.blend(a, b, t)
  a, b = color(a), color(b)
  t = math.max(0, math.min(1, t))
  local function mix(shift)
    local x, y = (a >> shift) & 255, (b >> shift) & 255
    return math.floor(x + (y - x) * t)
  end
  return (mix(16) << 16) | (mix(8) << 8) | mix(0)
end

function api.set(x, y, c) put(round(x), round(y), color(c)) end

function api.get(x, y)
  x, y = round(x), round(y)
  if x >= 0 and x < GRID and y >= 0 and y < GRID then return px[y * GRID + x] end
  return 0
end

function api.fill(c)
  c = color(c)
  for i = 0, GRID * GRID - 1 do px[i] = c end
end

function api.rect(x, y, w, h, c)
  c = color(c)
  x, y, w, h = round(x), round(y), round(w), round(h)
  for yy = y, y + h - 1 do for xx = x, x + w - 1 do put(xx, yy, c) end end
end

function api.line(x0, y0, x1, y1, c)
  c = color(c)
  local steps = math.floor(math.max(math.abs(x1 - x0), math.abs(y1 - y0))) + 1
  for k = 0, steps do
    local a = k / steps
    put(round(x0 + (x1 - x0) * a), round(y0 + (y1 - y0) * a), c)
  end
end

function api.circle(cx, cy, r, c, filled)
  c = color(c)
  if filled == nil then filled = true end
  for y = 0, GRID - 1 do
    for x = 0, GRID - 1 do
      local d = math.sqrt((x - cx) ^ 2 + (y - cy) ^ 2)
      if (filled and d <= r + 0.3) or (not filled and math.abs(d - r) <= 0.5) then put(x, y, c) end
    end
  end
end

-- sprite(rows, palette, x, y [, flip]): rows are strings, palette maps a character to a color;
-- characters missing from the palette (like '.' or ' ') are transparent
function api.sprite(rows, palette, x, y, flip)
  x, y = round(x or 0), round(y or 0)
  for r = 1, #rows do
    local row = rows[r]
    local w = #row
    for c = 1, w do
      local col = palette[row:sub(c, c)]
      if col then put(x + (flip and (w - c) or (c - 1)), y + r - 1, color(col)) end
    end
  end
end

-- Sandbox: same globals the ESP32 exposes (base + math + string + table, no file/code loading)
local env = {
  WIDTH = GRID, HEIGHT = GRID,
  math = math, string = string, table = table,
  assert = assert, error = error, ipairs = ipairs, next = next, pairs = pairs, pcall = pcall,
  select = select, tonumber = tonumber, tostring = tostring, type = type, xpcall = xpcall,
  rawequal = rawequal, rawget = rawget, rawlen = rawlen, rawset = rawset,
  setmetatable = setmetatable, getmetatable = getmetatable, print = function() end,
}
-- The ESP32's drawing calls are C and cost no VM instructions, so don't count ours either
local in_api = false
for k, f in pairs(api) do
  env[k] = function(...)
    in_api = true
    local r = f(...)
    in_api = false
    return r
  end
end
env._G = env

local instructions, budget, phase = 0, SETUP_BUDGET, "setup"
local function hook()
  if in_api then return end
  instructions = instructions + 1000
  if instructions > budget then
    error(string.format("too slow: %s used over %d instructions", phase, budget), 2)
  end
end

local function json_string(s)
  return '"' .. s:gsub('[%c"\\]', function(ch) return string.format("\\u%04x", ch:byte()) end) .. '"'
end

local function fail(msg)
  io.write('{"error": ' .. json_string(tostring(msg)) .. '}')
  os.exit(0)
end

-- Preview pixels go in a preallocated array so they don't count as script memory
local pixels = {}
for i = 1, nframes * GRID * GRID do pixels[i] = 0 end

collectgarbage()
local baseline_kb = collectgarbage("count")
local chunk, err = load(src, "=animation", "t", env)
if not chunk then fail(err) end

-- Count instructions only while the script's own code runs
local function run(f, ...)
  instructions, in_api = 0, false
  debug.sethook(hook, "", 1000)
  local ok, e = pcall(f, ...)
  debug.sethook()
  in_api = false
  return ok, e
end

local ok, e = run(chunk)
if not ok then fail(e) end
if type(env.draw) ~= "function" then fail("the script must define draw(t)") end

local fps = default_fps

local worst = 0
budget, phase = FRAME_BUDGET, "one draw() call"
for i = 0, nframes - 1 do
  clear()
  ok, e = run(env.draw, i / fps)
  if not ok then fail(e) end
  worst = math.max(worst, instructions)
  local base = i * GRID * GRID
  for p = 0, GRID * GRID - 1 do pixels[base + p + 1] = px[p] end
end
-- What the script keeps between frames (its tables and closures)
collectgarbage()
local script_kb = collectgarbage("count") - baseline_kb
if script_kb > MEMORY_LIMIT_KB then
  fail(string.format("uses too much memory: %.0f KB (limit %d KB)", script_kb, MEMORY_LIMIT_KB))
end

local hex = {}
for i = 1, #pixels do hex[i] = string.format("%06x", pixels[i]) end
io.write(string.format('{"frames": "%s", "fps": %d, "max_instructions": %d, "memory_kb": %.1f}',
  table.concat(hex), math.floor(fps), worst, script_kb))
