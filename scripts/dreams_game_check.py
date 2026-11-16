#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 Dreams-Recomp contributors
# SPDX-License-Identifier: GPL-2.0-or-later
"""Is this folder a Dreams game dump the port can run?  dreams_game_check.py GAME_DIR

Replaces the Bloodborne-only game_check.py (BB_SKIP_GAME_CHECK) for Dreams. Reads sce_sys/param.sfo
and prints TITLE_ID, region (from the CONTENT_ID prefix: UP=US, EP=EU), VERSION (the game version) and APP_VER, SYSTEM_VER (minimum firmware), and whether eboot.bin
and sce_module/{libc,libSceFios2,libSceJobManager}.prx exist and are plaintext SELF (magic
4f153d1d: encrypted/fake-signed dumps are not supported).

Exit 0: a Dreams title with all files present. Exit 2: not Dreams, or files missing/not SELF.
Versions: 2.52 (CUSA08010, the first target) and 2.64 (the Dreams fork's target) are known; any other
version is accepted with a warning (nothing here is version specific). Run with `python3 -I`.
"""
import struct
import sys
from pathlib import Path

SELF_MAGIC = bytes.fromhex('4f153d1d')
REQUIRED = ('eboot.bin', 'sce_module/libc.prx', 'sce_module/libSceFios2.prx',
            'sce_module/libSceJobManager.prx')
KNOWN_VERSIONS = ('02.52', '02.64')
# Title id -> (description, verified). Only what we have seen on a dump is verified; the rest is from
# memory of PSN/Dreams release listings and must be confirmed against a real param.sfo.
TITLES = {
    'CUSA08010': ('Dreams retail (user dump, CONTENT_ID UP9000-CUSA08010)', True),
    'CUSA04301': ('Dreams (the title the Dreams fork gates its fixes on)', True),
    'CUSA04271': ('Dreams, other region/edition', False),
    'CUSA07887': ('Dreams, other region/edition', False),
    'CUSA07888': ('Dreams, other region/edition', False),
}
REGIONS = {'UP': 'US', 'EP': 'EU', 'JP': 'JP', 'HP': 'Asia', 'KP': 'Korea', 'IP': 'Intl'}


def sfo(data):
    """Parse a param.sfo; reuses prepare.py's parser when importable."""
    try:
        sys.path.insert(0, str(Path(__file__).resolve().parent))
        import prepare
        return prepare.sfo(data)
    except Exception:
        pass
    magic, _, keys, values, count = struct.unpack_from('<4sIIII', data, 0)
    if magic != b'\0PSF':
        raise ValueError('bad SFO signature')
    out = {}
    for i in range(count):
        key, fmt, size, _, off = struct.unpack_from('<HHIII', data, 20 + i * 16)
        k = data[keys + key:data.index(0, keys + key)].decode()
        v = data[values + off:values + off + size]
        out[k] = struct.unpack('<I', v)[0] if fmt == 0x404 else v.rstrip(b'\0').decode('utf8', 'replace')
    return out


def region(content_id):
    return REGIONS.get(content_id[:2], 'unknown') if content_id else 'unknown'


def fw(value):
    """SYSTEM_VER is 0xMMmmpppp BCD-ish (0x07508001 = 7.50)."""
    if isinstance(value, int):
        return f'{value:#010x} (firmware {value >> 24:x}.{(value >> 16) & 0xff:02x})'
    return str(value)


def check(game):
    """Returns (exit_code, lines)."""
    game = Path(game)
    out = []
    sfo_path = game / 'sce_sys' / 'param.sfo'
    if not sfo_path.is_file():
        return 2, [f'FAIL: {sfo_path} not found: not an extracted PS4 game folder']
    try:
        p = sfo(sfo_path.read_bytes())
    except Exception as e:
        return 2, [f'FAIL: cannot parse {sfo_path}: {e}']
    tid = str(p.get('TITLE_ID', ''))
    cid = str(p.get('CONTENT_ID', ''))
    title = str(p.get('TITLE', ''))
    app_ver = str(p.get('APP_VER', '?'))
    version = str(p.get('VERSION', '?'))  # the game version (2.52); APP_VER is the package's 01.00
    out += [f'TITLE      {title}', f'TITLE_ID   {tid}', f'CONTENT_ID {cid}', f'region     {region(cid)}',
            f'VERSION    {version}   (APP_VER {app_ver})',
            f'SYSTEM_VER {fw(p.get("SYSTEM_VER", "?"))}']
    known = tid in TITLES
    if known:
        desc, verified = TITLES[tid]
        out.append(f'title      {desc}{"" if verified else " [UNVERIFIED id]"}')
    is_dreams = known or title.strip().lower().rstrip('\u2122') == 'dreams'
    if not is_dreams:
        return 2, out + [f'FAIL: {tid or "?"} ({title or "?"}) is not Dreams']
    ok = True
    for rel in REQUIRED:
        path = game / rel
        if not path.is_file():
            out.append(f'MISSING    {rel}')
            ok = False
            continue
        with open(path, 'rb') as f:
            magic = f.read(4)
        if magic != SELF_MAGIC:
            out.append(f'BAD        {rel}: magic {magic.hex()} is not a plaintext SELF (4f153d1d)')
            ok = False
        else:
            out.append(f'ok         {rel}')
    if not ok:
        return 2, out + ['FAIL: missing or unsupported files']
    if version not in KNOWN_VERSIONS:
        out.append(f'WARNING: version {version} is not one of {", ".join(KNOWN_VERSIONS)}; continuing')
    return 0, out + [f'OK: Dreams {version}']


if __name__ == '__main__':
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    code, lines = check(sys.argv[1])
    print('\n'.join(lines))
    sys.exit(code)
