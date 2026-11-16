#!/usr/bin/env python3
"""Group the loader's unresolved imports by library, with names and shadPS4 references.

Usage:
  BB_LIST_UNRESOLVED=1 port/run-dreams.sh --cpu-only > work/run.log 2>&1
  python3 -I tools/unresolved_report.py work/run.log [--md out.md] [--lib libkernel]

Input lines look like "Unresolved import: <NID>#<lib>#<mod>". Names/libraries come from
notes/dreams-imports.json (built by tools/ps4_imports.py) and, as a fallback, from shadPS4's
LIB_FUNCTION registrations in ref/shadps4/src.
"""
import argparse
import collections
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def nid_table():
    table = {}
    data = json.loads((ROOT / 'notes/dreams-imports.json').read_text())
    for image, libs in data['imports'].items():
        for lib, entries in (libs.items() if isinstance(libs, dict) else []):
            for e in entries:
                table.setdefault(e['nid'], dict(name=e.get('name'), lib=lib,
                                                shadps4=e.get('shadps4_hle'), images=set()))
                table[e['nid']]['images'].add(image)
    return table


def shadps4_table():
    """NID -> (library, function, file) from shadPS4 LIB_FUNCTION registrations."""
    out = {}
    pat = re.compile(r'LIB_FUNCTION\("([^"]+)",\s*"([^"]+)",\s*\d+,\s*"[^"]+",\s*([\w:]+)')
    src = ROOT / 'ref/shadps4/src/core/libraries'
    for f in src.rglob('*.cpp'):
        for m in pat.finditer(f.read_text(errors='replace')):
            out.setdefault(m.group(1), (m.group(2), m.group(3), str(f.relative_to(ROOT))))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('log', type=Path)
    ap.add_argument('--md', type=Path)
    ap.add_argument('--lib')
    a = ap.parse_args()
    nids = [m.group(1) for m in re.finditer(r'Unresolved import: ([^#\s]+)#', a.log.read_text(errors='replace'))]
    known, shad = nid_table(), shadps4_table()
    groups = collections.defaultdict(list)
    for nid in dict.fromkeys(nids):
        k, s = known.get(nid, {}), shad.get(nid)
        lib = k.get('lib') or (s[0] if s else '?')
        name = k.get('name') or (s[1] if s else '?')
        ref = k.get('shadps4') or (s[2] if s else '')
        imgs = ','.join(sorted(i.split('/')[-1] for i in k.get('images', ())))
        groups[lib].append((nid, name, ref, imgs))
    lines = [f'# Unresolved imports ({len(set(nids))}) from {a.log.name}', '']
    for lib in sorted(groups, key=lambda l: (-len(groups[l]), l)):
        if a.lib and lib != a.lib:
            continue
        lines.append(f'## {lib} ({len(groups[lib])})')
        lines.append('| NID | name | imported by | shadPS4 |')
        lines.append('|---|---|---|---|')
        for nid, name, ref, imgs in sorted(groups[lib], key=lambda r: r[1] or ''):
            lines.append(f'| `{nid}` | {name} | {imgs} | {ref} |')
        lines.append('')
    text = '\n'.join(lines)
    if a.md:
        a.md.write_text(text)
    else:
        sys.stdout.write(text)
    print('\n'.join(f'{len(v):4} {k}' for k, v in sorted(groups.items(), key=lambda kv: -len(kv[1]))),
          file=sys.stderr)


if __name__ == '__main__':
    main()
