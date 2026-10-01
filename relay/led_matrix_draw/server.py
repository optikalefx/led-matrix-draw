"""HTTP relay: ESP32 audio -> OpenAI speech to text -> Claude -> 16x16 RGB pixels."""

import asyncio
import io
import json
import logging
import os
import re
import struct
import sys
import time
import urllib.parse
import wave

from html import escape as html_escape

import aiohttp
from aiohttp import web

GRID = 16
PORT = 8765
OPTIONS_PATH = "/data/options.json"
LUA_RUNNER = os.path.join(os.path.dirname(os.path.abspath(__file__)), "lua_runner.lua")
LUA_BIN = os.environ.get("LUA_BIN", "lua5.4")
PREVIEW_SECONDS = 4   # how much of a Lua animation the preview page shows
PREVIEW_FPS = 30

logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
log = logging.getLogger("led_matrix_draw")


def load_options():
    if os.path.exists(OPTIONS_PATH):
        with open(OPTIONS_PATH) as f:
            return json.load(f)
    # Local development: fall back to environment variables
    return {
        "openai_api_key": os.environ.get("OPENAI_API_KEY", ""),
        "anthropic_api_key": os.environ.get("ANTHROPIC_API_KEY", ""),
        "stt_model": os.environ.get("STT_MODEL", "gpt-4o-mini-transcribe"),
        "claude_model": os.environ.get("CLAUDE_MODEL", "claude-sonnet-5"),
    }


OPTIONS = load_options()


# ---------- audio ----------

def _mulaw_table():
    table = []
    for byte in range(256):
        u = ~byte & 0xFF
        sign = u & 0x80
        exponent = (u >> 4) & 0x07
        mantissa = u & 0x0F
        sample = ((mantissa << 3) + 0x84) << exponent
        sample -= 0x84
        table.append(-sample if sign else sample)
    return table


MULAW = _mulaw_table()


def mulaw_to_wav(data: bytes, sample_rate: int) -> bytes:
    pcm = struct.pack(f"<{len(data)}h", *(MULAW[b] for b in data))
    buf = io.BytesIO()
    with wave.open(buf, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(sample_rate)
        w.writeframes(pcm)
    return buf.getvalue()


# ---------- AI calls ----------

async def transcribe(session: aiohttp.ClientSession, wav: bytes) -> str:
    form = aiohttp.FormData()
    form.add_field("file", wav, filename="speech.wav", content_type="audio/wav")
    form.add_field("model", OPTIONS["stt_model"])
    form.add_field("response_format", "json")
    # Auto language detection guesses wrong on short, noisy clips
    form.add_field("language", "en")
    form.add_field("prompt", "A short spoken request for a picture to draw, like 'a red heart', "
                             "'a cow', or 'a rocket ship'.")
    async with session.post(
        "https://api.openai.com/v1/audio/transcriptions",
        headers={"Authorization": f"Bearer {OPTIONS['openai_api_key']}"},
        data=form,
    ) as resp:
        body = await resp.json(content_type=None)
        if resp.status != 200:
            raise web.HTTPBadGateway(text=f"speech to text failed: {body}")
        return body.get("text", "").strip()


MAX_FRAMES = 4

DRAW_TOOL = {
    "name": "draw_pixel_art",
    "description": "Draw 16x16 pixel art for an RGB LED matrix: one frame for a still image, "
                   "or 2-4 frames for a looping animation.",
    "input_schema": {
        "type": "object",
        "properties": {
            "title": {"type": "string", "description": "Short name of what was drawn."},
            "palette": {
                "type": "object",
                "description": "Map of single-character keys to hex colors like '#ff8800'. "
                "Use '.' for off/black; do not include '.' in the palette. Shared by all frames.",
                "additionalProperties": {"type": "string"},
            },
            "frames": {
                "type": "array",
                "description": "1 frame for a still image, 2-4 frames for an animation. Each frame is "
                "exactly 16 strings of exactly 16 characters, top row first. Each character is a "
                "palette key or '.' for an unlit pixel.",
                "minItems": 1,
                "maxItems": MAX_FRAMES,
                "items": {"type": "array", "items": {"type": "string"}},
            },
            "frame_ms": {
                "type": "integer",
                "description": "How long each frame shows, in milliseconds. Ignored for a still image.",
            },
        },
        "required": ["title", "palette", "frames"],
    },
}

SYSTEM_PROMPT = """You are a pixel artist drawing on a 16x16 RGB LED matrix.
The matrix sits on a black background, so unlit pixels ('.') are black and read as empty space.

Guidelines:
- Make the subject large and centered; use most of the 16x16 area.
- Bold, simple, recognizable silhouettes read best at this size. Avoid fine detail.
- Use saturated colors. LEDs make dark colors (below ~#303030) look almost off, so avoid them except as deliberate shading.
- Use a small palette (2-8 colors) with single-character keys.
- If asked for text or a word, draw the most iconic image of it rather than letters, unless it is short (1-3 characters).

Still image or animation:
- Default to a STILL IMAGE: exactly 1 frame.
- Make an ANIMATION only when the request contains a verb or action word describing motion or change
  (running, flying, spinning, jumping, waving, falling, burning, blowing, swimming, dancing, blinking,
  "in the wind", "blasting off"), or when the user explicitly asks for an animation ("animated",
  "animation", "moving", "make it move").
- Nouns and adjectives alone are never animated, even if the thing usually moves in real life.
  "a cat", "a red heart", "a rocket ship", "a bird", "a big wave" -> 1 frame.
  "a cat running", "a beating heart", "a rocket ship blasting off", "a bird flying",
  "a flower in the wind", "an animated star" -> animation.

Animation rules:
- Use 4 frames for smooth motion (2-3 only if the motion is a simple toggle, like blinking).
- The frames play in a loop, so the last frame must lead naturally back into the first.
- Keep the composition, size and palette the same across frames; change only the parts that move.
- Prefer small movements of 1-2 pixels per frame; big jumps look like flicker at this size.
- frame_ms: usually 150-300 for lively motion, 300-500 for gentle motion like swaying or breathing.

Always call the draw_pixel_art tool exactly once."""


async def call_claude(session: aiohttp.ClientSession, model: str, system: str, tool: dict,
                      messages: list) -> dict:
    """Asks for one call of `tool` and returns that tool_use block.

    Newer models (Sonnet 5.5, Opus 5.5) reject a forced tool_choice, so the
    system prompt does the forcing and tool_choice stays on auto."""
    payload = {
        "model": model,
        "max_tokens": 16000,
        "system": system,
        "tools": [tool],
        "tool_choice": {"type": "auto", "disable_parallel_tool_use": True},
        "messages": messages,
    }
    async with session.post(
        "https://api.anthropic.com/v1/messages",
        headers={
            "x-api-key": OPTIONS["anthropic_api_key"],
            "anthropic-version": "2023-06-01",
            "content-type": "application/json",
        },
        json=payload,
    ) as resp:
        body = await resp.json(content_type=None)
        if resp.status != 200:
            raise web.HTTPBadGateway(text=f"Claude request failed: {body}")
    for block in body.get("content", []):
        if block.get("type") == "tool_use":
            return block
    raise web.HTTPBadGateway(text="Claude did not call the tool")


async def draw(session: aiohttp.ClientSession, prompt: str) -> dict:
    block = await call_claude(session, OPTIONS["claude_model"], SYSTEM_PROMPT, DRAW_TOOL,
                              [{"role": "user", "content": f"Draw: {prompt}"}])
    return block["input"]


# ---------- code mode: "code a bouncing ball", "write code for fireworks" ----------

CODE_TRIGGER = re.compile(
    r"^\W*(?:please\s+)?(?:(?:can|could)\s+you\s+)?"
    r"(?:write\s+(?:some\s+|me\s+)?code\s+(?:for|to|that)|code(?:\s+up)?)\b[\s,:.-]*",
    re.IGNORECASE)


def code_request(text: str):
    """The request without its trigger phrase, or None when this isn't a code request."""
    m = CODE_TRIGGER.match(text)
    if not m:
        return None
    rest = text[m.end():].strip(" .!?")
    rest = re.sub(r"^(?:draws?|makes?|shows?|animates?)\s+", "", rest, flags=re.IGNORECASE)
    return rest or None


CODE_TOOL = {
    "name": "write_animation_code",
    "description": "Write a Lua 5.4 script with a draw(t) function that animates a 16x16 RGB LED "
                   "matrix live on the device.",
    "input_schema": {
        "type": "object",
        "properties": {
            "title": {"type": "string", "description": "Short name of the animation."},
            "code": {"type": "string", "description": "Lua source defining draw(t)."},
        },
        "required": ["title", "code"],
    },
}

CODE_PROMPT = """You write short Lua 5.4 scripts that animate a 16x16 RGB LED matrix. The script runs
live on a small microcontroller (ESP32, 240 MHz), forever, until the user asks for something else.

Define:

    function draw(t)

- It is called about 30 times per second on a canvas that has been cleared to black. t is seconds
  since the animation started (a float). Drive all motion from t so speed doesn't depend on frame
  rate. It never has to loop: animations can evolve forever (use t, math.sin, math.random).
- Top-level code runs once: precompute there. State that changes between frames (particles,
  positions) lives in top-level locals; update it in draw(t) using dt = t - previous t.

Drawing (x is 0-15 left to right, y is 0-15 top to bottom; coordinates are rounded to the nearest
pixel; anything off the canvas is ignored):
- set(x, y, color), get(x, y), fill(color), rect(x, y, w, h, color),
  line(x0, y0, x1, y1, color), circle(cx, cy, r, color [, filled=true])
- sprite(rows, palette, x, y [, flip]): rows is a table of equal-length strings, palette maps a
  character to a color; characters not in the palette (use '.') are transparent. flip mirrors it.
- Colors are integers 0xRRGGBB. rgb(r, g, b) with 0-255 returns a color; hsv(h, s, v) with all
  values 0-1 returns a color; blend(a, b, amount) mixes two colors. Never pass strings or tables
  as colors.

Environment:
- Available: math, string, table, pairs, ipairs, select, tonumber, tostring, type, pcall, error.
- Not available: io, os, require, load, dofile, coroutine, debug. print does nothing.
- Numbers are 32-bit on the device: fine for animation math, but don't use integers above
  2 billion or rely on high float precision.
- Stay small and fast: under ~40 KB of data (a few hundred numbers; prefer flat arrays over many
  small tables) and a few hundred drawing calls per frame at most.

Characters and objects vs effects:
- For a character or object (cat, bird, person, rocket, heart...), hand-draw it as pixel-art
  sprite(s) with sprite() and a palette, as big as the scene allows. Animate by moving, bobbing or
  flipping it, and for walking, flapping or blinking switch between 2-4 sprite poses based on t.
- Use shapes, particles, gradients and math for effects: fire, rain, sparkles, stars, water,
  smoke, trails, explosions. Combine both when it helps (a rocket sprite with a particle exhaust).

Make it look good on LEDs: bold, saturated colors on black. Dark colors (below ~40 per channel)
look off. Big, simple shapes; this is only 16x16.

Always call write_animation_code exactly once."""


def _limit_resources():
    import resource
    resource.setrlimit(resource.RLIMIT_CPU, (5, 5))
    resource.setrlimit(resource.RLIMIT_AS, (256 << 20, 256 << 20))


async def run_lua(code: str) -> dict:
    """Test-runs a script like the device will; returns preview frames or raises ValueError."""
    frames = PREVIEW_SECONDS * PREVIEW_FPS
    proc = await asyncio.create_subprocess_exec(
        LUA_BIN, LUA_RUNNER, str(frames), str(PREVIEW_FPS),
        stdin=asyncio.subprocess.PIPE, stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.PIPE,
        env={}, preexec_fn=_limit_resources if sys.platform == "linux" else None)
    try:
        out, err = await asyncio.wait_for(proc.communicate(code.encode()), timeout=10)
    except asyncio.TimeoutError:
        proc.kill()
        raise ValueError("the script took too long to run")
    try:
        result = json.loads(out or b"{}")
    except json.JSONDecodeError:
        result = {}
    if "frames" not in result:
        raise ValueError(result.get("error") or f"the script crashed: {err.decode()[-300:]}")
    data = bytes.fromhex(result["frames"])
    size = GRID * GRID * 3
    return {"frames": [data[k:k + size] for k in range(0, len(data), size)], "fps": result["fps"],
            "memory_kb": result["memory_kb"], "max_instructions": result["max_instructions"]}


async def code_animation(session: aiohttp.ClientSession, request_text: str) -> dict:
    """Asks Claude for a Lua script and test-runs it; retries once with the error if it fails."""
    messages = [{"role": "user", "content": f"Animate: {request_text}"}]
    last_error = None
    for attempt in range(2):
        block = await call_claude(session, OPTIONS["claude_model"], CODE_PROMPT, CODE_TOOL, messages)
        code = block["input"].get("code", "")
        try:
            run = await run_lua(code)
            log.info("lua script ok: %d bytes, %.1f KB memory, %d instructions/frame",
                     len(code), run["memory_kb"], run["max_instructions"])
            return {"title": block["input"].get("title", ""), "frames": run["frames"], "code": code,
                    "frame_ms": round(1000 / run["fps"]), "mode": "lua"}
        except ValueError as e:
            last_error = e
            log.warning("lua attempt %d failed: %s", attempt + 1, e)
            messages += [
                {"role": "assistant", "content": [block]},
                {"role": "user", "content": [{"type": "tool_result", "tool_use_id": block["id"],
                                              "is_error": True,
                                              "content": f"{e}. Fix the script and call the tool again."}]},
            ]
    raise web.HTTPBadGateway(text=f"animation script failed twice: {last_error}")


async def create(session: aiohttp.ClientSession, text: str) -> dict:
    """Turns a request into {title, frames (768 bytes each), frame_ms, mode, code}.

    For mode "lua" the device gets the script itself; frames are only the relay's preview."""
    code_text = code_request(text)
    if code_text:
        return await code_animation(session, code_text)
    drawing = await draw(session, text)
    frames = to_frames(drawing)
    return {"title": drawing.get("title", ""), "frames": frames, "frame_ms": frame_ms(drawing),
            "mode": "frames" if len(frames) > 1 else "still", "code": None}


def hex_to_rgb(value: str) -> tuple:
    value = value.strip().lstrip("#")
    if len(value) == 3:
        value = "".join(c * 2 for c in value)
    try:
        return tuple(int(value[i:i + 2], 16) for i in (0, 2, 4))
    except ValueError:
        return (0, 0, 0)


def frame_list(drawing: dict) -> list:
    frames = drawing.get("frames") or []
    return [f for f in frames if isinstance(f, list)][:MAX_FRAMES] or [[]]


def frame_ms(drawing: dict) -> int:
    try:
        return max(80, min(2000, int(drawing.get("frame_ms") or 250)))
    except (TypeError, ValueError):
        return 250


def frame_to_pixels(rows: list, palette: dict) -> bytes:
    rows = list(rows)[:GRID]
    rows += [""] * (GRID - len(rows))
    out = bytearray()
    for row in rows:
        row = str(row)[:GRID].ljust(GRID, ".")
        for ch in row:
            out.extend(palette.get(ch, (0, 0, 0)))
    return bytes(out)


def to_frames(drawing: dict) -> list:
    """Each frame as 768 bytes: 16x16 RGB, row-major, top-left first."""
    palette = {k: hex_to_rgb(v) for k, v in drawing.get("palette", {}).items() if len(k) == 1}
    return [frame_to_pixels(rows, palette) for rows in frame_list(drawing)]


# ---------- HTTP handlers ----------

LAST = {"transcript": None, "result": None, "at": None, "wav": None}


def pixel_response(transcript: str, result: dict, draw_seconds: float,
                   preview: bool = False) -> web.Response:
    LAST.update(transcript=transcript, result=result, at=time.strftime("%Y-%m-%d %H:%M:%S"))
    headers = {
        "X-Transcript": urllib.parse.quote(transcript),
        "X-Title": urllib.parse.quote(result["title"]),
        "X-Mode": result["mode"],
        "X-Draw-Seconds": f"{draw_seconds:.2f}",
    }
    if result["mode"] == "lua" and not preview:
        # The device runs the script itself
        return web.Response(body=result["code"].encode(), content_type="text/x-lua", headers=headers)
    frames = result["frames"]
    headers["X-Frames"] = str(len(frames))
    headers["X-Frame-Ms"] = str(result["frame_ms"])
    return web.Response(body=b"".join(frames), content_type="application/octet-stream", headers=headers)


async def handle_draw(request: web.Request) -> web.Response:
    started = time.monotonic()
    data = await request.read()
    if not data:
        raise web.HTTPBadRequest(text="empty body")

    rate = int(request.headers.get("X-Sample-Rate", "16000"))
    if request.content_type in ("audio/wav", "audio/x-wav"):
        wav = data
    else:
        wav = mulaw_to_wav(data, rate)

    LAST["wav"] = wav
    session = request.app["http"]
    transcript = await transcribe(session, wav)
    stt_done = time.monotonic()
    log.info("heard (%.1fs audio, %.1fs): %r", len(data) / rate, stt_done - started, transcript)
    if not transcript:
        raise web.HTTPUnprocessableEntity(text="no speech detected")

    result = await create(session, transcript)
    draw_seconds = time.monotonic() - stt_done
    log.info("drew %r (%s, %d frames) in %.1fs (total %.1fs)", result["title"], result["mode"],
             len(result["frames"]), draw_seconds, time.monotonic() - started)
    return pixel_response(transcript, result, draw_seconds)


async def handle_draw_text(request: web.Request) -> web.Response:
    started = time.monotonic()
    text = (await request.text()).strip()
    if not text:
        raise web.HTTPBadRequest(text="empty body")
    result = await create(request.app["http"], text)
    draw_seconds = time.monotonic() - started
    log.info("drew %r (%s, %d frames) for %r in %.1fs", result["title"], result["mode"],
             len(result["frames"]), text, draw_seconds)
    # ?preview=1 returns the relay's rendered frames even for Lua scripts (for test tools)
    return pixel_response(text, result, draw_seconds, preview=bool(request.query.get("preview")))


async def handle_last_wav(request: web.Request) -> web.Response:
    if not LAST["wav"]:
        raise web.HTTPNotFound(text="no recording yet")
    return web.Response(body=LAST["wav"], content_type="audio/wav")


async def handle_index(request: web.Request) -> web.Response:
    frames_js = "[]"
    delay = 250
    title = ""
    code_html = ""
    result = LAST["result"]
    if result:
        frames = result["frames"]
        frames_js = json.dumps([[f"rgb({f[i*3]},{f[i*3+1]},{f[i*3+2]})" for i in range(GRID * GRID)]
                                for f in frames])
        delay = result["frame_ms"]
        title = f"{result['title']} ({result['mode']}, {len(frames)} frame{'s' if len(frames) != 1 else ''})"
        if result.get("code"):
            code_html = f"<h2>Code</h2><pre>{html_escape(result['code'])}</pre>"
    html = f"""<!doctype html><html><head><meta charset="utf-8"><title>LED Matrix Draw</title>
<style>body{{background:#111;color:#ddd;font-family:system-ui;padding:16px}}
pre{{background:#1b1b1f;padding:12px;border-radius:8px;overflow-x:auto;font-size:13px}}
.grid{{display:grid;grid-template-columns:repeat(16,18px);gap:2px}}.grid div{{width:18px;height:18px;border-radius:50%}}</style>
</head><body><h1>LED Matrix Draw</h1>
<p>Last prompt: {LAST['transcript'] or '(none yet)'} {('at ' + LAST['at']) if LAST['at'] else ''}</p>
<p>{title}</p>
<div class="grid" id="grid"></div>
{'<p><audio controls src="/last.wav"></audio></p>' if LAST['wav'] else ''}
{code_html}
<script>
const frames = {frames_js}, grid = document.getElementById("grid");
const cells = Array.from({{length: 256}}, () => grid.appendChild(document.createElement("div")));
let n = 0;
function show() {{ if (!frames.length) return; frames[n % frames.length].forEach((c, i) => cells[i].style.background = c); n++; }}
show(); if (frames.length > 1) setInterval(show, {delay});
</script></body></html>"""
    return web.Response(text=html, content_type="text/html")


async def on_startup(app: web.Application):
    app["http"] = aiohttp.ClientSession(timeout=aiohttp.ClientTimeout(total=60))
    missing = [k for k in ("openai_api_key", "anthropic_api_key") if not OPTIONS.get(k)]
    if missing:
        log.warning("missing options: %s - set them on the add-on Configuration tab", ", ".join(missing))
    log.info("listening on :%d (stt=%s, claude=%s)", PORT, OPTIONS["stt_model"], OPTIONS["claude_model"])


async def on_cleanup(app: web.Application):
    await app["http"].close()


def main():
    app = web.Application(client_max_size=2 * 1024 * 1024)
    app.router.add_get("/", handle_index)
    app.router.add_get("/last.wav", handle_last_wav)
    app.router.add_post("/draw", handle_draw)
    app.router.add_post("/draw_text", handle_draw_text)
    app.on_startup.append(on_startup)
    app.on_cleanup.append(on_cleanup)
    web.run_app(app, port=PORT, access_log=None)


if __name__ == "__main__":
    main()
