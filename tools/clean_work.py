#!/usr/bin/env python3 -I
"""Prune run output in work/ (dry run by default).

  python3 -I tools/clean_work.py [--apply] [--keep N] [--older-than DAYS]

Categories (the newest --keep of each are always kept, default 10; an item is only removed if it is
also older than --older-than days, default 3):
  work/captures/<run>/   work/logs/run-*.log   work/mangohud/* (csv + subdirs)
  work/*.log, work/*.out scratch files (age rule only)
  work/agent-rt.*        stale sandbox runtime dirs, removed only when no process has them as cwd/
                         open file/cmdline and no weston/dreams-probe mentions the path (age rule
                         does not apply)
Never touched: work/out, work/user, work/vcore-user, work/vcore-shader-cache, work/shadps4-ref,
work/crash, work/vcore-probe, anything outside work/. Without --apply nothing is deleted.
"""
import argparse, os, shutil, sys, time

ROOT = os.path.realpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
WORK = os.path.join(ROOT, "work")
PROTECTED = {"out", "user", "vcore-user", "vcore-shader-cache", "shadps4-ref", "crash", "vcore-probe"}


def size(p):
    if os.path.islink(p) or os.path.isfile(p):
        return os.lstat(p).st_size
    return sum(os.lstat(os.path.join(d, f)).st_size for d, _, fs in os.walk(p) for f in fs)


def mtime(p):
    return os.lstat(p).st_mtime


def in_use(path):
    """True if any process has cwd/fds/cmdline pointing into path."""
    for pid in filter(str.isdigit, os.listdir("/proc")):
        base = "/proc/" + pid
        try:
            if os.path.realpath(base + "/cwd").startswith(path):
                return True
            with open(base + "/cmdline", "rb") as f:
                if path.encode() in f.read() or path.encode() in open(base + "/environ", "rb").read():
                    return True
            for fd in os.listdir(base + "/fd"):
                if os.path.realpath(base + "/fd/" + fd).startswith(path):
                    return True
        except OSError:
            continue
    return False


def pick(items, keep, older_days):
    """items: paths. Keep the newest `keep`, remove the rest if older than the cutoff."""
    cutoff = time.time() - older_days * 86400
    items = sorted(items, key=mtime, reverse=True)
    return [p for p in items[keep:] if mtime(p) < cutoff]


def ls(d, pred=lambda n: True):
    if not os.path.isdir(d):
        return []
    return [os.path.join(d, n) for n in os.listdir(d) if pred(n)]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawTextHelpFormatter)
    ap.add_argument("--apply", action="store_true", help="really delete (default: dry run)")
    ap.add_argument("--keep", type=int, default=10)
    ap.add_argument("--older-than", type=float, default=3, dest="days")
    a = ap.parse_args()

    plan = []  # (category, path)
    plan += [("captures", p) for p in pick(ls(WORK + "/captures", lambda n: True), a.keep, a.days)]
    plan += [("run logs", p) for p in pick(ls(WORK + "/logs", lambda n: n.startswith("run-") and n.endswith(".log")), a.keep, a.days)]
    plan += [("mangohud", p) for p in pick(ls(WORK + "/mangohud"), a.keep, a.days)]
    scratch = ls(WORK, lambda n: n.endswith((".log", ".out")) and os.path.isfile(os.path.join(WORK, n)))
    plan += [("scratch", p) for p in pick(scratch, 0, a.days)]
    for p in ls(WORK, lambda n: n.startswith("agent-rt.")):
        if os.path.isdir(p) and not in_use(p):
            plan.append(("agent-rt", p))
        elif os.path.isdir(p):
            print("keeping (in use): " + os.path.relpath(p, ROOT))

    total = 0
    for cat, p in plan:
        assert os.path.realpath(p).startswith(WORK + os.sep), p
        assert os.path.relpath(p, WORK).split(os.sep)[0] not in PROTECTED, p
        s = size(p)
        total += s
        print("%-9s %9.1f MB  %s" % (cat, s / 1e6, os.path.relpath(p, ROOT)))
        if a.apply:
            shutil.rmtree(p) if os.path.isdir(p) and not os.path.islink(p) else os.remove(p)
    print("%s %d items, %.1f MB (keep=%d, older-than=%g d)" %
          ("removed" if a.apply else "would remove", len(plan), total / 1e6, a.keep, a.days))
    if not a.apply:
        print("dry run: rerun with --apply to delete")


if __name__ == "__main__":
    sys.exit(main())
