#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 OpenDream contributors
# SPDX-License-Identifier: GPL-2.0-or-later
"""Experimental internal scene resolution as one BBPATCH2 data write (docs/dreams/RUNNING.md).

python3 -I tools/dreams_render_resolution.py EBOOT_BIN --resolution 1280x720
    --patch DIR/render-resolution.bbpatch --report DIR/render-resolution.json

Offline and fail-closed: the selected SELF is parsed, the mode-indexed dimension constructor is
found only by unique semantic evidence (decoded instructions, NID-anchored
sceVideoOutSetBufferAttribute/sceVideoOutRegisterBuffers calls, GNU_EH_FRAME FDE bounds), and any
missing or ambiguous pattern aborts before anything is published. No fixed game addresses or
version hashes. The write changes the constructor's shared default dimension vector, so the
mode-0 default, the mode-15 load, an allocation floor and a comparison all move together; indirect
uses and scene/depth/viewport coherence are not proven for every pass. The host window and the
output image stay at their size: only the internal drawing extent shrinks. Experimental.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True
# SELF/ELF parsing and the PS4 NID hash are shared with preparation; never a second copy.
sys.path.insert(0, str(Path(__file__).resolve().parent.parent / 'scripts'))
from prepare import parse_self, span, unpack, nid

ORIGINAL = struct.pack('<IIII', 1920, 1080, 0, 0)
NAMES = ('sceVideoOutSetBufferAttribute', 'sceVideoOutRegisterBuffers')
OBJDUMP = shutil.which('objdump')
SUPPORTED = ((1280, 720), (1920, 1080))


def resolution(value):
    """WIDTHxHEIGHT of an exact 16:9 extent; only verified values are accepted."""
    if not re.fullmatch(r'[0-9]+x[0-9]+', value):
        raise ValueError('expected WIDTHxHEIGHT')
    w, h = map(int, value.split('x'))
    if not (640 <= w <= 1920 and 360 <= h <= 1080 and w * 9 == h * 16):
        raise ValueError('expected exact 16:9 between 640x360 and 1920x1080')
    if (w, h) not in SUPPORTED:
        raise ValueError('supported values: 1280x720 (verified) and 1920x1080 (explicit default)')
    return w, h


def output_paths(patch, report, source):
    """Both outputs direct children of one existing directory, distinct and never the input."""
    paths = [Path(p).resolve() for p in (patch, report)]
    source = Path(source).resolve()
    if paths[0] == paths[1]:
        raise ValueError('--patch and --report must be different files')
    directory = paths[0].parent
    if paths[1].parent != directory:
        raise ValueError('--patch and --report must be in the same directory')
    if not directory.is_dir(follow_symlinks=True):
        raise ValueError(f'output directory does not exist: {directory}')
    if source in paths:
        raise ValueError('an output would overwrite the input')
    if any(p.exists() for p in paths):
        raise ValueError('output already exists')
    return paths[0], paths[1], directory


def mapped_bytes(elf, loads, va, n):
    hits = [p for p in loads if p['vaddr'] <= va and va + n <= p['vaddr'] + p['filesz']]
    if len(hits) != 1:
        raise ValueError('not uniquely file-backed')
    p = hits[0]
    return bytes(span(elf, p['offset'] + va - p['vaddr'], n))


def metadata(elf, ph):
    dp = next(p for p in ph if p['type'] == 2)
    lib = next(p for p in ph if p['type'] == 0x61000000)
    tags = {}
    for pos in range(dp['offset'], dp['offset'] + dp['filesz'], 16):
        tag, value = unpack('<QQ', elf, pos)
        if not tag: break
        if tag in tags and tag in (0x61000035,0x61000037,0x61000039,0x6100003f,0x61000029,0x6100002d,0x6100002f,0x61000031):
            raise ValueError('duplicate relevant dynamic tag')
        tags[tag] = value
    blob = span(elf, lib['offset'], lib['filesz'])
    strings = span(blob, tags[0x61000035], tags[0x61000037])
    syms = span(blob, tags[0x61000039], tags[0x6100003f])
    if len(syms) % 24: raise ValueError('malformed symbols')
    names = []
    for pos in range(0, len(syms), 24):
        off, _, _, shndx, _, _ = unpack('<IBBHQQ', syms, pos)
        if off >= len(strings): raise ValueError('bad symbol name')
        end = strings.find(b'\0', off)
        if end < 0: raise ValueError('unterminated symbol')
        names.append((strings[off:end].decode('ascii').split('#')[0], shndx))
    got, relocs = {}, []
    wanted = {nid(n): n for n in NAMES}
    for a, b in ((0x61000029, 0x6100002d), (0x6100002f, 0x61000031)):
        table = span(blob, tags[a], tags[b])
        if len(table) % 24: raise ValueError('malformed relocations')
        for pos in range(0, len(table), 24):
            target, info, _ = unpack('<QQq', table, pos)
            relocs.append(target)
            sym = info >> 32
            if sym >= len(names): raise ValueError('bad relocation symbol')
            name, section = names[sym]
            if name in wanted and section == 0:
                got[target] = wanted[name]
    if set(got.values()) != set(NAMES): raise ValueError('missing VideoOut import')
    return got, relocs


def function_ranges(elf, ph, loads):
    """Validate GNU .eh_frame_hdr table and each referenced zR/PC-relative FDE."""
    headers = [p for p in ph if p['type'] == 0x6474e550]
    if len(headers) != 1: raise ValueError('expected one GNU_EH_FRAME header')
    p = headers[0]; base = p['vaddr']
    hdr = mapped_bytes(elf, loads, base, p['filesz'])
    if len(hdr) < 12 or hdr[:4] != b'\x01\x1b\x03\x3b':
        raise ValueError('unsupported eh_frame_hdr encodings')
    eh = base + 4 + struct.unpack_from('<i', hdr, 4)[0]
    count = struct.unpack_from('<I', hdr, 8)[0]
    if not count or count > (len(hdr)-12)//8: raise ValueError('invalid FDE table count')
    mapped_bytes(elf, loads, eh, 8)
    def rec(va):
        length, ident = struct.unpack('<II', mapped_bytes(elf, loads, va, 8))
        if length < 4 or length == 0xffffffff or length > 1024*1024:
            raise ValueError('unsupported frame record length')
        return mapped_bytes(elf, loads, va, length+4), ident
    def leb(data, pos):
        value=0
        for i in range(10):
            if pos >= len(data): raise ValueError('truncated LEB')
            b=data[pos]; pos+=1; value |= (b & 127) << (7*i)
            if not b & 128: return value,pos
        raise ValueError('oversized LEB')
    ranges=[]; cie_cache=set(); indexed=set(); previous_end=0
    for i in range(count):
        start = base + struct.unpack_from('<i',hdr,12+i*8)[0]
        fde = base + struct.unpack_from('<i',hdr,16+i*8)[0]
        if fde in indexed: raise ValueError('duplicate FDE')
        indexed.add(fde)
        data, delta = rec(fde)
        if not delta or fde < eh or delta > fde+4-eh: raise ValueError('bad FDE/CIE pointer')
        cie=fde+4-delta
        if cie not in cie_cache:
            c, ident = rec(cie)
            if ident != 0 or len(c)<13 or c[8:12] != b'\x01zR\x00':
                raise ValueError('unsupported CIE version/augmentation')
            pos=12
            _,pos=leb(c,pos); _,pos=leb(c,pos); _,pos=leb(c,pos)
            size,pos=leb(c,pos)
            if size != 1 or pos+size>len(c) or c[pos]!=0x1b:
                raise ValueError('unsupported CIE pointer encoding')
            cie_cache.add(cie)
        if len(data)<17: raise ValueError('short FDE')
        actual=fde+8+struct.unpack_from('<i',data,8)[0]
        size=struct.unpack_from('<I',data,12)[0]
        auglen,pos=leb(data,16)
        if pos+auglen>len(data) or actual!=start or not size or start<previous_end:
            raise ValueError('inconsistent FDE table')
        if not any(q['flags'] & 1 and q['vaddr'] <= start and start+size <= q['vaddr']+q['filesz'] for q in loads):
            raise ValueError('FDE outside executable file-backed LOAD')
        ranges.append((start,start+size)); previous_end=start+size
    # This supported layout places the complete .eh_frame directly before its header.
    # A zero encoded initial-location denotes a discarded/linker-tombstone FDE.
    cursor=eh; seen=set()
    while cursor < base:
        length, = struct.unpack('<I',mapped_bytes(elf,loads,cursor,4))
        if length == 0:
            cursor += 4
            break
        data, ident=rec(cursor)
        if cursor+len(data)>base: raise ValueError('frame record crosses header')
        if ident:
            if len(data)<17: raise ValueError('short FDE')
            raw,=struct.unpack_from('<i',data,8)
            if raw:
                if cursor not in indexed: raise ValueError('unindexed live FDE')
                seen.add(cursor)
            elif cursor in indexed:
                raise ValueError('indexed tombstone FDE')
        cursor+=len(data)
    if cursor != base or seen != indexed: raise ValueError('incomplete eh_frame table')
    return ranges


def disassemble(code, base):
    # A temporary file in the system temp dir; NamedTemporaryFile is 0600 and removed on close.
    if OBJDUMP is None: raise ValueError('objdump not found: GNU binutils is required')
    with tempfile.NamedTemporaryFile(prefix='.dreams-objdump-', delete=True) as f:
        f.write(code); f.flush()
        p = subprocess.run([OBJDUMP, '-D', '-b', 'binary', '-m', 'i386:x86-64',
                            '--insn-width=16', '--adjust-vma='+hex(base), f.name],
                           check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
                           env={**os.environ, 'LC_ALL': 'C'})
    ins = []
    for line in p.stdout.splitlines():
        m = re.match(r'^\s*([0-9a-f]+):\s+((?:[0-9a-f]{2} )+)\s*([^\s]+)\s*(.*?)\s*$', line)
        if m:
            addr = int(m[1], 16); raw = bytes.fromhex(m[2]); op = m[4].split('#')[0].strip()
            target = re.search(r'#\s*(?:0x)?([0-9a-f]+)', m[4])
            ins.append((addr, raw, m[3], op, int(target[1],16) if target else None))
    if not ins or ins[0][0] != base: raise ValueError('disassembly failed')
    return ins


def call_target(x):
    return int(x[3], 16) if x[2] == 'call' and re.fullmatch(r'0x[0-9a-f]+', x[3]) else None


def branch_target(x):
    return int(x[3],16) if re.fullmatch(r'0x[0-9a-f]+',x[3]) else None


def has(seq, mnemonic, operands):
    return any(x[2] == mnemonic and x[3] == operands for x in seq)


def validate_cluster(ins, lit, elf, loads, got):
    # Decoding is rooted at a standard frame prologue, not at a raw xref candidate.
    refs = [x for x in ins if x[4] == lit]
    if len(refs) != 4 or [x[2] for x in refs] != ['vmovdqu','vmovdqu','vpmaxud','vmovdqu']:
        raise ValueError('not four ordered decoded references')
    if not (refs[0][3].endswith('(%rip),%xmm0') and refs[1][3].endswith('(%rip),%xmm0') and
            refs[2][3].endswith('(%rip),%xmm0,%xmm0') and refs[3][3].endswith('(%rip),%xmm1')):
        raise ValueError('unexpected reference operands')
    idx = {x[0]: i for i,x in enumerate(ins)}
    a,b,c,d = [idx[x[0]] for x in refs]
    before = ins[:a]
    # Establish a single ordered dispatch with no branches or register clobbers.
    leas = [i for i,x in enumerate(before) if x[2]=='lea' and x[3].endswith('(%rip),%rcx') and x[4] is not None]
    steps = [leas]
    for op,args in (('mov','%edi,%eax'),('movslq','(%rcx,%rax,4),%rax'),('add','%rcx,%rax'),('jmp','*%rax')):
        steps.append([i for i,x in enumerate(before) if x[2:4]==(op,args)])
    if any(len(q)!=1 for q in steps): raise ValueError('ambiguous dispatch')
    li,mi,si,ai,ji=[q[0] for q in steps]
    if not li < mi < si < ai < ji or ji != a-1: raise ValueError('reordered dispatch')
    groups = ((('%rcx','%ecx','%cx','%cl','%ch'),li+1,si),
              (('%rax','%eax','%ax','%al','%ah'),mi+1,si),
              (('%rax','%eax','%ax','%al','%ah','%rcx','%ecx','%cx','%cl','%ch'),si+1,ai),
              (('%rax','%eax','%ax','%al','%ah'),ai+1,ji),
              (('%rdi','%edi','%di','%dil'),0,ji))
    for aliases,start,end in groups:
        for x in ins[start:end]:
            if x[2] not in ('push','mov','movb','movl','movabs','lea','and','sub','add','movslq','vmovq','vmovdqu') or (x[2] != 'push' and x[3].split(',')[-1] in aliases):
                raise ValueError('dispatch register clobber or branch')
    leas=[ins[li]]
    table = leas[0][4]
    jump0 = table + struct.unpack('<i', mapped_bytes(elf, loads, table, 4))[0]
    if jump0 != refs[0][0]: raise ValueError('mode 0 does not select first reference')
    stores=[x for x in ins[a+1:b] if x[2]=='vmovq' and x[3].startswith('%xmm0,') and x[4] is not None]
    if len(stores)!=1 or c<1 or ins[c-1][2]!='vmovq' or ins[c-1][4]!=stores[0][4] or not ins[c-1][3].endswith('(%rip),%xmm0'):
        raise ValueError('allocation floor does not consume packed dimension store')
    if not has(ins[a:b], 'cmp', '$0xf,%edi') or not any(x[2]=='jne' and x[0] < refs[1][0] for x in ins[a:b]):
        raise ValueError('missing mode 15 branch')
    if not (has(ins[c:c+15], 'vmovd', '%xmm0,%eax') and has(ins[c:c+20], 'vpextrd', '$0x1,%xmm0,%ebx') and has(ins[c:c+20], 'imul', '%eax,%ebx')):
        raise ValueError('missing allocation product')
    if not has(ins[d:d+10], 'vpcmpgtd', '%xmm0,%xmm1,%xmm1'):
        raise ValueError('missing vector comparison')
    extracts = [i for i in range(b,c) if ins[i][2:4] == ('vmovd','%xmm0,%r15d')]
    if len(extracts)!=1: raise ValueError('missing width extraction')
    e=extracts[0]
    # Follow the mode-0 branch conditions: (mode-9) is unsigned >= 6,
    # mode is neither 16 nor 15, and execution joins the extraction path.
    q=ins[a:b]
    lea=[x for x in q if x[2:4]==('lea','-0x9(%rdi),%ecx')]
    cmps=[x for x in q if x[2:4]==('cmp','$0x6,%ecx')]
    jae=[x for x in q if x[2]=='jae']
    if len(lea)!=1 or len(cmps)!=1 or len(jae)!=1 or branch_target(jae[0]) not in idx or not (idx[lea[0][0]] < idx[cmps[0][0]] and idx[cmps[0][0]] < idx[jae[0][0]] and all(x[2] in ('mov','movl','movabs','lea') for x in ins[idx[cmps[0][0]]+1:idx[jae[0][0]]])):
        raise ValueError('mode 0 initial branch unproven')
    for k,x in enumerate(ins[a+1:idx[jae[0][0]]],a+1):
        aliases=('%rdi','%edi','%di','%dil') + (('%rcx','%ecx','%cx','%cl','%ch') if k>idx[lea[0][0]] else ())
        if x[2] not in ('lea','mov','movl','movabs','vmovq','cmp') or x[3].split(',')[-1]=='%xmm0' or (x[2] != 'cmp' and x[3].split(',')[-1] in aliases):
            raise ValueError('mode 0 initial vector flow unproven')
    j=idx[branch_target(jae[0])]; modepath=ins[j:b]
    if len(modepath)<4 or modepath[0][2:4]!=('cmp','$0x10,%edi') or modepath[1][2]!='je' or modepath[2][2:4]!=('cmp','$0xf,%edi') or modepath[3][2]!='jne':
        raise ValueError('mode 0 branch chain unproven')
    if branch_target(modepath[1]) not in idx:
        raise ValueError('mode 16 branch target outside function')
    join=branch_target(modepath[3])
    if join not in idx or not (b < idx[join] <= e): raise ValueError('mode 0 join unproven')
    for x in ins[idx[join]:e]:
        if x[2].startswith('j') or x[2]=='call' or x[3].split(',')[-1]=='%xmm0':
            raise ValueError('mode 0 vector flow unproven')
    if ins[e+1][2:4] != ('vpextrd','$0x1,%xmm0,%r14d'):
        raise ValueError('missing height extraction')
    # Resolve PLT entries by disassembling each decoded direct call target, then matching GOT.
    calls = []
    for i,x in enumerate(ins):
        target=call_target(x)
        if target is None: continue
        try: stub=disassemble(mapped_bytes(elf,loads,target,16),target)[0]
        except (ValueError, subprocess.SubprocessError): continue
        if stub[2]=='jmp' and stub[4] in got and stub[3].startswith('*'):
            calls.append((i,got[stub[4]],target,stub[4]))
    attrs = [q for q in calls if q[1]==NAMES[0]]
    regs = [q for q in calls if q[1]==NAMES[1]]
    if len(attrs)!=1 or len(regs)!=1 or not (c < attrs[0][0] < regs[0][0] < d):
        raise ValueError('VideoOut call anchors missing/ambiguous')
    ai=attrs[0][0]
    widths=[i for i in range(e+2,ai) if ins[i][2:4] == ('mov','%r15d,%r8d')]
    heights=[i for i in range(e+2,ai) if ins[i][2:4] == ('mov','%r14d,%r9d')]
    if len(widths)!=1 or len(heights)!=1: raise ValueError('missing VideoOut width/height arguments')
    # Reject writes to source/argument registers along the linear decoded window; calls preserve r14/r15.
    for aliases,start,end,volatile in ((('%r15','%r15d','%r15w','%r15b'),e+1,widths[0],False),(('%r14','%r14d','%r14w','%r14b'),e+2,heights[0],False),(('%r8','%r8d','%r8w','%r8b'),widths[0]+1,ai,True),(('%r9','%r9d','%r9w','%r9b'),heights[0]+1,ai,True)):
        for x in ins[start:end]:
            if (volatile and x[2]=='call') or (x[2] not in ('cmp','test','push') and x[3].split(',')[-1] in aliases):
                raise ValueError('dimension register clobber')
    return {'constructor':hex(ins[0][0]), 'mode0_table':hex(table), 'roles':dict(zip(('mode0_load','mode15_load','allocation_floor','comparison_load'),(hex(x[0]) for x in refs))), 'calls':{name:{'instruction':hex(ins[i][0]),'plt':hex(t),'got':hex(g)} for i,name,t,g in calls if name in NAMES}, 'width_extract':hex(ins[e][0]), 'height_extract':hex(ins[e+1][0]), 'width_argument':hex(ins[widths[0]][0]), 'height_argument':hex(ins[heights[0]][0])}


def find(elf, ph):
    loads = [p for p in ph if p['type'] in (1,0x61000010)]
    for p in loads:
        if p['filesz'] > p['memsz']: raise ValueError('invalid LOAD')
        span(elf,p['offset'],p['filesz'])
    got, relocs = metadata(elf,ph)
    literals=[]
    for p in loads:
        data=bytes(span(elf,p['offset'],p['filesz'])); off=0
        while True:
            off=data.find(ORIGINAL,off)
            if off<0: break
            literals.append(p['vaddr']+off); off+=1
    if len(set(literals)) != len(literals): raise ValueError('overlapping LOADs')
    ranges=function_ranges(elf,ph,loads)
    matches=[]
    # A literal inside a declared function range is code/data in a live function, not the
    # dimension vector. The predicate depends only on the literal, so evaluate it once.
    candidates=[lit for lit in literals if not any(a < lit+16 and lit < b for a,b in ranges)]
    # Select literal references only as a prefilter; FDEs define decoding boundaries.
    for start,end in ranges:
        if end-start>4096: continue
        code=mapped_bytes(elf,loads,start,end-start)
        for lit in candidates:
            rawhits=0
            for m in re.finditer(rb'\xc5\xfa\x6f[\x05\x0d]',code):
                i=m.start()
                if i+8<=len(code) and start+i+8+struct.unpack_from('<i',code,i+4)[0]==lit:
                    rawhits+=1
            if rawhits<3: continue
            try:
                ins=disassemble(code,start)
                result=validate_cluster(ins,lit,elf,loads,got)
                mapped_bytes(elf,loads,lit,16)
                if any(t<lit+16 and lit<t+8 for t in relocs): raise ValueError('relocation overlap')
                result['function_range']=[hex(start),hex(end)]
                result['literal_outside_all_fde_ranges']=True
                result['fde_range_count']=len(ranges)
                matches.append((lit,result))
            except (ValueError, IndexError, subprocess.SubprocessError): continue
    if len(matches)!=1: raise ValueError(f'expected unique verified cluster, found {len(matches)}')
    lit,result=matches[0]
    p = next(p for p in loads if p['vaddr'] <= lit and lit+16 <= p['vaddr']+p['filesz'])
    result.update(literal=hex(lit), original=[1920,1080,0,0], mapped_span=[hex(p['vaddr']),hex(p['vaddr']+p['filesz'])], no_relocation_overlap=True, literal_occurrences=[hex(x) for x in literals])
    return result


def encode_patch(va, w, h):
    return struct.pack('<8sQQQQIIII',b'BBPATCH2',0,1,va,16,w,h,0,0)


def write_and_sync(f, data):
    f.write(data)
    f.flush()
    os.fsync(f.fileno())


def publish(patch_path, patch, report_path, report):
    """Publish report first, patch last; a patch is the completed-pair marker.

    Hard links are exclusive, so an existing file is never replaced. The temporary files live in
    the outputs' directory (same filesystem, private 0600) and are removed on every path.
    """
    patch_path, report_path = Path(patch_path), Path(report_path)
    directory = patch_path.parent
    if report_path.parent != directory:
        raise ValueError('--patch and --report must be in the same directory')
    temps=[]; linked=[]
    try:
        for data in (patch, report):
            fd,name=tempfile.mkstemp(prefix='.publish-',dir=directory)
            temps.append(Path(name))
            with os.fdopen(fd,'wb') as f:
                write_and_sync(f,data)
        identities=[os.stat(p,follow_symlinks=False) for p in temps]
        # Hard-link publication is exclusive; it cannot replace an existing file.
        os.link(temps[1],report_path); linked.append((report_path,identities[1]))
        os.link(temps[0],patch_path); linked.append((patch_path,identities[0]))
        fd=os.open(directory,os.O_RDONLY|os.O_DIRECTORY)
        try: os.fsync(fd)
        finally: os.close(fd)
        for path in temps:
            os.unlink(path)
    except BaseException:
        for path,identity in reversed(linked):
            try:
                if os.path.samestat(os.stat(path,follow_symlinks=False),identity):
                    os.unlink(path)
            except FileNotFoundError: pass
        raise
    finally:
        for path in temps:
            try: os.unlink(path)
            except FileNotFoundError: pass


def main():
    ap=argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('input',type=Path,help='the selected eboot.bin (SELF or plain ELF)')
    ap.add_argument('--resolution',default='1920x1080',
                    help='internal 16:9 scene extent; 1280x720 is the verified value, '
                         '1920x1080 writes the game default back (no-op)')
    ap.add_argument('--patch',type=Path,required=True,help='BBPATCH2 output, in an existing directory')
    ap.add_argument('--report',type=Path,required=True,help='JSON report, same directory as --patch')
    args=ap.parse_args(); w,h=resolution(args.resolution)
    patch_path,report_path,_=output_paths(args.patch,args.report,args.input)
    src=args.input.read_bytes(); elf,_,ph,_,_=parse_self(src)
    report=find(elf,ph)
    report.update(input_sha256=hashlib.sha256(src).hexdigest(), resolution=[w,h], experimental=True,
        risk='Shared mode0/mode15/allocation-floor/comparison literal changes; unknown indirect uses and per-pass scene/depth/viewport coherence. The host window and output image are unchanged. No runtime proof for this launch.')
    patch=encode_patch(int(report['literal'],16),w,h)
    report['patch_sha256']=hashlib.sha256(patch).hexdigest()
    # Complete and sync both private files before publishing either name.
    publish(patch_path,patch,report_path,(json.dumps(report,indent=2)+'\n').encode())
    print(json.dumps(report,indent=2))

if __name__=='__main__':
    try: main()
    except (ValueError, OSError, KeyError, StopIteration, struct.error, subprocess.SubprocessError) as e:
        print(f'error: {e}',file=sys.stderr); sys.exit(1)
