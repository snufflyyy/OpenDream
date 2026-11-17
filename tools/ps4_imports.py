#!/usr/bin/env python3
"""Analyse a PS4 game folder's executables (eboot.bin + sce_module/*.prx) for what they need
from the OS, and compare against the bbport C runtime.

Reuses bbport's SELF->ELF reconstruction (ref/bbport/scripts/prepare.py: parse_self, sfo, nid)
read-only. NID names come from shadPS4's aerolib table (tools/thirdparty/shadps4-v0.5.0) plus
bbport's import_names*.inc and names computed from every identifier string found in the
reference sources.

Usage: ps4_imports.py GAME_DIR OUT_JSON
"""
import collections
import glob
import json
import os
import re
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BBPORT = os.path.join(ROOT, "ref/bbport")
SHAD_OLD = os.path.join(ROOT, "tools/thirdparty/shadps4-v0.5.0")
SHAD_NEW = os.path.join(ROOT, "ref/shadps4")
sys.path.insert(0, os.path.join(BBPORT, "scripts"))
from prepare import parse_self, sfo, nid, span, unpack  # noqa: E402

ALPHABET = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+-'

PT_NAMES = {0: "NULL", 1: "LOAD", 2: "DYNAMIC", 3: "INTERP", 4: "NOTE", 7: "TLS",
            0x61000000: "SCE_DYNLIBDATA", 0x61000001: "SCE_PROCPARAM", 0x61000002: "SCE_MODULEPARAM",
            0x61000010: "SCE_RELRO", 0x6474e550: "GNU_EH_FRAME", 0x6FFFFF00: "SCE_COMMENT",
            0x6FFFFF01: "SCE_LIBVERSION"}
ET_NAMES = {0xFE00: "ET_SCE_EXEC", 0xFE10: "ET_SCE_DYNEXEC", 0xFE18: "ET_SCE_DYNAMIC",
            0xFE0C: "ET_SCE_STUBLIB", 0xFE01: "ET_SCE_REPLAY_EXEC", 0xFE08: "ET_SCE_RELEXEC"}
DT = dict(NEEDED=1, INIT=0xC, FINI=0xD, INIT_ARRAY=0x19, FINI_ARRAY=0x1A, PREINIT_ARRAY=0x20,
          INIT_ARRAYSZ=0x1B, FINI_ARRAYSZ=0x1C, PREINIT_ARRAYSZ=0x21, TEXTREL=0x16, FLAGS=0x1E,
          FINGERPRINT=0x61000007, ORIGINAL_FILENAME=0x61000009, MODULE_INFO=0x6100000D,
          NEEDED_MODULE=0x6100000F, MODULE_ATTR=0x61000011, EXPORT_LIB=0x61000013,
          IMPORT_LIB=0x61000015, EXPORT_LIB_ATTR=0x61000017, IMPORT_LIB_ATTR=0x61000019,
          HASH=0x61000025, PLTGOT=0x61000027, JMPREL=0x61000029, PLTREL=0x6100002B,
          PLTRELSZ=0x6100002D, RELA=0x6100002F, RELASZ=0x61000031, RELAENT=0x61000033,
          STRTAB=0x61000035, STRSZ=0x61000037, SYMTAB=0x61000039, SYMENT=0x6100003B,
          HASHSZ=0x6100003D, SYMTABSZ=0x6100003F)


def encode_id(value):
    r = ALPHABET[value & 63]
    value >>= 6
    while value:
        r = ALPHABET[value & 63] + r
        value >>= 6
    return r


# ------------------------------------------------------------------ NID name database
def load_names():
    names = {}
    src = {}
    p = os.path.join(SHAD_OLD, "src/core/aerolib/aerolib.inl")
    for m in re.finditer(r'STUB\("([^"]+)",\s*([^)\s]+)\)', open(p).read()):
        names.setdefault(m.group(1), m.group(2))
        src.setdefault(m.group(1), "aerolib")
    for inc in ("src/import_names.inc", "src/import_names_extra.inc"):
        for m in re.finditer(r'\{"([^"#]+)[^"]*","([^"]+)"\}', open(os.path.join(BBPORT, inc)).read()):
            names.setdefault(m.group(1), m.group(2))
            src.setdefault(m.group(1), "bbport")
    # fallback: function names of shadPS4 HLE registrations (current clone, then v0.5.0)
    rx = re.compile(r'LIB_(?:FUNCTION|OBJ)\(\s*"([^"]+)"\s*,\s*"[^"]+"\s*,\s*\w+\s*,\s*"[^"]+"\s*,\s*([\w:]+)')
    for base in (SHAD_NEW, SHAD_OLD):
        for dp, _, fs in os.walk(os.path.join(base, "src/core/libraries")):
            for f in fs:
                if f.endswith((".cpp", ".h")):
                    for m in rx.finditer(open(os.path.join(dp, f), errors="replace").read()):
                        if m.group(1) not in names:
                            names[m.group(1)] = m.group(2).split("::")[-1]
                            src[m.group(1)] = "shadps4_lib_function"
    return names, src


def harvest_identifiers():
    """Every quoted identifier string in reference sources -> NID candidates."""
    ids = set()
    roots = [os.path.join(BBPORT, "src"), os.path.join(BBPORT, "gpu")]
    for r in roots:
        for dp, _, fs in os.walk(r):
            for f in fs:
                if f.endswith((".c", ".cpp", ".h", ".inc", ".py")):
                    try:
                        t = open(os.path.join(dp, f), errors="replace").read()
                    except OSError:
                        continue
                    ids.update(re.findall(r'"([A-Za-z_][A-Za-z0-9_]{2,80})"', t))
    return ids


# ------------------------------------------------------------------ bbport coverage
# bbport scripts/link_modules.py DEFAULT_MODULES: linked statically into the image
BBPORT_LINKED = ("libc.prx", "libSceFios2.prx")
STUB_FUNCS = {"ok_void", "net_unreachable", "http_fail", "np_signed_out", "lib_init_id",
              "http_object"}


def bbport_coverage():
    """NID -> list of {how, where, func}. how in:
      runtime_nid  - exact NID check in src/*.c
      runtime_name - RuntimeExport name table in src/*.c (binds via import_names*.inc)
      gpu_hle      - NID table in gpu/platform (GnmDriver, VideoOut, AvPlayer, kernel equeue)
      bbnet_online - LIB_FUNCTION in gpu/bbnet20 (shadPS4 network/NP code; only with BB_ONLINE=1)"""
    cov = collections.defaultdict(list)

    def add(n, **kw):
        if not any(c["how"] == kw["how"] for c in cov[n]):
            cov[n].append(kw)

    paths = sorted(glob.glob(os.path.join(BBPORT, "src/**/*.c"), recursive=True))
    for path in paths:
        t = open(path, errors="replace").read()
        base = os.path.relpath(path, os.path.join(BBPORT, "src"))
        # {"NID#lib#mod", fn} tables compared with strcmp on the canonical (Bloodborne) spelling
        for m in re.finditer(r'\{\s*"([A-Za-z0-9+\-]{11})#[A-Za-z0-9+\-#]*"\s*,\s*&?(\w+)\s*\}', t):
            add(m.group(1), how="runtime_nid", where=base, func=m.group(2))
        for m in re.finditer(r'runtime_nid_is\(\s*name\s*,\s*"([^"]+)"\s*\)\s*\)\s*return\s*\(uintptr_t\)\s*&?(\w+)', t):
            add(m.group(1), how="runtime_nid", where=base, func=m.group(2))
        for m in re.finditer(r'runtime_nid_is\(\s*\w+\s*,\s*"([^"]+)"\s*\)', t):
            add(m.group(1), how="runtime_nid", where=base, func="?")
        for tm in re.finditer(r'RuntimeExport\s+\w+\s*\[\s*\]\s*=\s*\{(.*?)\};', t, re.S):
            for em in re.finditer(r'\{\s*"([^"]+)"\s*,\s*(\w+)\s*\}', tm.group(1)):
                add(nid(em.group(1)), how="runtime_name", where=base, func=em.group(2), name=em.group(1))
    # data imports matched by symbol name in runtime.c (runtime_symbol + strcmp)
    for m in re.finditer(r'strcmp\(symbol,\s*"(\w+)"\)', open(os.path.join(BBPORT, "src/runtime.c")).read()):
        add(nid(m.group(1)), how="runtime_name", where="runtime.c", func=m.group(1), name=m.group(1))
    gpu_tbl = re.compile(r'\{\s*"([A-Za-z0-9+\-]{11})"\s*,(?:[^{}]*?,)?\s*[Ff]n\(\s*([A-Za-z_0-9:]+)\s*\)')
    lib_fn = re.compile(r'LIB_(?:FUNCTION|OBJ)\(\s*"([^"]+)"\s*,\s*"([^"]+)"\s*,\s*\w+\s*,\s*"([^"]+)"\s*,\s*([^)]*)\)')
    for dp, _, fs in os.walk(os.path.join(BBPORT, "gpu")):
        online = "bbnet20" in dp
        for f in fs:
            if not f.endswith((".cpp", ".h", ".inc", ".cc")):
                continue
            full = os.path.join(dp, f)
            t = open(full, errors="replace").read()
            rel = os.path.relpath(full, BBPORT)
            if online:
                for m in lib_fn.finditer(t):
                    add(m.group(1), how="bbnet_online", where=rel, func=m.group(4).strip())
            else:
                for m in gpu_tbl.finditer(t):
                    add(m.group(1), how="gpu_hle", where=rel, func=m.group(2))
    return cov


def bbport_name_table():
    s = set()
    for inc in ("src/import_names.inc", "src/import_names_extra.inc"):
        for m in re.finditer(r'\{"([^"#]+)', open(os.path.join(BBPORT, inc)).read()):
            s.add(m.group(1))
    return s


def shadps4_hle():
    """NIDs with an HLE LIB_FUNCTION in shadPS4 (current clone if present, else v0.5.0)."""
    base = SHAD_NEW if os.path.isdir(os.path.join(SHAD_NEW, "src/core/libraries")) else SHAD_OLD
    out = {}
    rx = re.compile(r'LIB_(?:FUNCTION|OBJ)\(\s*"([^"]+)"\s*,\s*"([^"]+)"')
    for dp, _, fs in os.walk(os.path.join(base, "src/core/libraries")):
        for f in fs:
            if f.endswith((".cpp", ".h")):
                for m in rx.finditer(open(os.path.join(dp, f), errors="replace").read()):
                    out.setdefault(m.group(1), os.path.relpath(os.path.join(dp, f), base))
    return out, os.path.relpath(base, ROOT)


# ------------------------------------------------------------------ SELF / ELF
def self_info(data):
    if data[:4] != b'O\x15=\x1d':
        return dict(container="ELF")
    version, mode, endian, attribs, key_type, hsize, msize, fsize, count, flags = \
        struct.unpack_from("<BBBBIHHQHH", data, 4)
    segs = []
    enc = comp = 0
    for i in range(count):
        fl, off, sz, mem = struct.unpack_from("<QQQQ", data, 32 + i * 32)
        segs.append(dict(flags=hex(fl), offset=off, filesz=sz, memsz=mem, ph_index=(fl >> 20) & 0xFFF,
                         encrypted=bool(fl & 2), compressed=bool(fl & 8), blocked=bool(fl & 0x800),
                         signed=bool(fl & 4), has_digests=bool(fl & 0x10000)))
        if fl & 0x800:
            enc += bool(fl & 2)
            comp += bool(fl & 8)
    elf_off = 32 + count * 32
    phoff, = struct.unpack_from("<Q", data, elf_off + 0x20)
    phnum, = struct.unpack_from("<H", data, elf_off + 0x38)
    ext = elf_off + phoff + phnum * 56
    ext = (ext + 15) & ~15
    paid, ptype, app_ver, fw_ver = struct.unpack_from("<QQQQ", data, ext)
    return dict(container="SELF", self_version=version, mode=mode, key_type=key_type, header_size=hsize,
                meta_size=msize, file_size=fsize, flags=hex(flags), segment_count=count,
                encrypted_segments=enc, compressed_segments=comp, segments=segs,
                auth_id=hex(paid), program_type=hex(ptype), app_version=hex(app_ver), fw_version=hex(fw_ver))


def read_va(image_segments, elf, va, n):
    for p in image_segments:
        if p['vaddr'] <= va and va + n <= p['vaddr'] + p['filesz']:
            return bytes(elf[p['offset'] + va - p['vaddr']:p['offset'] + va - p['vaddr'] + n])
    return None


def read_cstr(image_segments, elf, va, limit=256):
    if not va:
        return None
    for p in image_segments:
        if p['vaddr'] <= va < p['vaddr'] + p['filesz']:
            o = p['offset'] + va - p['vaddr']
            b = bytes(elf[o:o + limit])
            return b.split(b"\0")[0].decode("ascii", "replace")
    return None


def analyse(path, names):
    data = open(path, "rb").read()
    si = self_info(data)
    elf, header, ph, segments, missing = parse_self(data)
    e_type = header[1]
    loads = [p for p in ph if p['type'] in (1, 0x61000010)]
    info = dict(file=os.path.basename(path), size=len(data), self=si,
                e_type=ET_NAMES.get(e_type, hex(e_type)), entry=hex(header[4]),
                missing_phdr_data=missing)
    info["program_headers"] = [dict(type=PT_NAMES.get(p['type'], hex(p['type'])), flags=
                                    ("R" if p['flags'] & 4 else "-") + ("W" if p['flags'] & 2 else "-") +
                                    ("X" if p['flags'] & 1 else "-"),
                                    offset=hex(p['offset']), vaddr=hex(p['vaddr']), filesz=p['filesz'],
                                    memsz=p['memsz'], align=hex(p['align'])) for p in ph]
    info["image_span"] = hex(max(p['vaddr'] + p['memsz'] for p in loads) - min(p['vaddr'] for p in loads))
    # TLS
    tls = [p for p in ph if p['type'] == 7]
    if tls:
        t = tls[0]
        info["tls"] = dict(vaddr=hex(t['vaddr']), filesz=t['filesz'], memsz=t['memsz'], align=t['align'])
    # dynamic
    dp = next(p for p in ph if p['type'] == 2)
    dyn = []
    for pos in range(dp['offset'], dp['offset'] + dp['filesz'], 16):
        tag, value = unpack('<QQ', elf, pos)
        if tag == 0:
            break
        dyn.append((tag, value))
    tags = {}
    for t, v in dyn:
        tags.setdefault(t, v)
    lib = next(p for p in ph if p['type'] == 0x61000000)
    blob = span(elf, lib['offset'], lib['filesz'])
    strings = span(blob, tags[DT['STRTAB']], tags[DT['STRSZ']])

    def string(off):
        return strings[off:strings.index(0, off)].decode('ascii', 'replace')

    needed, mods, libs, exp_libs = [], {}, {}, {}
    module_info = None
    for t, v in dyn:
        if t == DT['NEEDED']:
            needed.append(string(v))
        elif t in (DT['NEEDED_MODULE'], DT['MODULE_INFO']):
            ident = dict(name=string(v & 0xffffffff), version="%d.%d" % ((v >> 32) & 0xff, (v >> 40) & 0xff),
                         id=v >> 48)
            if t == DT['MODULE_INFO']:
                module_info = ident
            else:
                mods[encode_id(v >> 48)] = ident
        elif t in (DT['IMPORT_LIB'], DT['EXPORT_LIB']):
            ident = dict(name=string(v & 0xffffffff), version=(v >> 32) & 0xffff, id=v >> 48)
            (libs if t == DT['IMPORT_LIB'] else exp_libs)[encode_id(v >> 48)] = ident
    if module_info:
        mods.setdefault(encode_id(module_info['id']), module_info)
    info.update(module_info=module_info, dt_needed=needed,
                needed_modules=sorted(m['name'] for k, m in mods.items() if m is not module_info),
                import_libraries=sorted("%s (v%d)" % (l['name'], l['version']) for l in libs.values()),
                export_libraries=sorted(l['name'] for l in exp_libs.values()),
                original_filename=string(tags[DT['ORIGINAL_FILENAME']]) if DT['ORIGINAL_FILENAME'] in tags else None,
                fingerprint=bytes(blob[tags[DT['FINGERPRINT']]:tags[DT['FINGERPRINT']] + 20]).hex()
                if DT['FINGERPRINT'] in tags else None,
                textrel=DT['TEXTREL'] in tags,
                init=hex(tags[DT['INIT']]) if DT['INIT'] in tags else None,
                init_array=tags.get(DT['INIT_ARRAYSZ'], 0) // 8, preinit_array=tags.get(DT['PREINIT_ARRAYSZ'], 0) // 8)
    # symbols
    syms_raw = span(blob, tags[DT['SYMTAB']], tags[DT['SYMTABSZ']])
    syms = []
    for pos in range(0, len(syms_raw), 24):
        nm, sinfo, other, shndx, value, sz = unpack('<IBBHQQ', syms_raw, pos)
        syms.append(dict(name=string(nm), bind=sinfo >> 4, type=sinfo & 15, shndx=shndx, value=value, size=sz))
    # relocations
    referenced = collections.Counter()
    reloc_kinds = collections.Counter()
    relmap = {}
    for ot, st in ((DT['JMPREL'], DT['PLTRELSZ']), (DT['RELA'], DT['RELASZ'])):
        if ot not in tags:
            continue
        tbl = span(blob, tags[ot], tags[st])
        for pos in range(0, len(tbl), 24):
            off, rinfo, add = unpack('<QQq', tbl, pos)
            kind = rinfo & 0xffffffff
            reloc_kinds[kind] += 1
            referenced[rinfo >> 32] += 1
            if kind == 8:
                relmap[off] = add & 0xFFFFFFFFFFFFFFFF
            elif kind in (1, 6) and syms[rinfo >> 32]['shndx']:
                relmap[off] = (syms[rinfo >> 32]['value'] + add) & 0xFFFFFFFFFFFFFFFF
    info["relocation_kinds"] = {str(k): v for k, v in sorted(reloc_kinds.items())}
    # proc / module param (pointer fields are RELATIVE relocations: read through relmap)
    def rd(va, n):
        return read_va(loads, elf, va, n)

    def rd8(va):
        if va in relmap:
            return relmap[va]
        b = rd(va, 8)
        return struct.unpack("<Q", b)[0] if b else None

    def rd4(va):
        b = rd(va, 4)
        return struct.unpack("<I", b)[0] if b else None

    for typ, key in ((0x61000001, "proc_param"), (0x61000002, "module_param")):
        pp = [p for p in ph if p['type'] == typ]
        if not pp:
            continue
        p = pp[0]
        base = p['vaddr']
        raw = bytes(elf[p['offset']:p['offset'] + p['filesz']])
        d = dict(vaddr=hex(base), size=p['filesz'])
        if len(raw) >= 0x18:
            size, magic, entries, sdk = struct.unpack_from("<QIIQ", raw, 0)
            d.update(struct_size=hex(size), magic=raw[8:12].decode("ascii", "replace"),
                     entry_count=entries, sdk_version=hex(sdk))
        if key == "proc_param":
            # OrbisProcParam (shadPS4): size, magic, entry_count, sdk_version, process_name,
            # main_thread_name, main_thread_prio*, main_thread_stack_size*, libc_param*, mem_param*,
            # fs_param*, process_preload_enabled*, unknown
            labels = ["process_name", "user_main_thread_name", "user_main_thread_priority",
                      "user_main_thread_stack_size", "libc_param", "kernel_mem_param",
                      "kernel_fs_param", "process_preload_enabled", "unknown"]
            fields = {}
            for i, lab in enumerate(labels):
                off = 0x18 + 8 * i
                if off + 8 > p['filesz']:
                    break
                v = rd8(base + off)
                fields[lab] = hex(v) if v is not None else None
                if not v:
                    continue
                if lab in ("process_name", "user_main_thread_name"):
                    fields[lab + "_str"] = read_cstr(loads, elf, v)
                elif lab in ("user_main_thread_priority", "process_preload_enabled"):
                    fields[lab + "_value"] = hex(rd4(v) or 0)
                elif lab == "user_main_thread_stack_size":
                    fields[lab + "_value"] = hex(rd4(v) or 0)
                elif lab == "kernel_mem_param":
                    # OrbisKernelMemParam: size; u64* extended_page_table; u64* flexible_memory_size;
                    # u8* extended_memory_1; u64* extended_gpu_page_table; u8* extended_memory_2;
                    # u64* extended_cpu_page_table
                    sz = rd8(v)
                    vals = dict(size=hex(sz or 0))
                    for k, lname in enumerate(["extended_page_table", "flexible_memory_size", "extended_memory_1",
                                               "extended_gpu_page_table", "extended_memory_2",
                                               "extended_cpu_page_table"]):
                        if 8 + 8 * k >= (sz or 0):
                            break
                        ptr = rd8(v + 8 + 8 * k)
                        if ptr:
                            val = rd(ptr, 1)[0] if lname.startswith("extended_memory") else rd8(ptr)
                            vals[lname] = hex(val) if val is not None else "ptr " + hex(ptr)
                    fields["kernel_mem_param_values"] = vals
                elif lab == "kernel_fs_param":
                    sz = rd8(v)
                    fp = dict(size=hex(sz or 0))
                    if (sz or 0) >= 0x10:
                        ptr = rd8(v + 8)  # u64* extended_fs_cache_size? (rarely set)
                        fp["field1"] = hex(rd8(ptr) or 0) if ptr else "null"
                    fields["kernel_fs_param_values"] = fp
                elif lab == "libc_param":
                    # SceLibcParam: size; u32 entry_count; u32 SceLibcInternalHeap(ver>=1);
                    # size_t* sceLibcHeapSize; u32* sceLibcHeapDelayedAlloc; u32* sceLibcHeapExtendedAlloc;
                    # u32* sceLibcHeapInitialSize; SceLibcMallocReplace*; SceLibcNewReplace*;
                    # size_t* sceLibcHeapHighAddressAlloc; u32* Need_sceLibc; size_t* sceLibcHeapMemoryLock;
                    # size_t* sceKernelInternalMemorySize; ...
                    sz = rd8(v)
                    lp = dict(size=hex(sz or 0), entry_count=rd4(v + 8))
                    names_ = ["sceLibcHeapSize", "sceLibcHeapDelayedAlloc", "sceLibcHeapExtendedAlloc",
                              "sceLibcHeapInitialSize", "_sceLibcMallocReplace", "_sceLibcNewReplace",
                              "sceLibcHeapHighAddressAlloc", "Need_sceLibc", "sceLibcHeapMemoryLock",
                              "sceKernelInternalMemorySize", "_sceLibcMallocReplaceForTls"]
                    for k, nm_ in enumerate(names_):
                        off2 = 0x10 + 8 * k
                        if off2 + 8 > (sz or 0):
                            break
                        ptr = rd8(v + off2)
                        if not ptr:
                            continue
                        if nm_.startswith("_sce"):
                            lp[nm_] = "set (%s)" % hex(ptr)
                        elif nm_ in ("sceLibcHeapSize", "sceLibcHeapHighAddressAlloc", "sceKernelInternalMemorySize",
                                     "sceLibcHeapMemoryLock"):
                            val = rd8(ptr)
                            lp[nm_] = hex(val) if val is not None else "ptr " + hex(ptr)
                        else:
                            val = rd4(ptr)
                            lp[nm_] = hex(val) if val is not None else "ptr " + hex(ptr)
                    fields["libc_param_values"] = lp
            d["fields"] = fields
        info[key] = d
    imports = collections.defaultdict(list)
    exports = collections.defaultdict(list)
    for i, s in enumerate(syms):
        parts = s['name'].split('#')
        if len(parts) != 3:
            continue
        n, lk, mk = parts
        if s['shndx'] == 0:
            L = libs.get(lk)
            M = mods.get(mk)
            lname = L['name'] if L else "lib#" + lk
            imports[lname].append(dict(nid=n, name=names.get(n), type={1: "object", 2: "func", 6: "tls"}.get(s['type'], s['type']),
                                       bind={0: "local", 1: "global", 2: "weak"}.get(s['bind'], s['bind']),
                                       module=M['name'] if M else mk, refs=referenced.get(i, 0)))
        else:
            L = exp_libs.get(lk)
            exports[L['name'] if L else "lib#" + lk].append(dict(nid=n, name=names.get(n), value=hex(s['value'])))
    info["imports"] = {k: sorted(v, key=lambda x: (x['name'] or "~" + x['nid'])) for k, v in sorted(imports.items())}
    info["exports"] = {k: len(v) for k, v in exports.items()}
    info["_exports_full"] = exports
    info["_elf"] = elf
    info["_loads"] = loads
    return info


def scan_strings(elf, words):
    found = {}
    for w in words:
        found[w] = elf.count(w.encode())
    return found


def main():
    game, out_json = sys.argv[1], sys.argv[2]
    names, name_src = load_names()
    for ident in harvest_identifiers():
        names.setdefault(nid(ident), ident)
    cov = bbport_coverage()
    bb_names = bbport_name_table()
    shad, shad_base = shadps4_hle()
    params = sfo(open(os.path.join(game, "sce_sys/param.sfo"), "rb").read())
    files = [os.path.join(game, "eboot.bin")] + sorted(glob.glob(os.path.join(game, "sce_module/*")))
    results = {}
    for f in files:
        results[os.path.relpath(f, game)] = analyse(f, names)

    # libraries the game links statically from its own sce_module (bbport style): exports of those
    shipped_exports = {}
    for k, r in results.items():
        if k.startswith("sce_module/"):
            for libname, lst in r["_exports_full"].items():
                for e in lst:
                    shipped_exports.setdefault(e["nid"], (k, libname))

    def status(entry, lib):
        n = entry["nid"]
        impls = cov.get(n, [])
        offline = [c for c in impls if c["how"] != "bbport_online" and c["how"] != "bbnet_online"]
        online = [c for c in impls if c["how"] == "bbnet_online"]
        if offline:
            c = offline[0]
            st = "bbport:" + c["how"]
            if c["how"] == "runtime_name" and n not in bb_names:
                st += "(needs import_names entry)"
            if c.get("func") in STUB_FUNCS:
                st += ":stub"
            if online:
                st += "+online"
            return st, c
        if online:
            return "bbport:bbnet_online_only", online[0]
        if n in shipped_exports:
            mod = shipped_exports[n][0]
            if os.path.basename(mod) in BBPORT_LINKED:
                return "game_module:" + mod, None
            return "game_module_unlinked:" + mod, None
        return "missing", None

    out = dict(title=params, imports={}, files={})
    for k, r in results.items():
        per_lib = {}
        for lib, lst in r["imports"].items():
            rows = []
            for e in lst:
                st, c = status(e, lib)
                rows.append(dict(nid=e["nid"], name=e["name"],
                                 implemented_in_bbport=(st.startswith("bbport") and "online_only" not in st)
                                 or st.startswith("game_module:"),
                                 status=st, bbport_impl=(c["where"] + ":" + c["func"]) if c else None,
                                 shadps4_hle=shad.get(e["nid"]), type=e["type"], module=e["module"],
                                 refs=e["refs"]))
            per_lib[lib] = rows
        out["imports"][k] = per_lib
        r2 = {kk: vv for kk, vv in r.items() if not kk.startswith("_") and kk != "imports"}
        r2["import_counts"] = {lib: len(v) for lib, v in per_lib.items()}
        out["files"][k] = r2
    out["notes"] = dict(
        status_legend={
            "bbport:runtime_nid": "exact NID check in ref/bbport/src/*.c",
            "bbport:runtime_name": "name in a RuntimeExport table (bbport binds NID->name via import_names*.inc)",
            "bbport:gpu_hle": "NID table in ref/bbport/gpu/platform (GnmDriver/VideoOut/AvPlayer/equeue)",
            "bbport:bbnet_online_only": "only in ref/bbport/gpu/bbnet20 (shadPS4 NP/net code, BB_ONLINE=1); counted as NOT implemented",
            "+online": "also overridden by bbnet20 when BB_ONLINE=1",
            ":stub": "bbport maps it to a generic stub (ok_void/net_unreachable/...)",
            "game_module:*": "exported by libc.prx/libSceFios2.prx from sce_module/, which bbport links statically",
            "game_module_unlinked:*": "exported by another shipped module (libSceJobManager.prx); bbport does not link it yet - counted as NOT implemented",
            "missing": "no implementation found in bbport",
        },
        shadps4_reference=shad_base)
    eb = results["eboot.bin"]
    out["files"]["eboot.bin"]["string_scan"] = scan_strings(
        eb["_elf"], ["sceKernelLoadStartModule", "sceKernelDlsym", ".sprx", ".prx", "/app0/", "/data/",
                     "/hostapp/", "/download0/", "/temp0/", "/savedata0/", "/addcont", "JIT"])
    os.makedirs(os.path.dirname(out_json), exist_ok=True)
    with open(out_json, "w") as fo:
        json.dump(out, fo, indent=1)
    print("wrote", out_json)


if __name__ == "__main__":
    main()
