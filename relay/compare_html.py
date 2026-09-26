"""Like compare_models.py, but writes a color HTML comparison. Usage: compare_html.py OUT.html MODEL..."""
import sys, time, urllib.parse, urllib.request, html

RELAY = "http://192.168.1.178:8765"
PROMPTS = ["a red heart", "a cow", "a flower", "a rocket ship", "a smiley face", "a cat", "a flower in the wind", "a fish"]
out_path, models = sys.argv[1], sys.argv[2:]

def run(model, prompt):
    req = urllib.request.Request(f"{RELAY}/draw_text?model={urllib.parse.quote(model)}", data=prompt.encode(), method="POST")
    with urllib.request.urlopen(req, timeout=90) as r:
        return r.read(), float(r.headers.get("X-Draw-Seconds", "nan")), urllib.parse.unquote(r.headers.get("X-Title", ""))

def grid(body):
    cells = "".join(f'<i style="background:rgb({body[i*3]},{body[i*3+1]},{body[i*3+2]})"></i>' for i in range(256))
    return f'<div class="g">{cells}</div>'

times = {m: [] for m in models}
rows = ""
for p in PROMPTS:
    tds = ""
    for m in models:
        try:
            body, s, title = run(m, p)
            times[m].append(s)
            tds += f"<td>{grid(body)}<div class=c>{html.escape(title)} · {s:.1f}s</div></td>"
        except Exception as e:
            tds += f"<td>error: {html.escape(str(e))}</td>"
    rows += f"<tr><th>{html.escape(p)}</th>{tds}</tr>"
    print("done:", p, flush=True)

avg = "".join(f"<th>{html.escape(m)}<br><span class=c>{sum(t)/len(t):.2f}s avg</span></th>" for m, t in times.items() if t)
page = f"""<!doctype html><html><head><meta charset=utf-8><title>Model Comparison</title>
<meta name=viewport content="width=device-width,initial-scale=1">
<style>
:root{{--bg:#101012;--fg:#e8e8ea;--muted:#9a9aa2;--line:#2a2a30}}
@media (prefers-color-scheme: light){{:root:not([data-theme="dark"]){{--bg:#101012;--fg:#e8e8ea;--muted:#9a9aa2;--line:#2a2a30}}}}
body{{background:var(--bg);color:var(--fg);font-family:system-ui,sans-serif;margin:0;padding:16px}}
.wrap{{overflow-x:auto}} table{{border-collapse:collapse}} th,td{{padding:10px;border-bottom:1px solid var(--line);vertical-align:top;text-align:left}}
th{{font-weight:600}} .c{{color:var(--muted);font-size:13px;margin-top:6px;font-weight:400}}
.g{{display:grid;grid-template-columns:repeat(16,10px);gap:2px;background:#000;padding:6px;border-radius:6px;width:max-content}}
.g i{{width:10px;height:10px;border-radius:50%;display:block}}
</style></head><body><h1>16×16 drawing: model comparison</h1>
<p class=c>Same prompts, same system prompt. Time is the model call only (relay on the Pi).</p>
<div class=wrap><table><tr><th>Prompt</th>{avg}</tr>{rows}</table></div></body></html>"""
open(out_path, "w").write(page)
print("wrote", out_path)
