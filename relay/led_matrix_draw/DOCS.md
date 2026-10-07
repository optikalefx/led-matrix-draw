# LED Matrix Draw

HTTP relay for the ESP32 16x16 LED matrix: speech to text (OpenAI), then Claude draws.

## Endpoints

- `POST /draw` - body is raw 8-bit mu-law audio (header `X-Sample-Rate`, default 16000)
  or a WAV file (`Content-Type: audio/wav`). Transcribes it, then draws it.
- `POST /draw_text` - body is plain text to draw (skips speech to text). Add `?preview=1`
  to get rendered frames even for Lua animations.
- `GET /` - the last drawing (animated), its code if any, the last recording, and every saved
  drawing with a Delete button.
- `DELETE /cache/{key}` - deletes one saved drawing.
- `POST /cache/{key}/send` - the page's Send button: tells the device to show a saved drawing.
- `GET /cache/{key}/device` - a saved drawing in the same format `/draw` returns; the device
  fetches it after a Send.
- `POST /hello` - body is the device's IP; the device says hello at boot so Send can reach it.

The page also opens from the Home Assistant sidebar ("LED Matrix").
- `GET /last.wav` - the last recording.

## What comes back

Every response has `X-Transcript`, `X-Title` (URL-encoded) and `X-Mode`:

- `still` / `frames` - body is `X-Frames` frames of 768 bytes each (16 rows x 16 columns x RGB,
  row-major, top-left first), shown `X-Frame-Ms` apart. Claude animates (up to 4 hand-drawn
  frames) only when the request has an action word ("a cat running") or asks for an animation.
- `lua` - body is a Lua 5.4 script defining `draw(t)`, which the ESP32 runs live. Requests that
  start with "code ..." or "write code ..." take this path, and so do requests that mention
  the button, pressing, tapping, clicking or a game. The relay test-runs the script
  with `lua_runner.lua` first (same drawing API and limits as the device) and asks Claude to fix
  it once if it fails. Scripts that ask for the button define `press(t)` / `release(t)` and can
  read `button()`; the preview simulates one press at 1s so those handlers get tested too.

## Saved drawings

Every result is saved in `/data/cache` under its phrase, so saying the same thing again shows
the saved one instantly without asking Claude (`X-Cached: 1`). Phrases are matched loosely:
"draw me a red heart!" and "a red heart" are the same, and "code a ball" and "write code for a
ball" are too. Start with "another", "a different" or "redraw" ("another cat") to draw it fresh
and replace the saved one. Changing `claude_model` starts over with fresh drawings.

Send on a saved drawing shows it on the device right away: the relay POSTs its key to the
device's `/show` (port 80), and the device fetches it from `/cache/{key}/device`.

## Button on the device

- Nothing showing: hold to talk, release to send.
- A picture showing: presses go to its Lua script (if it has handlers); holding for 0.8s clears
  the picture, and the button goes back to push-to-talk.

## Options

- `openai_api_key`, `stt_model` - speech to text
- `anthropic_api_key`, `claude_model` - drawing and Lua animations
