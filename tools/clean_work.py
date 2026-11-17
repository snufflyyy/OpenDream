#!/usr/bin/env python3 -I
"""Prune run output in work/ (dry run by default).

  python3 -I tools/clean_work.py [--apply] [--keep N] [--older-than DAYS]

Categories (the newest --keep of each are always kept, default 10; an item is only removed if it is
also older than --older-than days, default 3):
  work/captures/<run>/   work/logs/run-*.log   work/mangohud/* (csv + subdirs)
  work/*.log, work/*.out scratch files (age rule only)
  work/agent-rt.*        stale sandbox runtime dirs with best-effort /proc in-use checks
                         (age rule does not apply; stop agent tests before --apply)
Protected: work/out, work/user, work/vcore-user, work/vcore-shader-cache, work/shadps4-ref,
work/crash, work/vcore-probe, paths outside work/ and symlink aliases. Deletion requires safe
no-follow directory descriptors and FD-relative rmtree; unsupported platforms fail closed.
This is not a sandbox against direct same-user renames or bind mounts.
Without --apply nothing is deleted.
"""
import argparse, contextlib, inspect, math, os, shutil, stat, sys, time

ROOT = os.path.realpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
WORK = os.path.join(ROOT, "work")
PROTECTED = {"out", "user", "vcore-user", "vcore-shader-cache", "shadps4-ref", "crash", "vcore-probe"}


# Every traversal below WORK is anchored to an open directory, never to a checked pathname.
DIR_FLAGS = (os.O_RDONLY | getattr(os, "O_DIRECTORY", 0) |
             getattr(os, "O_NOFOLLOW", 0) | getattr(os, "O_CLOEXEC", 0))


@contextlib.contextmanager
def parent_fd(root_fd, path):
    relative = os.path.relpath(os.path.abspath(path), os.path.abspath(WORK))
    parts = relative.split(os.sep)
    if (not parts or parts[0] in PROTECTED or
            any(part in ("", os.curdir, os.pardir) for part in parts)):
        raise ValueError("unsafe path")
    fd = os.dup(root_fd)
    try:
        for part in parts[:-1]:
            next_fd = os.open(part, DIR_FLAGS, dir_fd=fd)
            os.close(fd)
            fd = next_fd
        yield fd, parts[-1]
    finally:
        os.close(fd)


def leaf_stat(root_fd, path):
    with parent_fd(root_fd, path) as (fd, name):
        return os.stat(name, dir_fd=fd, follow_symlinks=False)


def tree_size(fd):
    total = 0
    for name in os.listdir(fd):
        entry = os.stat(name, dir_fd=fd, follow_symlinks=False)
        if stat.S_ISDIR(entry.st_mode):
            child = os.open(name, DIR_FLAGS, dir_fd=fd)
            try:
                actual = os.fstat(child)
                if (entry.st_dev, entry.st_ino) != (actual.st_dev, actual.st_ino):
                    raise OSError("directory changed during sizing")
                total += tree_size(child)
            finally:
                os.close(child)
        else:
            total += entry.st_size
    return total


def item_size(fd, name, entry):
    if not stat.S_ISDIR(entry.st_mode):
        return entry.st_size
    child = os.open(name, DIR_FLAGS, dir_fd=fd)
    try:
        actual = os.fstat(child)
        if (entry.st_dev, entry.st_ino) != (actual.st_dev, actual.st_ino):
            raise OSError("directory changed during sizing")
        return tree_size(child)
    finally:
        os.close(child)


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


def pick(root_fd, items, keep, older_days):
    """Keep the newest `keep`, remove the rest if older than the cutoff."""
    cutoff = time.time() - older_days * 86400
    dated = []
    for path in items:
        try:
            dated.append((leaf_stat(root_fd, path).st_mtime, path))
        except OSError:
            continue
    dated.sort(key=lambda item: item[0], reverse=True)
    return [path for mtime, path in dated[keep:] if mtime < cutoff]


def safe_path(path):
    """Reject protected paths and symlink aliases, including category-directory aliases."""
    relative = os.path.relpath(os.path.abspath(path), os.path.abspath(WORK))
    first = relative.split(os.sep)[0]
    if first in PROTECTED or first in (os.pardir, os.curdir):
        return False
    # Resolve WORK itself to support a checkout reached through a symlink, but never
    # follow a symlink below it (captures -> user was able to delete protected saves).
    expected = os.path.join(os.path.realpath(WORK), relative)
    return not os.path.islink(path) and os.path.realpath(path) == expected


def ls(root_fd, category, pred=lambda n: True):
    try:
        fd = os.open(category, DIR_FLAGS, dir_fd=root_fd) if category else os.dup(root_fd)
    except OSError:
        return []
    try:
        base = os.path.join(WORK, category) if category else WORK
        result = []
        for name in os.listdir(fd):
            path = os.path.join(base, name)
            try:
                entry = os.stat(name, dir_fd=fd, follow_symlinks=False)
                if pred(name) and not stat.S_ISLNK(entry.st_mode) and safe_path(path):
                    result.append(path)
            except OSError:
                continue
        return result
    finally:
        os.close(fd)


def is_type(root_fd, path, mode_test):
    try:
        return mode_test(leaf_stat(root_fd, path).st_mode)
    except OSError:
        return False


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawTextHelpFormatter)
    ap.add_argument("--apply", action="store_true", help="really delete (default: dry run)")
    ap.add_argument("--keep", type=int, default=10)
    ap.add_argument("--older-than", type=float, default=3, dest="days")
    a = ap.parse_args()
    if a.keep < 0 or not math.isfinite(a.days) or a.days < 0:
        ap.error("--keep and --older-than must be non-negative and finite")

    if (not getattr(shutil.rmtree, "avoids_symlink_attacks", False) or
            "dir_fd" not in inspect.signature(shutil.rmtree).parameters or
            not all(hasattr(os, flag) for flag in ("O_DIRECTORY", "O_NOFOLLOW")) or
            os.open not in os.supports_dir_fd or os.stat not in os.supports_dir_fd or
            os.unlink not in os.supports_dir_fd or os.stat not in os.supports_follow_symlinks or
            os.listdir not in os.supports_fd):
        ap.error("safe dir-fd cleanup is unavailable on this platform")
    try:
        root_fd = os.open(WORK, DIR_FLAGS)
    except OSError as error:
        ap.error("cannot safely open work directory: " + str(error))
    try:
        plan = []  # (category, path)
        plan += [("captures", p) for p in pick(root_fd, ls(root_fd, "captures"), a.keep, a.days)]
        plan += [("run logs", p) for p in pick(root_fd, ls(root_fd, "logs", lambda n: n.startswith("run-") and n.endswith(".log")), a.keep, a.days)]
        plan += [("mangohud", p) for p in pick(root_fd, ls(root_fd, "mangohud"), a.keep, a.days)]
        scratch = [p for p in ls(root_fd, "", lambda n: n.endswith((".log", ".out")))
                   if is_type(root_fd, p, stat.S_ISREG)]
        plan += [("scratch", p) for p in pick(root_fd, scratch, 0, a.days)]
        for p in ls(root_fd, "", lambda n: n.startswith("agent-rt.")):
            if is_type(root_fd, p, stat.S_ISDIR) and not in_use(p):
                plan.append(("agent-rt", p))
            elif is_type(root_fd, p, stat.S_ISDIR):
                print("keeping (in use): " + os.path.relpath(p, ROOT))

        total = 0
        processed = 0
        for cat, p in plan:
            if not safe_path(p):
                print("keeping (unsafe path): " + os.path.relpath(p, ROOT))
                continue
            try:
                with parent_fd(root_fd, p) as (fd, name):
                    entry = os.stat(name, dir_fd=fd, follow_symlinks=False)
                    if stat.S_ISLNK(entry.st_mode):
                        raise OSError("symbolic link")
                    s = item_size(fd, name, entry)
                    if a.apply:
                        if stat.S_ISDIR(entry.st_mode):
                            shutil.rmtree(name, dir_fd=fd)
                        else:
                            os.unlink(name, dir_fd=fd)
                total += s
                processed += 1
                print("%-9s %9.1f MB  %s" % (cat, s / 1e6, os.path.relpath(p, ROOT)))
            except (OSError, ValueError) as error:
                print("keeping (unsafe or changed path): %s: %s" % (os.path.relpath(p, ROOT), error))
        print("%s %d items, %.1f MB (keep=%d, older-than=%g d)" %
              ("removed" if a.apply else "would remove", processed, total / 1e6, a.keep, a.days))
        if not a.apply:
            print("dry run: rerun with --apply to delete")
    finally:
        os.close(root_fd)


if __name__ == "__main__":
    sys.exit(main())
