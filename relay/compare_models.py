"""Send the same prompts to the relay with different models and print timing + ASCII previews.

Usage: python3 compare_models.py MODEL [MODEL ...]
"""
import sys
import time
import urllib.parse
import urllib.request

RELAY = "http://192.168.1.178:8765"
PROMPTS = ["a red heart", "a cow", "a flower", "a rocket ship", "a smiley face", "a cat"]


def shade(r, g, b):
    if r + g + b == 0:
        return "  "
    # ANSI truecolor block so the preview shows real colors in the terminal
    return f"\x1b[38;2;{r};{g};{b}m██\x1b[0m"


def run(model, prompt):
    req = urllib.request.Request(
        f"{RELAY}/draw_text?model={urllib.parse.quote(model)}", data=prompt.encode(), method="POST")
    t0 = time.monotonic()
    with urllib.request.urlopen(req, timeout=90) as resp:
        body = resp.read()
        draw_s = float(resp.headers.get("X-Draw-Seconds", "nan"))
        title = urllib.parse.unquote(resp.headers.get("X-Title", ""))
    return body, draw_s, time.monotonic() - t0, title


models = sys.argv[1:]
results = {m: [] for m in models}
for prompt in PROMPTS:
    grids = []
    for m in models:
        try:
            body, draw_s, total_s, title = run(m, prompt)
            results[m].append(draw_s)
            grids.append((f"{m.split('-2025')[0]}: {title} ({draw_s:.1f}s)", body))
        except Exception as e:
            grids.append((f"{m}: ERROR {e}", b"\x00" * 768))
    print(f"\n=== {prompt} ===")
    print("    ".join(label[:32].ljust(32) for label, _ in grids))
    for y in range(16):
        print("    ".join(
            "".join(shade(*body[(y * 16 + x) * 3:(y * 16 + x) * 3 + 3]) for x in range(16))
            for _, body in grids))

print("\n=== average model time ===")
for m, times in results.items():
    if times:
        print(f"{m}: {sum(times) / len(times):.2f}s avg, {min(times):.2f}-{max(times):.2f}s over {len(times)} prompts")
