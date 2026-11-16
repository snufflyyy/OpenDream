#!/usr/bin/env python3
# Usage: python3 -I tools/spirv_spin_scan.py <dir>/*.spvasm   (disassemble first with tools/spirv_dis.c)
# Docs: docs/dreams/GPU_HANG.md
"""Find spin-wait-like loops in disassembled SPIR-V (spirv-dis friendly-names text).

A loop is flagged when an exit condition depends on a memory read (OpLoad from a
StorageBuffer/Workgroup/Image pointer, OpAtomic*, OpImageRead) performed inside the loop, and
no loop-carried value that feeds that exit condition is a simple counter (phi updated by
IAdd/ISub with a constant).  Each loop also reports whether the read address is loop-invariant.
"""
import re, sys
from collections import defaultdict

def parse(path):
    lines = open(path).read().splitlines()
    defs = {}            # id -> (opcode, operands, type)
    storage = {}         # pointer type id -> storage class
    var_sc = {}          # variable id -> storage class
    blocks = {}          # label -> list of (res, op, args)
    order = []
    cur = None
    for ln in lines:
        s = ln.strip()
        if not s or s.startswith(';'):
            continue
        m = re.match(r'(%\S+)\s*=\s*(Op\w+)\s*(.*)', s)
        if m:
            res, op, rest = m.groups()
        else:
            res = None
            parts = s.split(None, 1)
            op, rest = parts[0], (parts[1] if len(parts) > 1 else '')
        args = re.findall(r'%[\w.]+|"[^"]*"|\S+', rest)
        if op == 'OpTypePointer':
            storage[res] = args[0]
        if op == 'OpVariable':
            var_sc[res] = args[1] if len(args) > 1 else '?'
        if op == 'OpLabel':
            cur = res
            blocks[cur] = []
            order.append(cur)
            continue
        if res:
            defs[res] = (op, args, cur)
        if cur is not None:
            blocks[cur].append((res, op, args))
        if op in ('OpFunctionEnd',):
            cur = None
    return defs, blocks, order, var_sc

def succ(block):
    for res, op, args in block[::-1]:
        if op == 'OpBranch':
            return [args[0]]
        if op == 'OpBranchConditional':
            return [args[1], args[2]]
        if op == 'OpSwitch':
            return [a for a in args[1:] if a.startswith('%')]
        if op in ('OpReturn', 'OpReturnValue', 'OpKill', 'OpUnreachable', 'OpTerminateInvocation'):
            return []
    return []

def ptr_root(defs, p, depth=0):
    while p in defs and defs[p][0] in ('OpAccessChain', 'OpInBoundsAccessChain', 'OpPtrAccessChain', 'OpCopyObject') and depth < 50:
        p = defs[p][1][1]
        depth += 1
    return p

def analyze(path):
    defs, blocks, order, var_sc = parse(path)
    out = []
    for h in order:
        blk = blocks[h]
        lm = [a for r, op, a in blk if op == 'OpLoopMerge']
        if not lm:
            continue
        merge, cont = lm[0][0], lm[0][1]
        # loop body: reachable from header without passing merge
        body = set([h]); stack = [s for s in succ(blk)]
        while stack:
            b = stack.pop()
            if b == merge or b in body or b not in blocks:
                continue
            body.add(b); stack.extend(succ(blocks[b]))
        # values defined in loop
        inloop = {r for b in body for r, op, a in blocks[b] if r}
        header_phis = {r for r, op, a in blk if op == 'OpPhi'}
        # exit conditions
        conds = []
        for b in body:
            for r, op, a in blocks[b]:
                if op == 'OpBranchConditional' and (a[1] not in body or a[2] not in body):
                    conds.append(a[0])
                if op == 'OpSwitch':
                    pass
        def slice_(v, seen):
            if v in seen or v not in inloop:
                return
            seen.add(v)
            op, args, _ = defs[v]
            if op == 'OpPhi' and v in header_phis:
                return  # loop-carried: stop here
            for x in args:
                if x.startswith('%'):
                    slice_(x, seen)
        reads = []
        counters = []
        for c in conds:
            sl = set(); slice_(c, sl)
            for v in sl:
                op, args, _ = defs[v]
                if op.startswith('OpAtomic') or op == 'OpImageRead':
                    reads.append((op, v))
                elif op == 'OpLoad':
                    root = ptr_root(defs, args[1])
                    sc = var_sc.get(root, '?')
                    if sc in ('StorageBuffer', 'Workgroup', 'Uniform', 'PhysicalStorageBuffer', '?'):
                        reads.append((f'OpLoad[{sc}:{root}]', v))
            # loop-carried values feeding the condition
            for v in sl:
                op, args, _ = defs[v]
                for x in args:
                    if x in header_phis:
                        counters.append(x)
        # classify header phis that feed conds: counter if incoming from continue is IAdd/ISub const
        def is_counter(phi):
            _, args, _ = defs[phi]
            for i in range(0, len(args), 2):
                val = args[i]
                if val in defs and defs[val][0] in ('OpIAdd', 'OpISub') and defs[val][2] in body:
                    a = defs[val][1]
                    if any(y in defs and defs[y][0] == 'OpConstant' for y in a[1:]) or phi in a:
                        return True
            return False
        cnt = sorted(set(counters))
        has_counter = any(is_counter(p) for p in cnt)
        if reads:
            out.append(dict(header=h, merge=merge, nblocks=len(body), reads=sorted(set(r[0] for r in reads)),
                            carried=cnt, counter=has_counter))
    return out

if __name__ == '__main__':
    for p in sys.argv[1:]:
        for r in analyze(p):
            tag = 'BOUNDED?' if r['counter'] else 'SPIN?'
            print(f"{tag:8} {p.split('/')[-1]} loop {r['header']} blocks={r['nblocks']} reads={r['reads']} carried={r['carried']}")
