#!/usr/bin/env python3
"""Review a native-port test capture (port/vcore/shim/capture.cpp, docs/dreams/CAPTURE.md).

A capture dir (work/captures/<run>/) holds small PNGs of frames that changed + index.csv.

  python3 -I tools/capture_sheet.py timeline <dir>          text overview: activity per second,
                                                            saved frames, static stretches
  python3 -I tools/capture_sheet.py sheet <dir> [opts]      4x4 contact sheets -> <dir>/sheets/
        --from S --to S     time window in seconds
        --every K           take every K-th saved frame (overview of a long run)
        --cols C --rows R   grid (default 4x4); --width W tile width (default 480)
  python3 -I tools/capture_sheet.py latest                  print the newest work/captures/<run>

For agents: read the timeline first (cheap text), then open only the sheets around interesting
times. Each tile is labelled "#seq  t=12.5s  d=3.4" (d = change vs the previous saved frame).
"""
import argparse
import csv
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def load(dirpath):
    rows = []
    with open(Path(dirpath) / 'index.csv', newline='') as f:
        for r in csv.DictReader(f):
            rows.append(dict(seq=int(r['seq']), t=float(r['t_ms']) / 1000.0, saved=int(r['saved']),
                             diff=float(r['diff']) if r['diff'] else None, file=r['file']))
    return rows


def timeline(dirpath):
    rows = load(dirpath)
    if not rows:
        print('no captures'); return
    saved = [r for r in rows if r['saved'] == 1]
    dropped = sum(1 for r in rows if r['saved'] == -1)
    print(f'{dirpath}: {len(rows)} captures over {rows[-1]["t"]:.1f}s, {len(saved)} saved, {dropped} dropped')
    print('t(s)    #seq   diff   note')
    prev_t = None
    for r in saved:
        if prev_t is not None and r['t'] - prev_t >= 3.0:
            print(f'        ...    static for {r["t"] - prev_t:.1f}s')
        d = r['diff']
        note = 'first' if d is not None and d >= 255 else ('BIG CHANGE' if d and d > 25 else ('change' if d and d > 5 else 'small'))
        print(f'{r["t"]:6.1f}  {r["seq"]:5d}  {d if d is not None else 0:5.1f}  {note}')
        prev_t = r['t']


def sheet(dirpath, start, end, every, cols, rows_n, width):
    from PIL import Image, ImageDraw, ImageFont
    rows = [r for r in load(dirpath) if r['saved'] == 1 and (start is None or r['t'] >= start) and (end is None or r['t'] <= end)]
    rows = rows[::max(1, every)]
    if not rows:
        print('no saved frames in that window'); return
    out = Path(dirpath) / 'sheets'
    out.mkdir(exist_ok=True)
    try:
        font = ImageFont.truetype('/usr/share/fonts/dejavu-sans-fonts/DejaVuSans-Bold.ttf', 16)
    except OSError:
        font = ImageFont.load_default()
    per = cols * rows_n
    written = []
    for page in range(0, len(rows), per):
        chunk = rows[page:page + per]
        first = Image.open(Path(dirpath) / chunk[0]['file'])
        th = round(first.height * width / first.width)
        canvas = Image.new('RGB', (cols * width, rows_n * th), (40, 0, 40))
        draw = ImageDraw.Draw(canvas)
        for i, r in enumerate(chunk):
            img = Image.open(Path(dirpath) / r['file']).convert('RGB')
            if img.width != width:
                img = img.resize((width, th))
            x, y = (i % cols) * width, (i // cols) * th
            canvas.paste(img, (x, y))
            label = f'#{r["seq"]}  t={r["t"]:.1f}s  d={r["diff"]:.1f}' if r['diff'] is not None else f'#{r["seq"]}  t={r["t"]:.1f}s'
            draw.rectangle([x, y, x + 8 + 9 * len(label), y + 22], fill=(0, 0, 0))
            draw.text((x + 4, y + 2), label, fill=(255, 255, 0), font=font)
            draw.rectangle([x, y, x + width - 1, y + th - 1], outline=(90, 90, 90))
        name = out / f'sheet_t{chunk[0]["t"]:07.1f}-{chunk[-1]["t"]:07.1f}.png'
        canvas.save(name)
        written.append(name)
    for w in written:
        print(w)


def latest():
    runs = sorted((ROOT / 'work/captures').glob('*/index.csv'), key=lambda p: p.stat().st_mtime)
    print(runs[-1].parent if runs else 'no captures')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)
    t = sub.add_parser('timeline'); t.add_argument('dir')
    s = sub.add_parser('sheet'); s.add_argument('dir')
    s.add_argument('--from', dest='start', type=float); s.add_argument('--to', dest='end', type=float)
    s.add_argument('--every', type=int, default=1); s.add_argument('--cols', type=int, default=4)
    s.add_argument('--rows', type=int, default=4); s.add_argument('--width', type=int, default=480)
    sub.add_parser('latest')
    a = ap.parse_args()
    if a.cmd == 'timeline':
        timeline(a.dir)
    elif a.cmd == 'sheet':
        sheet(a.dir, a.start, a.end, a.every, a.cols, a.rows, a.width)
    else:
        latest()


if __name__ == '__main__':
    sys.exit(main())
