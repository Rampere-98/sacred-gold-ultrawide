"""sheet.py <dump dir> <out.png> [start] [count]: contact sheet of dumped textures (index.txt order), on a checker."""
import os
import sys
from PIL import Image, ImageDraw

d, out = sys.argv[1], sys.argv[2]
start = int(sys.argv[3]) if len(sys.argv) > 3 else 0
count = int(sys.argv[4]) if len(sys.argv) > 4 else 48
lines = [l.split() for l in open(os.path.join(d, 'index.txt')) if l.strip()][start:start + count]
cell, cols = 160, 8
rows = (len(lines) + cols - 1) // cols
sheet = Image.new('RGB', (cols * cell, rows * (cell + 14)), (40, 40, 40))
dr = ImageDraw.Draw(sheet)
for i, (h, size, fmt) in enumerate(lines):
    try:
        im = Image.open(os.path.join(d, h + '.png')).convert('RGBA')
    except OSError:
        continue
    im.thumbnail((cell - 4, cell - 4))
    bg = Image.new('RGBA', im.size, (255, 0, 255, 255))
    bg.alpha_composite(im)
    x, y = (i % cols) * cell, (i // cols) * (cell + 14)
    sheet.paste(bg.convert('RGB'), (x + 2, y + 2))
    dr.text((x + 2, y + cell), f'{start + i} {size}', fill=(255, 255, 255))
sheet.save(out)
