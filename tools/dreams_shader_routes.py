#!/usr/bin/env python3
"""Dreams shader routes across game versions (see docs/dreams/VERSIONS.md).

The Dreams-ShadPS4 fork keys its fixes on the shader hash embedded in each PS4 shader binary
(BinaryInfo.shader_hash, shadPS4 `pgm_hash`). That hash changes with every game build even when
the code does not, so the fixes only engage on v2.64 (CUSA04301). This tool works on the game's
own shader pack (`<game>/shaders/dd.shaders`) to:

  census PACK [PACK...]   list every DS_ORDERED_COUNT ("DOC") shader with its signature and the
                          fork route it maps to; `--all` lists every shader
  check  PACK [PACK...]   find each route of tools/dreams_routes.json in each pack (by stable pack
                          key, then fork hash) and report hash / code hash / DOC signature
  emit   PACK [PACK...] -o FILE
                          write the C++ route table (ref/shadps4 branch docmatch:
                          src/shader_recompiler/dreams_route_table.inc) from the routes file and
                          the code hashes found in the given packs

Name each pack VERSION=PATH (e.g. 2.52=../game/CUSA08010/shaders/dd.shaders); a bare path is
labelled by its directory. Signatures (identical in shader_recompiler/dreams_route_match.cpp):

  code hash  XXH64 (seed 0) of the shader's code span: BinaryInfo.length bytes from the
             program start (the `s_mov_b32 vcc_hi, ...` token). This is exactly the span shadPS4
             passes as ShaderParams::code; it excludes the embedded hash.
  DOC sig    for each DS_ORDERED_COUNT in program order "<control>@<counter>", joined by ",":
             control = shadPS4's packed DOC control without the GDS binding
                       ((offset1>>4)&3) | ((offset1&3)<<2) | (offset0<<8)
             counter = guest GDS dword (((M0 base bytes) & 0xfffc) + offset0) >> 2, where the M0
                       base is the first literal with a zero low half found by following the
                       last M0 write (and, through up to 4 levels, the SGPRs it reads) back from
                       the DOC; "?" when none is found. Pass/row/wave terms added to M0 at run
                       time are not part of it (they are register values, not literals).

Run with python3 -I. No third-party modules.
"""
import argparse
import json
import os
import struct
import sys

MASK64 = (1 << 64) - 1
ROUTES_JSON = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'dreams_routes.json')

# ---------------------------------------------------------------- XXH64 (reference algorithm)
_P1, _P2, _P3, _P4, _P5 = (11400714785074694791, 14029467366897019727, 1609587929392839161,
                           9650029242287828579, 2870177450012600261)


def _rotl(x, r):
    return ((x << r) | (x >> (64 - r))) & MASK64


def _round(acc, lane):
    acc = (acc + lane * _P2) & MASK64
    return (_rotl(acc, 31) * _P1) & MASK64


def xxh64(data, seed=0):
    n, i = len(data), 0
    if n >= 32:
        v = [(seed + _P1 + _P2) & MASK64, (seed + _P2) & MASK64, seed, (seed - _P1) & MASK64]
        while i + 32 <= n:
            for k in range(4):
                v[k] = _round(v[k], struct.unpack_from('<Q', data, i + 8 * k)[0])
            i += 32
        h = (_rotl(v[0], 1) + _rotl(v[1], 7) + _rotl(v[2], 12) + _rotl(v[3], 18)) & MASK64
        for k in range(4):
            h ^= _round(0, v[k])
            h = (h * _P1 + _P4) & MASK64
    else:
        h = (seed + _P5) & MASK64
    h = (h + n) & MASK64
    while i + 8 <= n:
        h ^= _round(0, struct.unpack_from('<Q', data, i)[0])
        h = (_rotl(h, 27) * _P1 + _P4) & MASK64
        i += 8
    if i + 4 <= n:
        h ^= (struct.unpack_from('<I', data, i)[0] * _P1) & MASK64
        h = (_rotl(h, 23) * _P2 + _P3) & MASK64
        i += 4
    while i < n:
        h ^= (data[i] * _P5) & MASK64
        h = (_rotl(h, 11) * _P1) & MASK64
        i += 1
    h ^= h >> 33
    h = (h * _P2) & MASK64
    h ^= h >> 29
    h = (h * _P3) & MASK64
    return h ^ (h >> 32)


assert xxh64(b'abc') == 0x44bc2cf5ad770999

# ---------------------------------------------------------------- dd.shaders
CODE_TOKEN = 0xBEEB03FF  # s_mov_b32 vcc_hi, <literal>: first instruction of every PS4 shader


def load_pack(path):
    """dd.shaders: u32 magic 0x12345678, u32 count, count x {u64 key, u32 offset, u32 size}."""
    data = open(path, 'rb').read()
    magic, count = struct.unpack_from('<II', data, 0)
    if magic != 0x12345678:
        raise SystemExit(f'{path}: not a Dreams shader pack (magic {magic:#x})')
    entries = []
    for i in range(count):
        key, off, size = struct.unpack_from('<QII', data, 8 + 16 * i)
        payload = data[off:off + size]
        start = next((j for j in range(0, len(payload) - 8, 4)
                      if struct.unpack_from('<I', payload, j)[0] == CODE_TOKEN), None)
        info = payload.find(b'OrbShdr')
        if start is None or info < 0:
            entries.append(dict(key=key, error='no code/OrbShdr'))
            continue
        word = struct.unpack_from('<I', payload, info + 8)[0]
        length = (word >> 8) & 0xffffff
        code = payload[start:start + length]
        entries.append(dict(key=key, hash=struct.unpack_from('<Q', payload, info + 16)[0],
                            length=length, stage=(word >> 2) & 0xf, code=code))
    return entries


# ---------------------------------------------------------------- GCN (GFX7) walking
def encoding(t):
    if t >> 23 == 0x17d: return 'SOP1'
    if t >> 23 == 0x17f: return 'SOPP'
    if t >> 23 == 0x17e: return 'SOPC'
    if t >> 25 == 0x3f: return 'VOP1'
    if t >> 25 == 0x3e: return 'VOPC'
    top6 = {0x34: 'VOP3', 0x3e: 'EXP', 0x32: 'VINTRP', 0x36: 'DS', 0x38: 'MUBUF', 0x3a: 'MTBUF',
            0x3c: 'MIMG'}.get(t >> 26)
    if top6: return top6
    if t >> 27 == 0x18: return 'SMRD'
    if t >> 28 == 0xb: return 'SOPK'
    if t >> 30 == 0x2: return 'SOP2'
    if t >> 31 == 0: return 'VOP2'
    return '?'


def instructions(code):
    """Yield (pc, encoding, words). Checked against all 604 shaders of v2.52: every walk ends
    with S_ENDPGM exactly at BinaryInfo.length."""
    n = len(code) // 4
    w = struct.unpack('<%dI' % n, code[:n * 4])
    i = 0
    while i < n:
        t = w[i]
        enc = encoding(t)
        size = 1
        if enc in ('VOP3', 'EXP', 'DS', 'MUBUF', 'MTBUF', 'MIMG'):
            size = 2
        elif enc in ('SOP2', 'SOPC'):
            size = 2 if (t & 0xff) == 255 or ((t >> 8) & 0xff) == 255 else 1
        elif enc == 'SOP1':
            size = 2 if (t & 0xff) == 255 else 1
        elif enc == 'SOPK':
            size = 2 if ((t >> 23) & 0x1f) == 0x15 else 1  # s_setreg_imm32_b32
        elif enc in ('VOP1', 'VOPC', 'VOP2'):
            size = 2 if (t & 0x1ff) == 255 or (enc == 'VOP2' and (t >> 25) & 0x3f in (0x20, 0x21)) else 1
        elif enc == 'SMRD':
            size = 2 if (t & 0xff) == 255 and not (t >> 8) & 1 else 1
        yield i * 4, enc, w[i:i + size]
        i += size


M0 = 124


def scalar_dst_and_srcs(enc, words):
    """(sdst, [scalar src codes], literal or None) for scalar ALU writes; else None."""
    t = words[0]
    lit = words[1] if len(words) > 1 else None
    if enc == 'SOP1':
        return (t >> 16) & 0x7f, [t & 0xff], lit
    if enc == 'SOP2':
        return (t >> 16) & 0x7f, [t & 0xff, (t >> 8) & 0xff], lit
    if enc == 'SOPK':
        return (t >> 16) & 0x7f, [], None
    return None


def doc_signature(code):
    insts = list(instructions(code))
    defs = []  # (index, sdst, srcs, literal)
    for idx, (pc, enc, words) in enumerate(insts):
        r = scalar_dst_and_srcs(enc, words)
        if r:
            defs.append((idx,) + r)

    def last_def(reg, before):
        for d in reversed(defs):
            if d[0] < before and d[1] == reg:
                return d
        return None

    def base_from(d, depth):
        if d is None or depth > 4:
            return None
        _, _, srcs, lit = d
        if lit is not None and lit and (lit & 0xffff) == 0:
            return lit >> 16
        for s in srcs:
            if s <= 103 or s in (106, 107):
                b = base_from(last_def(s, d[0]), depth + 1)
                if b is not None:
                    return b
        return None

    sig = []
    for idx, (pc, enc, words) in enumerate(insts):
        if enc == 'DS' and (words[0] >> 18) & 0xff == 63:
            off0, off1 = words[0] & 0xff, (words[0] >> 8) & 0xff
            control = ((off1 >> 4) & 3) | ((off1 & 3) << 2) | (off0 << 8)
            base = base_from(last_def(M0, idx), 0)
            counter = '?' if base is None else '%x' % (((base & 0xfffc) + off0) >> 2)
            sig.append('%04x@%s' % (control, counter))
    return ','.join(sig)


# ---------------------------------------------------------------- routes
def load_routes(path=ROUTES_JSON):
    with open(path) as f:
        return json.load(f)


def parse_packs(specs):
    packs = []
    for spec in specs:
        label, _, path = spec.rpartition('=')
        label = label or os.path.basename(os.path.dirname(os.path.dirname(os.path.abspath(path))))
        entries = [e for e in load_pack(path) if 'error' not in e]
        for e in entries:
            e['code_hash'] = xxh64(e['code'])
            e['doc'] = doc_signature(e['code'])
        packs.append((label, path, entries))
    return packs


def find_route(route, entries):
    by_key = {e['key']: e for e in entries}
    by_hash = {e['hash']: e for e in entries}
    fork = int(route['fork_hash'], 16)
    if fork in by_hash:
        return by_hash[fork], 'fork hash'
    if route.get('key') and int(route['key'], 16) in by_key:
        return by_key[int(route['key'], 16)], 'pack key'
    return None, 'not found'


def cmd_census(args):
    routes = load_routes()
    for label, path, entries in parse_packs(args.packs):
        print(f'# {label}: {path} ({len(entries)} shaders)')
        by_key = {int(r['key'], 16): r for r in routes['routes'] if r.get('key')}
        forks = {int(r['fork_hash'], 16): r for r in routes['routes']}
        for e in sorted(entries, key=lambda e: e['key']):
            if not e['doc'] and not args.all:
                continue
            r = forks.get(e['hash']) or by_key.get(e['key'])
            route = f"{r['fork_hash']} {r['name']}" if r else '-'
            print(f"key={e['key']:016x} hash={e['hash']:08x} len={e['length']:5d} "
                  f"code={e['code_hash']:016x} doc={e['doc'] or '-'}  route={route}")


def cmd_check(args):
    routes = load_routes()
    packs = parse_packs(args.packs)
    for r in routes['routes']:
        print(f"{r['fork_hash']} {r['name']:<40} doc={r.get('doc') or '-'}")
        for label, _, entries in packs:
            e, how = find_route(r, entries)
            if not e:
                print(f'    {label:>6}: {how}')
                continue
            note = []
            if r.get('doc') is not None and e['doc'] != r['doc']:
                note.append(f"DOC SIGNATURE DIFFERS ({e['doc']})")
            if r.get('key') and int(r['key'], 16) != e['key']:
                note.append(f"pack key {e['key']:016x} != routes file {r['key']}")
            print(f"    {label:>6}: hash={e['hash']:08x} key={e['key']:016x} "
                  f"code={e['code_hash']:016x} len={e['length']} via {how} {' '.join(note)}")


def cmd_emit(args):
    routes = load_routes()
    packs = parse_packs(args.packs)
    lines = ['// Generated by port/tools/dreams_shader_routes.py emit from tools/dreams_routes.json',
             '// and the shader packs: ' + ', '.join(f'{l}' for l, _, _ in packs) + '.',
             '// Do not edit by hand: change the routes file and re-run (docs/dreams/VERSIONS.md).',
             '// {fork_hash, "name", "doc signature", shape_match, {code hashes}, {guest hashes}},']
    for r in routes['routes']:
        code_hashes, guest_hashes = [], []
        for label, _, entries in packs:
            e, _ = find_route(r, entries)
            if e and e['hash'] != int(r['fork_hash'], 16):
                code_hashes.append(f"0x{e['code_hash']:016x}ULL /* {label} */")
                guest_hashes.append(f"0x{e['hash']:08x} /* {label} */")
        doc = r.get('doc') or ''
        shape = 'true' if r.get('shape_match', bool(doc)) else 'false'
        lines.append(f"{{0x{int(r['fork_hash'], 16):08x}, \"{r['name']}\", \"{doc}\", "
                     f"{shape},\n             {{{', '.join(code_hashes)}}}, {{{', '.join(guest_hashes)}}}}},")
    out = '\n'.join(lines) + '\n'
    with open(args.output, 'w') as f:
        f.write(out)
    print(f'wrote {args.output}: {len(routes["routes"])} routes')


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    sub = ap.add_subparsers(dest='cmd', required=True)
    p = sub.add_parser('census'); p.add_argument('packs', nargs='+'); p.add_argument('--all', action='store_true')
    p.set_defaults(func=cmd_census)
    p = sub.add_parser('check'); p.add_argument('packs', nargs='+'); p.set_defaults(func=cmd_check)
    p = sub.add_parser('emit'); p.add_argument('packs', nargs='+'); p.add_argument('-o', '--output', required=True)
    p.set_defaults(func=cmd_emit)
    args = ap.parse_args()
    args.func(args)


if __name__ == '__main__':
    main()
