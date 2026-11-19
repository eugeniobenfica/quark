"""Draws docs/assets/hero.png: hero.qk beside what the terminal showed when it was run and built (the output is
pasted here as it came; run the commands again and update TERMINAL when either changes).
    python docs/assets/src/hero.py        (needs Pillow and the Consolas font)"""
import os, re
from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, '..', 'hero.png')
SOURCE = open(os.path.join(HERE, 'hero.qk'), encoding='utf-8').read().rstrip('\n').split('\n')
TERMINAL = [
    ('$ ', 'quark run orders.qk'),
    ('', 'skipped: not a number: "x"'),
    ('', 'total 37.50 [tea: 3, cake: 2]'),
    ('', ''),
    ('$ ', 'quark build orders.qk --time'),
    ('', 'quark: parsed and checked in 1.6 ms,'),
    ('', '  compiled in 0.8 ms'),
    ('', 'quark: wrote orders.exe (790 KB,'),
    ('', '  no C compiler, no runtime to install)'),
    ('', ''),
    ('$ ', './orders'),
    ('', 'skipped: not a number: "x"'),
    ('', 'total 37.50 [tea: 3, cake: 2]'),
]

S = 2  # drawn at twice the size: sharp on high-density screens
W, H = 820 * S, 470 * S
BG = (20, 20, 23)
PANEL = (28, 28, 33)
TEXT = (226, 226, 232)
DIM = (128, 128, 140)
ACCENT = (20, 184, 166)
STRING = (240, 196, 120)
NUMBER = (186, 160, 255)
TYPE = (125, 211, 252)

font = ImageFont.truetype('C:/Windows/Fonts/consola.ttf', 12 * S)
bold = ImageFont.truetype('C:/Windows/Fonts/consolab.ttf', 12 * S)
small = ImageFont.truetype('C:/Windows/Fonts/consola.ttf', 11 * S)
KEYWORDS = {'fn', 'let', 'var', 'for', 'in', 'if', 'else', 'match', 'return', 'type', 'use'}
TOKEN = re.compile(r'"(?:[^"\\]|\\.)*"|\d+\.\d+|\d+|[A-Za-z_][A-Za-z0-9_]*|\s+|.')

def code_line(d, x, y, line):
    for m in TOKEN.finditer(line):
        t = m.group(0)
        color, f = TEXT, font
        if t.startswith('"'):
            color = STRING
        elif t[0].isdigit():
            color = NUMBER
        elif t in KEYWORDS:
            color, f = ACCENT, bold
        elif t[0].isupper():
            color = TYPE
        d.text((x, y), t, font=f, fill=color)
        x += d.textlength(t, font=f)

img = Image.new('RGB', (W, H), BG)
d = ImageDraw.Draw(img)
pad = 16 * S
split = 505 * S
line_h = 16 * S
# the panels, each with its title
for (x0, x1, title) in [(pad, split - pad // 2, 'orders.qk'), (split + pad // 2, W - pad, 'terminal')]:
    d.rounded_rectangle((x0, pad, x1, H - pad), radius=10 * S, fill=PANEL)
    d.text((x0 + 14 * S, pad + 10 * S), title, font=small, fill=DIM)
y = pad + 34 * S
for line in SOURCE:
    code_line(d, pad + 14 * S, y, line)
    y += line_h
y = pad + 34 * S
for prompt, text in TERMINAL:
    x = split + pad // 2 + 14 * S
    if prompt:
        d.text((x, y), prompt, font=bold, fill=ACCENT)
        x += d.textlength(prompt, font=bold)
        d.text((x, y), text, font=bold, fill=TEXT)
    else:
        d.text((x, y), text, font=font, fill=DIM if text.startswith('quark:') or text.startswith('   ') else TEXT)
    y += line_h
img.save(OUT, optimize=True)
print('wrote', OUT, img.size)
