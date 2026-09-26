"""Runs model-written animation code in a locked-down subprocess.

Reads {"code": str, "frames": int} as JSON on stdin and writes {"frames": base64} or
{"error": str} as JSON on stdout. The parent runs this with no environment variables and
kills it on timeout; this script adds CPU/memory limits and rejects imports, dunder
access and format-string attribute tricks before executing anything.
"""

import ast
import base64
import json
import math
import random
import sys

try:
    import resource
    resource.setrlimit(resource.RLIMIT_CPU, (3, 3))
    resource.setrlimit(resource.RLIMIT_AS, (256 << 20, 256 << 20))
    resource.setrlimit(resource.RLIMIT_FSIZE, (0, 0))
except (ImportError, ValueError, OSError):
    pass  # macOS dev machines may reject some limits; the parent timeout still applies

GRID = 16
BLOCKED_ATTRS = {"format", "format_map", "mro", "gi_frame", "f_globals", "f_locals", "f_back"}
SAFE_BUILTINS = {
    name: __builtins__[name] if isinstance(__builtins__, dict) else getattr(__builtins__, name)
    for name in (
        "abs", "all", "any", "bool", "dict", "divmod", "enumerate", "filter", "float", "int",
        "isinstance", "len", "list", "map", "max", "min", "pow", "range", "reversed", "round",
        "set", "sorted", "sum", "tuple", "zip", "ValueError", "ZeroDivisionError", "Exception",
    )
}


def check(tree: ast.AST):
    for node in ast.walk(tree):
        if isinstance(node, (ast.Import, ast.ImportFrom, ast.Global, ast.Nonlocal)):
            raise ValueError("imports and global/nonlocal are not allowed")
        if isinstance(node, ast.Attribute) and (node.attr.startswith("_") or node.attr in BLOCKED_ATTRS):
            raise ValueError(f"attribute '{node.attr}' is not allowed")
        if isinstance(node, ast.Name) and node.id.startswith("__"):
            raise ValueError(f"name '{node.id}' is not allowed")


def clamp_color(c):
    try:
        r, g, b = c
        return (max(0, min(255, int(r))), max(0, min(255, int(g))), max(0, min(255, int(b))))
    except (TypeError, ValueError):
        return (0, 0, 0)


def hsv(h, s=1.0, v=1.0):
    h = (h % 1.0) * 6
    i = int(h)
    f = h - i
    p, q, t = v * (1 - s), v * (1 - s * f), v * (1 - s * (1 - f))
    r, g, b = [(v, t, p), (q, v, p), (p, v, t), (p, q, v), (t, p, v), (v, p, q)][i % 6]
    return (int(r * 255), int(g * 255), int(b * 255))


def blend(a, b, amount):
    amount = max(0.0, min(1.0, amount))
    return tuple(int(x + (y - x) * amount) for x, y in zip(clamp_color(a), clamp_color(b)))


class Canvas:
    def __init__(self):
        self.px = [[(0, 0, 0)] * GRID for _ in range(GRID)]

    def set(self, x, y, color):
        x, y = int(round(x)), int(round(y))
        if 0 <= x < GRID and 0 <= y < GRID:
            self.px[y][x] = clamp_color(color)

    def get(self, x, y):
        x, y = int(round(x)), int(round(y))
        return self.px[y][x] if 0 <= x < GRID and 0 <= y < GRID else (0, 0, 0)

    def fill(self, color):
        c = clamp_color(color)
        self.px = [[c] * GRID for _ in range(GRID)]

    def clear(self):
        self.fill((0, 0, 0))

    def rect(self, x, y, w, h, color):
        for yy in range(int(round(y)), int(round(y + h))):
            for xx in range(int(round(x)), int(round(x + w))):
                self.set(xx, yy, color)

    def line(self, x0, y0, x1, y1, color):
        steps = int(max(abs(x1 - x0), abs(y1 - y0))) + 1
        for k in range(steps + 1):
            a = k / steps
            self.set(x0 + (x1 - x0) * a, y0 + (y1 - y0) * a, color)

    def circle(self, cx, cy, r, color, filled=True):
        for y in range(GRID):
            for x in range(GRID):
                d = math.hypot(x - cx, y - cy)
                if (d <= r + 0.3) if filled else (abs(d - r) <= 0.5):
                    self.set(x, y, color)

    def to_bytes(self):
        return bytes(v for row in self.px for c in row for v in c)


def main():
    req = json.load(sys.stdin)
    frames = max(1, min(64, int(req.get("frames", 24))))
    try:
        tree = ast.parse(req["code"], "<animation>", "exec")
        check(tree)
        env = {"__builtins__": SAFE_BUILTINS, "math": math, "random": random.Random(1234),
               "hsv": hsv, "blend": blend, "WIDTH": GRID, "HEIGHT": GRID}
        exec(compile(tree, "<animation>", "exec"), env)
        draw = env.get("draw")
        if not callable(draw):
            raise ValueError("the code must define draw(canvas, t, i)")
        out = bytearray()
        for i in range(frames):
            canvas = Canvas()
            draw(canvas, i / frames, i)
            out += canvas.to_bytes()
        json.dump({"frames": base64.b64encode(bytes(out)).decode()}, sys.stdout)
    except Exception as e:
        json.dump({"error": f"{type(e).__name__}: {e}"}, sys.stdout)


if __name__ == "__main__":
    main()
