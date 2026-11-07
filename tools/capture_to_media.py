#!/usr/bin/env python3
"""Turn a UI_CAPTURE console log into PNG screenshots and GIFs.

Usage: capture_to_media.py <serial.log> <out_dir>

Frames named <gif>_NNN become <gif>.gif (using each frame's hold time); any other frame becomes
<name>.png. Images are masked to the round 360x360 panel. Needs ffmpeg on the PATH.
"""
import base64, os, re, subprocess, sys, tempfile
from collections import defaultdict

log, out = sys.argv[1], sys.argv[2]
os.makedirs(out, exist_ok=True)
tmp = tempfile.mkdtemp()

frames, cur, data = [], None, []
for line in open(log, errors='replace'):
    line = line.rstrip('\r\n')
    if line.startswith('SHOT '):
        _, name, w, h, hold = line.split()
        cur, data = (name, int(w), int(h), int(hold)), []
    elif line.startswith('D ') and cur:
        data.append(line[2:])
    elif line.startswith('END ') and cur:
        name, w, h, hold = cur
        try:
            raw = base64.b64decode(''.join(data))
        except ValueError:
            raw = b''  # damaged in transit
        if len(raw) == w * h * 2:
            frames.append((name, w, h, hold, raw))
        else:
            print(f'skipping {name}: {len(raw)} bytes, expected {w * h * 2}')
        cur = None

# Round-panel mask: transparent outside the circle.
MASK = "format=rgba,geq=r='r(X,Y)':g='g(X,Y)':b='b(X,Y)':a='if(lte(hypot(X-W/2+0.5,Y-H/2+0.5),W/2),255,0)'"

def ffmpeg(*args):
    subprocess.run(['ffmpeg', '-v', 'error', '-y', *args], check=True)

gifs = defaultdict(list)
for name, w, h, hold, raw in frames:
    path = os.path.join(tmp, name + '.raw')
    open(path, 'wb').write(raw)
    m = re.match(r'(.+)_(\d{3})$', name)
    if m:
        gifs[m.group(1)].append((path, w, h, hold))
    else:
        ffmpeg('-f', 'rawvideo', '-pix_fmt', 'rgb565le', '-s', f'{w}x{h}', '-i', path, '-vf', MASK,
               os.path.join(out, name + '.png'))
        print('wrote', name + '.png')

for gif, items in gifs.items():
    # Per-frame hold times via the concat demuxer, then a shared palette for clean colours.
    pngs = []
    for i, (path, w, h, hold) in enumerate(items):
        png = os.path.join(tmp, f'{gif}_{i:03d}.png')
        ffmpeg('-f', 'rawvideo', '-pix_fmt', 'rgb565le', '-s', f'{w}x{h}', '-i', path, png)
        pngs.append((png, hold))
    concat = os.path.join(tmp, gif + '.txt')
    with open(concat, 'w') as f:
        for png, hold in pngs:
            f.write(f"file '{png}'\nduration {max(hold, 20) / 1000:.3f}\n")
        f.write(f"file '{pngs[-1][0]}'\n")
    circle = "geq=r='if(lte(hypot(X-W/2+0.5,Y-H/2+0.5),W/2),r(X,Y),16)':g='if(lte(hypot(X-W/2+0.5,Y-H/2+0.5),W/2),g(X,Y),20)':b='if(lte(hypot(X-W/2+0.5,Y-H/2+0.5),W/2),b(X,Y),24)'"
    ffmpeg('-f', 'concat', '-safe', '0', '-i', concat,
           '-vf', f"{circle},split[a][b];[a]palettegen=stats_mode=full[p];[b][p]paletteuse=dither=sierra2_4a",
           '-loop', '0', os.path.join(out, gif + '.gif'))
    print('wrote', gif + '.gif', f'({len(items)} frames)')
