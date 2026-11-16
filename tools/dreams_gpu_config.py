"""Copy a shadPS4 per-game config template to the vcore user dir, docs/dreams/VCORE.md.

Usage: python3 -I tools/dreams_gpu_config.py TEMPLATE OUTPUT

run-dreams.sh calls this for every run ($ROOT/notes/shadps4-dreams-runtime/CUSA04301.json ->
$WORK/vcore-user/custom_configs/<SERIAL>.json), and DREAMS_VCORE_CONFIG=<file> replaces the
template so one run can enable an extra setting (e.g. GPU.dump_shaders) without editing the
shared template. The output is written to a temp file and renamed, so a bad template leaves the
existing config untouched; values are copied as-is.
"""
import json, os, sys, tempfile


def copy_config(template, output):
    """Write TEMPLATE's JSON object to OUTPUT; raises ValueError if it is not a JSON object."""
    with open(template, encoding="utf-8") as f:
        config = json.load(f)
    if not isinstance(config, dict):
        raise ValueError(f"{template}: not a JSON object")
    directory = os.path.dirname(os.path.abspath(output))
    os.makedirs(directory, exist_ok=True)
    fd, tmp = tempfile.mkstemp(dir=directory, prefix=".dreams_gpu_config.")
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as f:
            json.dump(config, f, indent=2)
            f.write("\n")
        os.replace(tmp, output)
    except BaseException:
        try:
            os.unlink(tmp)
        except OSError:
            pass
        raise
    return config


def main(argv):
    if len(argv) != 3:
        print("usage: dreams_gpu_config.py TEMPLATE OUTPUT", file=sys.stderr)
        return 2
    try:
        copy_config(argv[1], argv[2])
    except (OSError, ValueError) as e:
        print(f"dreams_gpu_config.py: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
