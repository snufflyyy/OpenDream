"""Read-only frame-rate sampler for a running native Dreams (v2.52), docs/dreams/FPS.md and GPU_HANG.md.

Usage: python3 -I tools/fps_sample.py OUT.csv START_S END_S [EXE_SUFFIX]
Waits for a process whose /proc/<pid>/exe ends with EXE_SUFFIX (default 'out/dreams-probe'), then
every 0.5 s between START_S and END_S (seconds since the process was found) writes the render frame
counter (eboot +0x5845980), the 30 Hz gameplay counter (+0x6f738e8), amdgpu gpu_busy_percent,
and the render clock's measured interval (+0x26e4f90). Elapsed time uses a monotonic clock.
Summary: python3 -I tools/fps_sample.py --summary OUT.csv...
"""
import glob, os, statistics, struct, sys, time

SIG_OFF = 0xe34d73  # v2.52 flip-rate pick (FPS.md); refuse other versions.
SIGNATURES = (bytes.fromhex('0fb605ee5ba704eb0931c0'),  # original PS4 timing
              bytes.fromhex('31c09090909090eb0931c0'))  # PatchFrameRate's paired patch


def find_base(starts, read):
    for lo in starts:
        try:
            if read(lo + SIG_OFF, len(SIGNATURES[0])) in SIGNATURES:
                return lo
        except OSError:
            pass
    return None


def summary(paths):
    import csv
    for path in paths:
        rows = list(csv.DictReader(open(path)))
        if len(rows) < 3:
            print(path, 'too few rows', len(rows)); continue
        t = [float(r['t']) for r in rows]
        rf = [int(r['render']) for r in rows]
        g = [int(r['game']) for r in rows]
        b = [int(r['busy']) for r in rows if r['busy']]
        dt = t[-1] - t[0]
        inst = sorted((rf[i + 1] - rf[i]) / (t[i + 1] - t[i]) for i in range(len(t) - 1))
        print(f"{os.path.basename(path)}: {dt:.1f} s, render {(rf[-1] - rf[0]) / dt:.1f} fps "
              f"(p10 {inst[len(inst) // 10]:.1f}), gameplay {(g[-1] - g[0]) / dt:.1f}/s, "
              f"gpu busy mean {statistics.mean(b) if b else 0:.1f}%")


def find_pid(suffix):
    for d in glob.glob('/proc/[0-9]*'):
        try:
            if os.readlink(d + '/exe').endswith(suffix):
                return int(d.rsplit('/', 1)[1])
        except OSError:
            pass


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    if sys.argv[1] == '--summary':
        return summary(sys.argv[2:])
    if len(sys.argv) not in (4, 5):
        sys.exit(__doc__)
    out, t_start, t_end = sys.argv[1], float(sys.argv[2]), float(sys.argv[3])
    suffix = sys.argv[4] if len(sys.argv) > 4 else 'out/dreams-probe'
    pid = None
    for _ in range(7200):
        pid = find_pid(suffix)
        if pid:
            break
        time.sleep(0.5)
    if not pid:
        sys.exit('no process')
    t0 = time.monotonic()
    busy = next(iter(glob.glob('/sys/class/drm/card*/device/gpu_busy_percent')), None)
    mem = open(f'/proc/{pid}/mem', 'rb', 0)

    def rd(a, n):
        mem.seek(a)
        return mem.read(n)

    base = None
    while base is None and time.monotonic() - t0 < 60:
        with open(f'/proc/{pid}/maps') as maps:
            base = find_base((int(line.split('-')[0], 16) for line in maps), rd)
        if base is None:
            time.sleep(1)
    if base is None:
        sys.exit('unsupported image: expected original or patched Dreams v2.52 signature')
    with open(out, 'w') as f:
        f.write('t,render,game,busy,render_dt\n')
        while time.monotonic() - t0 <= t_end:
            t = time.monotonic() - t0
            try:
                r = struct.unpack('<I', rd(base + 0x5845980, 4))[0]
                g = struct.unpack('<I', rd(base + 0x6f738e8, 4))[0]
                render_dt = struct.unpack('<f', rd(base + 0x26e4f90, 4))[0]
            except (OSError, struct.error):
                break
            b = open(busy).read().strip() if busy else ''
            if t >= t_start:
                f.write(f'{t:.2f},{r},{g},{b},{render_dt:.8f}\n')
                f.flush()
            time.sleep(0.5)


if __name__ == '__main__':
    main()
