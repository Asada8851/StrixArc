# nullarc lockups: the mark plus a monoline wordmark drawn from strokes (no font dependency).
# Usage: python3 tools/brand/lockup.py assets/brand
import re, sys
out = sys.argv[1]
mark = open(f"{out}/nullarc-mark.svg").read()
defs = re.search(r"<defs>.*</defs>", mark, re.S).group(0)
body = mark[mark.index("</defs>") + 7: mark.index("</svg>")]

SW = 9          # stroke width
BASE, XH, ASC = 100, 60, 36   # baseline, x-height top, ascender top
def n(x):  return f"M{x} {BASE}V78A18 18 0 0 1 {x+36} 78V{BASE}", 36
def u(x):  return f"M{x} {XH}V82A18 18 0 0 0 {x+36} 82M{x+36} {XH}V{BASE}", 36
def l(x):  return f"M{x} {ASC}V88A12 12 0 0 0 {x+12} {BASE}", 12
def a(x):  return f"M{x+18} 62A18 18 0 1 0 {x+18} 98A18 18 0 1 0 {x+18} 62M{x+36} {XH}V{BASE}", 36
def r_(x): return f"M{x} {BASE}V78A18 18 0 0 1 {x+18} {XH}H{x+22}", 22
def c_(x): return f"M{x+29.57} 66.21A18 18 0 1 0 {x+29.57} 93.79", 30
GAP = 17
def word(glyphs, x):
    d = []
    for g in glyphs:
        p, w = g(x); d.append(p); x += w + GAP
    return " ".join(d), x - GAP
null_d, x_end = word([n, u, l, l], 0)
arc_x0 = x_end + GAP
arc_d, x_end = word([a, r_, c_], arc_x0)

TX, TY = 152, -10     # wordmark offset next to the 128 px mark
W = TX + x_end + SW
for name, ink in (("dark", "#F2F4F8"), ("light", "#0B0D12")):
    svg = f'''<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {W:.0f} 128" width="{W:.0f}" height="128" role="img" aria-label="nullarc">
  <title>nullarc</title>
  {defs.replace('id="na-', 'id="nl-').replace('url(#na-', 'url(#nl-')}
  <g transform="scale(0.5)">{body.replace('url(#na-', 'url(#nl-')}</g>
  <defs>
    <linearGradient id="nl-word" gradientUnits="userSpaceOnUse" x1="{arc_x0}" y1="0" x2="{x_end}" y2="0">
      <stop offset="0" stop-color="#FF3D2E"/>
      <stop offset="0.5" stop-color="#FF2D87"/>
      <stop offset="1" stop-color="#7C5CFF"/>
    </linearGradient>
  </defs>
  <g transform="translate({TX} {TY})" fill="none" stroke-width="{SW}" stroke-linecap="round" stroke-linejoin="round">
    <path d="{null_d}" stroke="{ink}"/>
    <path d="{arc_d}" stroke="url(#nl-word)"/>
  </g>
</svg>
'''
    open(f"{out}/nullarc-logo-{name}.svg", "w").write(svg)
print("width", W)
