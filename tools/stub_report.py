#!/usr/bin/env python3
"""List the PS4 imports a run hit through the loader's return-0 fallback ("STUB:" lines).

Usage: python3 -I tools/stub_report.py work/<run>.log [--md port/docs/dreams/STUBS.md]
Each row is an import Dreams called that nothing implements yet; the fallback returned 0 (shadPS4
behaviour). Review these: some are fine as 0 (telemetry, streaming, VR when absent), others need
real implementations (GPU, video out, event flags, ...).
"""
import argparse
import re
from pathlib import Path

ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
ap.add_argument('log', type=Path)
ap.add_argument('--md', type=Path)
a = ap.parse_args()
pat = re.compile(r'^STUB: ([^#\s]+)#(\S+) = (\S+) returns 0 \(first call from (\S+), arg0=(\S+)\)')
rows = [m.groups() for m in map(pat.match, a.log.read_text(errors='replace').splitlines()) if m]
lines = [f'# Imports served by the return-0 fallback ({len(rows)}, from {a.log.name}, in first-call order)', '',
         '| # | NID | name | first caller | arg0 |', '|---|---|---|---|---|']
lines += [f'| {i} | `{nid}` | {name} | {caller} | {arg0} |' for i, (nid, _suf, name, caller, arg0) in enumerate(rows, 1)]
text = '\n'.join(lines) + '\n'
if a.md:
    a.md.write_text(text)
else:
    print(text)
