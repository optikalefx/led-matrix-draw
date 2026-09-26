# LED Matrix Draw

HTTP relay for the ESP32 16x16 LED matrix.

- `POST /draw` - body is raw 8-bit mu-law audio (header `X-Sample-Rate`, default 16000)
  or a WAV file (`Content-Type: audio/wav`). Transcribes it, then draws it.
- `POST /draw_text` - body is plain text to draw (skips speech to text).
- `GET /` - health check and the last drawing.

Responses to the draw endpoints are 768 bytes: 16 rows x 16 columns x RGB, row-major,
top-left first. The transcript is in the `X-Transcript` header (URL-encoded).
