#!/usr/bin/env python3
"""Find call sites of an imported function in eboot.bin and the immediate loaded into
edi/esi just before each call (e.g. sceSysmoduleLoadModule module IDs).

Usage: find_import_calls.py EBOOT NID [NID...]
"""
import os, re, struct, sys
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "ref/bbport/scripts"))
from prepare import parse_self, unpack, span  # noqa: E402


def main():
    data = open(sys.argv[1], "rb").read()
    want = set(sys.argv[2:])
    elf, h, ph, _, _ = parse_self(data)
    dp = next(p for p in ph if p['type'] == 2)
    tags = {}
    for pos in range(dp['offset'], dp['offset'] + dp['filesz'], 16):
        t, v = unpack('<QQ', elf, pos)
        if t == 0:
            break
        tags.setdefault(t, v)
    lib = next(p for p in ph if p['type'] == 0x61000000)
    blob = span(elf, lib['offset'], lib['filesz'])
    strings = span(blob, tags[0x61000035], tags[0x61000037])
    syms = span(blob, tags[0x61000039], tags[0x6100003f])
    names = []
    for pos in range(0, len(syms), 24):
        nm = unpack('<I', syms, pos)[0]
        names.append(strings[nm:strings.index(0, nm)].decode())
    got = {}
    tbl = span(blob, tags[0x61000029], tags[0x6100002d])
    for pos in range(0, len(tbl), 24):
        off, info, add = unpack('<QQq', tbl, pos)
        n = names[info >> 32].split('#')[0]
        if n in want:
            got[off] = n
    text = next(p for p in ph if p['type'] == 1 and p['flags'] & 1)
    code = bytes(elf[text['offset']:text['offset'] + text['filesz']])
    base = text['vaddr']
    # PLT stubs: jmp qword [rip+disp32] = FF 25 disp32
    plt = {}
    for m in re.finditer(rb'\xff\x25', code):
        i = m.start()
        disp = struct.unpack_from('<i', code, i + 2)[0]
        tgt = base + i + 6 + disp
        if tgt in got:
            plt[base + i] = got[tgt]
    print("plt stubs:", {hex(k): v for k, v in plt.items()})
    for m in re.finditer(rb'\xe8', code):
        i = m.start()
        if i + 5 > len(code):
            continue
        tgt = base + i + 5 + struct.unpack_from('<i', code, i + 1)[0]
        if tgt not in plt:
            continue
        window = code[max(0, i - 24):i]
        imms = [hex(struct.unpack_from('<I', window, j + 1)[0]) for j in range(len(window) - 4)
                if window[j] == 0xbf]
        print("call %s at %#x  mov edi,imm candidates: %s" % (plt[tgt], base + i, imms[-1:] if imms else None))


if __name__ == "__main__":
    main()
