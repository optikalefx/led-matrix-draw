"""Check which prompts animate, and write an animated HTML preview. Usage: animation_test.py OUT.html"""
import sys, json, html, urllib.parse, urllib.request

RELAY = "http://192.168.1.178:8765"
CASES = [  # (prompt, should animate)
    ("a cat", False), ("a cat running", True),
    ("a red heart", False), ("a beating heart", True),
    ("a rocket ship", False), ("a rocket ship blasting off", True),
    ("a bird", False), ("a flower in the wind", True),
    ("an animated star", True), ("a ball bouncing", True),
]
if len(sys.argv) > 2:  # custom prompts: animation_test.py OUT.html "prompt one" "prompt two"
    CASES = [(p, True) for p in sys.argv[2:]]

cards, ok = [], 0
for prompt, expect in CASES:
    req = urllib.request.Request(f"{RELAY}/draw_text", data=prompt.encode(), method="POST",
                                 headers={"X-Max-Frames": "30"})
    with urllib.request.urlopen(req, timeout=120) as r:
        body = r.read()
        n, ms = int(r.headers.get("X-Frames", "1")), int(r.headers.get("X-Frame-Ms", "250"))
        secs, title = float(r.headers["X-Draw-Seconds"]), urllib.parse.unquote(r.headers["X-Title"])
        mode = r.headers.get("X-Mode", "?")
    right = (n > 1) == expect
    ok += right
    print(f"{'OK  ' if right else 'MISS'} {prompt!r:32} mode={mode} frames={n} frame_ms={ms} {secs:.1f}s  ({title})")
    frames = [[f"rgb({f[i*3]},{f[i*3+1]},{f[i*3+2]})" for i in range(256)]
              for f in (body[k*768:(k+1)*768] for k in range(n))]
    cards.append({"prompt": prompt, "title": title, "n": n, "ms": ms, "secs": secs,
                  "expect": expect, "right": right, "frames": frames, "mode": mode})
print(f"\n{ok}/{len(CASES)} matched the still/animate rule")

page = """<!doctype html><html><head><meta charset=utf-8><title>Animation Test</title>
<meta name=viewport content="width=device-width,initial-scale=1">
<style>body{background:#101012;color:#e8e8ea;font-family:system-ui,sans-serif;margin:0;padding:16px}
.wrap{display:flex;flex-wrap:wrap;gap:16px}.card{background:#18181c;border:1px solid #2a2a30;border-radius:10px;padding:12px}
.g{display:grid;grid-template-columns:repeat(16,10px);gap:2px;background:#000;padding:6px;border-radius:6px;width:max-content}
.g i{width:10px;height:10px;border-radius:50%;display:block}.c{color:#9a9aa2;font-size:13px;margin-top:6px}
.miss{color:#ff6b6b}</style></head><body><h1>Still vs animation</h1><p class=c>SCORE</p><div class=wrap id=w></div>
<script>const D=DATA;const w=document.getElementById('w');
D.forEach(d=>{const c=document.createElement('div');c.className='card';
c.innerHTML=`<b>${d.prompt}</b><div class=c>${d.title} · ${d.mode} · ${d.n} frame${d.n>1?'s':''}${d.n>1?' @ '+d.ms+'ms':''} · ${d.secs.toFixed(1)}s</div>
<div class=c ${d.right?'':'style="color:#ff6b6b"'}>expected ${d.expect?'animation':'still'}${d.right?' ✓':' ✗'}</div>`;
const g=document.createElement('div');g.className='g';const cells=[...Array(256)].map(()=>g.appendChild(document.createElement('i')));
c.insertBefore(g,c.children[1]);w.appendChild(c);let k=0;const show=()=>{d.frames[k%d.frames.length].forEach((col,i)=>cells[i].style.background=col);k++};
show();if(d.n>1)setInterval(show,d.ms);});</script></body></html>"""
open(sys.argv[1], "w").write(page.replace("DATA", json.dumps(cards)).replace("SCORE", f"{ok}/{len(CASES)} matched the still/animate rule"))
