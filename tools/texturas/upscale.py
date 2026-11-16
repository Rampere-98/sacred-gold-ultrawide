"""Upscale dumped Sacred textures with Real-ESRGAN for [Textures] Replace.

upscale.py <dump dir> <out dir> <esrgan dir> [--scale 2] [--list hashes.txt] [--min 16] [--max 512]

- Opaque textures (ground tiles, most world art) are padded by wrapping around before upscaling, so a tile's
  edges still match its neighbours after the network has seen them.
- Textures with alpha: the colour of transparent texels is filled from nearby opaque ones (no dark halos), the
  colour and the alpha are upscaled separately (alpha as a grey image through the same network), then joined.
- The x4 result is resized to --scale with Lanczos (2x keeps memory moderate in the 32-bit game).
"""
import argparse
import os
import shutil
import subprocess
import sys
from PIL import Image, ImageFilter

ap = argparse.ArgumentParser()
ap.add_argument('dump')
ap.add_argument('out')
ap.add_argument('esrgan')
ap.add_argument('--scale', type=int, default=2)
ap.add_argument('--list')
ap.add_argument('--min', type=int, default=16)
ap.add_argument('--max', type=int, default=512)
ap.add_argument('--model', default='realesrgan-x4plus')
a = ap.parse_args()

work = os.path.join(a.out, '_work')
src_dir, res_dir = os.path.join(work, 'in'), os.path.join(work, 'out')
shutil.rmtree(work, ignore_errors=True)
os.makedirs(src_dir)
os.makedirs(res_dir)

names = sorted(f for f in os.listdir(a.dump) if f.endswith('.png') and len(f) == 20)
if a.list:
    wanted = {l.strip()[:16] for l in open(a.list) if l.strip()}
    names = [n for n in names if n[:16] in wanted]


def pad_of(w, h):
    return max(2, min(16, min(w, h) // 8))


def wrap_pad(im, p):
    w, h = im.size
    out = Image.new(im.mode, (w + 2 * p, h + 2 * p))
    for dy in (-1, 0, 1):
        for dx in (-1, 0, 1):
            out.paste(im, (p + dx * w, p + dy * h))
    return out


def edge_pad(im, p):
    w, h = im.size
    out = Image.new(im.mode, (w + 2 * p, h + 2 * p))
    out.paste(im.resize((w + 2 * p, h + 2 * p), Image.NEAREST), (0, 0))   # rough fill, then exact centre
    out.paste(im, (p, p))
    # Replicate edge rows/columns exactly.
    for i in range(p):
        out.paste(im.crop((0, 0, w, 1)), (p, i))
        out.paste(im.crop((0, h - 1, w, h)), (p, p + h + i))
    for i in range(p):
        out.paste(out.crop((p, 0, p + 1, h + 2 * p)), (i, 0))
        out.paste(out.crop((p + w - 1, 0, p + w, h + 2 * p)), (p + w + i, 0))
    return out


def bleed(rgba):
    """RGB where transparent texels take the colour of nearby opaque ones."""
    r, g, b, al = rgba.split()
    rgb = Image.merge('RGB', (r, g, b))
    base = rgb.copy()
    known = al.point(lambda v: 255 if v > 8 else 0)
    cur, mask = rgb, known
    for radius in (1, 2, 4, 8, 16):
        # Average of known neighbours: blur(colour*mask) / blur(mask).
        prem = Image.composite(cur, Image.new('RGB', cur.size), mask)
        bp = prem.filter(ImageFilter.BoxBlur(radius))
        bm = mask.filter(ImageFilter.BoxBlur(radius))
        pp, mp = bp.load(), bm.load()
        filled = cur.copy()
        fp = filled.load()
        mk = mask.load()
        w, h = cur.size
        newmask = mask.copy()
        nm = newmask.load()
        for y in range(h):
            for x in range(w):
                if not mk[x, y] and mp[x, y] > 0:
                    m = mp[x, y]
                    c = pp[x, y]
                    fp[x, y] = (min(255, c[0] * 255 // m), min(255, c[1] * 255 // m), min(255, c[2] * 255 // m))
                    nm[x, y] = 255
        cur, mask = filled, newmask
    return Image.composite(base, cur, known)


jobs = []
for n in names:
    im = Image.open(os.path.join(a.dump, n)).convert('RGBA')
    w, h = im.size
    if min(w, h) < a.min or max(w, h) > a.max:
        continue
    lo, hi = im.getchannel('A').getextrema()
    opaque = lo == 255
    p = pad_of(w, h)
    stem = n[:16]
    if opaque:
        wrap_pad(im.convert('RGB'), p).save(os.path.join(src_dir, stem + '_c.png'))
    else:
        edge_pad(bleed(im), p).save(os.path.join(src_dir, stem + '_c.png'))
        edge_pad(im.getchannel('A'), p).convert('RGB').save(os.path.join(src_dir, stem + '_a.png'))
    jobs.append((stem, w, h, p, opaque))

print(f'{len(jobs)} textures to upscale', flush=True)
exe = os.path.join(a.esrgan, 'realesrgan-ncnn-vulkan.exe')
r = subprocess.run([exe, '-i', src_dir, '-o', res_dir, '-n', a.model, '-m', os.path.join(a.esrgan, 'models'),
                    '-f', 'png'], capture_output=True, text=True)
if r.returncode != 0:
    print(r.stderr[-2000:])
    sys.exit(1)

done = 0
for stem, w, h, p, opaque in jobs:
    c = Image.open(os.path.join(res_dir, stem + '_c.png')).convert('RGB')
    k = c.size[0] // (w + 2 * p)
    c = c.crop((p * k, p * k, (p + w) * k, (p + h) * k)).resize((w * a.scale, h * a.scale), Image.LANCZOS)
    if opaque:
        al = Image.new('L', c.size, 255)
    else:
        al = Image.open(os.path.join(res_dir, stem + '_a.png')).convert('L')
        al = al.crop((p * k, p * k, (p + w) * k, (p + h) * k)).resize(c.size, Image.LANCZOS)
    out = c.convert('RGBA')
    out.putalpha(al)
    out.save(os.path.join(a.out, stem + '.png'))
    done += 1
shutil.rmtree(work, ignore_errors=True)
print(f'{done} written to {a.out}')
