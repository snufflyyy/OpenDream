# SPDX-FileCopyrightText: Copyright 2026 OpenDream contributors
# SPDX-License-Identifier: GPL-2.0-or-later
"""DREAMS_RENDER_RESOLUTION: the offline patch generator, its CLI and its launch plumbing.

All fixtures are synthetic and analyzed, never executed. The launch tests run the real
run-dreams.sh in a temporary copy of the port with stub game-check/prepare/probe dependencies and
a stub generator: no game file, cache or GPU is touched.
"""
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

PORT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(PORT / 'tools'))
import dreams_render_resolution as g  # noqa: E402

TOOL = PORT / 'tools' / 'dreams_render_resolution.py'
BINUTILS = tuple(shutil.which(name) for name in ('as', 'objcopy', 'nm', 'objdump'))


class ResolutionTests(unittest.TestCase):
    def test_supported_and_default_noop(self):
        self.assertEqual(g.resolution('1280x720'), (1280, 720))
        wh = g.resolution('1920x1080')
        patch = g.encode_patch(0x200, *wh)
        self.assertEqual(struct.unpack('<8sQQQQ', patch[:40]), (b'BBPATCH2', 0, 1, 0x200, 16))
        self.assertEqual(patch[40:], g.ORIGINAL)

    def test_invalid_and_bounds(self):
        for value in ('x', '1280X720', '0x0', '639x360', '1921x1080', '1280x721', '640x360',
                      '-1280x720', '1280x720 '):
            with self.subTest(value=value), self.assertRaises(ValueError):
                g.resolution(value)

    def test_unique_file_backing_and_out_of_bounds(self):
        load = dict(vaddr=0x100, filesz=16, offset=0)
        self.assertEqual(g.mapped_bytes(b'abcdefghijklmnop', [load], 0x100, 16), b'abcdefghijklmnop')
        for va, n, loads in [(0xff, 1, [load]), (0x10f, 2, [load]), (0x100, 1, [load,load])]:
            with self.assertRaises(ValueError):
                g.mapped_bytes(b'abcdefghijklmnop', loads, va, n)


class CandidateTests(unittest.TestCase):
    def image(self, prologues):
        data = bytearray(0x900)
        data[0x800:0x810] = g.ORIGINAL
        for p in prologues:
            data[p:p+4] = b'\x55\x48\x89\xe5'
            for pos in (p+16, p+32, p+48):
                data[pos:pos+8] = b'\xc5\xfa\x6f\x05'+struct.pack('<i',0x800-(pos+8))
        ph=[dict(type=1, flags=5, offset=0, filesz=len(data), memsz=len(data), vaddr=0)]
        return data,ph

    def test_missing_and_ambiguous_site_fail(self):
        fakeins=[(0,b'', 'push','%rbp',None),(1,b'', 'ret','',None)]
        with mock.patch.object(g,'metadata',return_value=({},[])), mock.patch.object(g,'disassemble',return_value=fakeins), mock.patch.object(g,'validate_cluster',return_value={}):
            for prologues, expected in [([], 0), ([0x100,0x200], 2)]:
                with self.subTest(prologues=prologues), mock.patch.object(g,'function_ranges',return_value=[(p,p+0x80) for p in prologues]), self.assertRaisesRegex(ValueError, f'found {expected}'):
                    g.find(*self.image(prologues))

    def test_relocation_overlap_rejected(self):
        fakeins=[(0,b'', 'push','%rbp',None),(1,b'', 'ret','',None)]
        with mock.patch.object(g,'metadata',return_value=({},[0x7fc])), mock.patch.object(g,'function_ranges',return_value=[(0x100,0x180)]), mock.patch.object(g,'disassemble',return_value=fakeins), mock.patch.object(g,'validate_cluster',return_value={}):
            with self.assertRaisesRegex(ValueError,'found 0'):
                g.find(*self.image([0x100]))

    def test_missing_site_fails_before_output(self):
        with tempfile.TemporaryDirectory() as td:
            root=Path(td); src=root/'input'; src.write_bytes(b'x')
            patch=root/'test-no-output.bbpatch'; report=root/'test-no-output.json'
            argv=['dreams_render_resolution',str(src),'--resolution','1280x720',
                  '--patch',str(patch),'--report',str(report)]
            with mock.patch.object(sys,'argv',argv), mock.patch.object(g,'parse_self',return_value=(b'',None,[],None,None)), mock.patch.object(g,'find',side_effect=ValueError('missing site')):
                with self.assertRaisesRegex(ValueError,'missing site'): g.main()
            self.assertFalse(patch.exists()); self.assertFalse(report.exists())
            self.assertEqual(list(root.glob('.publish-*')), [])


class DecodedRoleTests(unittest.TestCase):
    def fixture(self):
        # Synthetic decoded instructions: no executable/game bytes or hardcoded game VAs.
        rows = [
            ('push','%rbp',None), ('lea','0x1(%rip),%rcx',0x800),
            ('mov','%edi,%eax',None), ('movslq','(%rcx,%rax,4),%rax',None),
            ('add','%rcx,%rax',None), ('jmp','*%rax',None),
            ('vmovdqu','0x1(%rip),%xmm0',0x900), ('lea','-0x9(%rdi),%ecx',None),
            ('vmovq','%xmm0,0x1(%rip)',0xa00), ('cmp','$0x6,%ecx',None),
            ('jae','0x10b',None), ('cmp','$0x10,%edi',None), ('je','0x11f',None),
            ('cmp','$0xf,%edi',None), ('jne','0x111',None),
            ('vmovdqu','0x1(%rip),%xmm0',0x900), ('jmp','0x111',None),
            ('xor','%r12d,%r12d',None), ('vmovd','%xmm0,%r15d',None),
            ('vpextrd','$0x1,%xmm0,%r14d',None), ('vmovq','0x1(%rip),%xmm0',0xa00),
            ('vpmaxud','0x1(%rip),%xmm0,%xmm0',0x900), ('vmovd','%xmm0,%eax',None),
            ('vpextrd','$0x1,%xmm0,%ebx',None), ('imul','%eax,%ebx',None),
            ('mov','%r14d,%r9d',None), ('mov','%r15d,%r8d',None),
            ('call','0x700',None), ('call','0x710',None),
            ('vmovdqu','0x1(%rip),%xmm1',0x900), ('vpcmpgtd','%xmm0,%xmm1,%xmm1',None),
            ('ret','',None)]
        return [(0x100+i,b'X',mn,op,t) for i,(mn,op,t) in enumerate(rows)]

    def check(self, ins, table_target=0x106, missing_call=False):
        got={0xb00:g.NAMES[0],0xb10:g.NAMES[1]}
        def mem(elf,loads,va,n):
            if va==0x800: return struct.pack('<i',table_target-0x800)
            if va in (0x700,0x710): return b'\0'*n
            raise ValueError('unmapped')
        def dis(code,base):
            return [(base,b'X','jmp','*0x1(%rip)',0xb00 if base==0x700 else (0 if missing_call else 0xb10))]
        with mock.patch.object(g,'mapped_bytes',side_effect=mem), mock.patch.object(g,'disassemble',side_effect=dis):
            return g.validate_cluster(ins,0x900,b'',[],got)

    def test_verified_roles_and_flow(self):
        result=self.check(self.fixture())
        self.assertEqual(result['width_argument'],'0x11a')
        self.assertEqual(result['height_argument'],'0x119')

    def test_bad_table_flow_role_and_call_fail_closed(self):
        with self.assertRaises(ValueError): self.check(self.fixture(),table_target=0x107)
        with self.assertRaises(ValueError): self.check(self.fixture(),missing_call=True)
        for index,mn,op,target in [(26,'mov','%r14d,%r8d',None),(21,'vpmaxud','0x1(%rip),%xmm1,%xmm1',0x900),(20,'vmovq','0x1(%rip),%xmm0',0xa10),(30,'vpxor','%xmm0,%xmm1,%xmm1',None)]:
            ins=self.fixture(); old=ins[index]; ins[index]=(old[0],old[1],mn,op,target)
            with self.subTest(index=index), self.assertRaises(ValueError): self.check(ins)


ASM = r'''
.text
.org 0x200
ctor:
 push %rbp
 mov %rsp,%rbp
 lea table(%rip),%rcx
 mov %edi,%eax
 movslq (%rcx,%rax,4),%rax
 add %rcx,%rax
 jmp *%rax
mode0:
 vmovdqu literal(%rip),%xmm0
 lea -9(%rdi),%ecx
 vmovq %xmm0,global_value(%rip)
 cmp $6,%ecx
 jae modecheck
 vpxor %xmm0,%xmm0,%xmm0
 jmp join
modecheck:
 cmp $16,%edi
 je endlabel
 cmp $15,%edi
 jne join
 vmovdqu literal(%rip),%xmm0
 jmp join
join:
 xor %r12d,%r12d
 vmovd %xmm0,%r15d
 vpextrd $1,%xmm0,%r14d
 vmovq global_value(%rip),%xmm0
 vpmaxud literal(%rip),%xmm0,%xmm0
 vmovd %xmm0,%eax
 vpextrd $1,%xmm0,%ebx
 imul %eax,%ebx
 mov %r14d,%r9d
 mov %r15d,%r8d
 call plt_attr
 call plt_reg
 vmovdqu literal(%rip),%xmm1
 vpcmpgtd %xmm0,%xmm1,%xmm1
endlabel:
 ret
ctor_end:
.org 0x600
plt_attr: jmp *got_attr(%rip)
.org 0x610
plt_reg: jmp *got_reg(%rip)
.org 0x700
table: .long mode0-table
.org 0x800
literal: .long 1920,1080,0,0
.org 0x900
global_value: .quad 0
got_attr: .quad 0
got_reg: .quad 0
.org 0x1000
'''


def build_fixture(assembly=ASM, mutation=None):
    """Real GNU as/ld bytes plus synthetic PS4 metadata; returns (raw ELF, symbols)."""
    assembler, objcopy, nm = BINUTILS[:3]
    with tempfile.TemporaryDirectory(prefix='dreams-rr-fixture-') as td:
        td=Path(td); source=td/'fixture.s'; obj=td/'fixture.o'; binary=td/'fixture.bin'
        source.write_text(assembly)
        subprocess.run([assembler,'--64','-o',str(obj),str(source)],check=True,capture_output=True)
        subprocess.run([objcopy,'-O','binary','-j','.text',str(obj),str(binary)],check=True,capture_output=True)
        out=subprocess.run([nm,'-n',str(obj)],check=True,text=True,capture_output=True).stdout
        syms={parts[2]:int(parts[0],16) for line in out.splitlines() if len(parts:=line.split())==3}
        data=bytearray(binary.read_bytes())
    start,end=syms['ctor'],syms['ctor_end']; eh=0xa00
    # One real CIE and FDE, then an end marker followed by the GNU search header.
    cie=bytes.fromhex('1400000000000000017a5200017810011b0c070890010000')
    fde=eh+len(cie)
    frame=cie+struct.pack('<IIiIB3x',16,fde+4-eh,start-(fde+8),end-start,0)+b'\0'*4
    hdr=eh+len(frame)
    hdrbytes=struct.pack('<BBBBiIii',1,0x1b,3,0x3b,eh-(hdr+4),1,start-hdr,fde-hdr)
    data[eh:eh+len(frame)]=frame; data[hdr:hdr+len(hdrbytes)]=hdrbytes
    # Synthetic PS4 dynamic/symbol/relocation metadata, consumed by real metadata().
    names=[b'',g.nid(g.NAMES[0]).encode()+b'#A',g.nid(g.NAMES[1]).encode()+b'#A']
    strings=b'\0'+names[1]+b'\0'+names[2]+b'\0'; offsets=[0,1,2+len(names[1])]
    symbols=b''.join(struct.pack('<IBBHQQ',off,0x12,0,0,0,0) for off in offsets)
    reloc=b''.join(struct.pack('<QQq',syms[name],(index<<32)|7,0) for index,name in ((1,'got_attr'),(2,'got_reg')))
    blob=0xb00; so=0; yo=len(strings); ro=yo+len(symbols); data[blob:blob+ro+len(reloc)]=strings+symbols+reloc
    tags=[(0x61000035,so),(0x61000037,len(strings)),(0x61000039,yo),(0x6100003f,len(symbols)),(0x61000029,ro),(0x6100002d,len(reloc)),(0x6100002f,ro+len(reloc)),(0x61000031,0),(0,0)]
    dyn=b''.join(struct.pack('<QQ',*x) for x in tags); data[0xc00:0xc00+len(dyn)]=dyn
    # Minimal ELF including all program headers; input parser is the production parser.
    ident=b'\x7fELF\x02\x01\x01'+bytes(9)
    header=struct.pack('<16sHHIQQQIHHHHHH',ident,2,62,1,start,64,0,0,64,56,4,0,0,0)
    def ph(t,flags,off,va,sz): return struct.pack('<IIQQQQQQ',t,flags,off,va,va,sz,sz,1)
    data[:64]=header
    data[64:64+4*56]=b''.join([ph(1,5,0,0,len(data)),ph(2,4,0xc00,0xc00,len(dyn)),ph(0x61000000,4,blob,blob,ro+len(reloc)),ph(0x6474e550,4,hdr,hdr,len(hdrbytes))])
    if mutation: mutation(data,syms,eh,hdr,ro)
    return bytes(data), syms


def fixture(assembly=ASM, mutation=None):
    data, syms = build_fixture(assembly, mutation)
    elf,_,phs,_,_=g.parse_self(data)
    return elf,phs,syms


@unittest.skipUnless(all(BINUTILS), 'GNU binutils (as, objcopy, nm, objdump) required')
class EndToEndTests(unittest.TestCase):
    def test_real_decode_metadata_and_frame(self):
        elf,ph,syms=fixture(); result=g.find(elf,ph)
        self.assertEqual(int(result['literal'],16),syms['literal'])
        self.assertEqual(result['function_range'],[hex(syms['ctor']),hex(syms['ctor_end'])])
        self.assertTrue(result['literal_outside_all_fde_ranges'])
        self.assertEqual(len(result['calls']),2)

    def test_bad_branches_dispatch_and_relocations(self):
        versions=[
            ASM.replace('mov %edi,%eax\n movslq', 'xor %ecx,%ecx\n mov %edi,%eax\n movslq'),
            ASM.replace('mov %edi,%eax\n movslq (%rcx,%rax,4),%rax', 'movslq (%rcx,%rax,4),%rax\n mov %edi,%eax'),
            ASM.replace('cmp $6,%ecx\n jae modecheck','cmp $6,%ecx\n mov $0,%ecx\n jae modecheck'),
            ASM.replace('cmp $6,%ecx\n jae modecheck','cmp $6,%ecx\n mov $0,%edi\n jae modecheck'),
            ASM.replace('lea table(%rip),%rcx\n mov %edi,%eax','lea table(%rip),%rcx\n xor %edi,%edi\n mov %edi,%eax'),
            ASM.replace('jae modecheck','jae join'),
            ASM.replace('jne join','jne mode0'),
            ASM.replace('call plt_reg','call plt_attr'),
        ]
        for source in versions:
            with self.subTest(source=source), self.assertRaises(ValueError):
                elf,ph,_=fixture(source); g.find(elf,ph)
        def overlap(data,s,eh,hdr,ro):
            struct.pack_into('<Q',data,0xb00+ro,s['literal'])
        def missing_nid(data,s,eh,hdr,ro):
            data[0xb01] ^= 1
        def missing_literal(data,s,eh,hdr,ro):
            data[s['literal']] ^= 1
        def table(data,s,eh,hdr,ro):
            struct.pack_into('<i',data,s['table'],0)
        def frame(data,s,eh,hdr,ro):
            data[hdr+3]=0xff
        def within_function(data,s,eh,hdr,ro):
            # Widen FDE to include literal, exercising the data classification check.
            fde=eh+24; struct.pack_into('<I',data,fde+12,s['literal']+16-s['ctor'])
        for mutation in (overlap,table,frame,within_function,missing_nid,missing_literal):
            with self.subTest(mutation=mutation.__name__), self.assertRaises(ValueError):
                elf,ph,_=fixture(mutation=mutation); g.find(elf,ph)


class PublicationTests(unittest.TestCase):
    def test_failures_never_publish_patch(self):
        for target,when in (('mkstemp',1),('mkstemp',2),('write_and_sync',1),('write_and_sync',2),('link',1),('link',2),('fsync',1),('fsync',2),('fsync',3),('unlink',1),('unlink',2)):
            with self.subTest(target=target,when=when), tempfile.TemporaryDirectory(prefix='dreams-rr-pub-') as td:
                root=Path(td); patch=root/'p'; report=root/'r'
                owner = g if target=='write_and_sync' else (g.tempfile if target=='mkstemp' else g.os)
                original=getattr(owner,target); count=0
                def fail(*args,**kwargs):
                    nonlocal count
                    count+=1
                    if count==when:
                        if target=='write_and_sync': args[0].write(b'partial')
                        raise OSError('injected '+target)
                    return original(*args,**kwargs)
                with mock.patch.object(owner,target,side_effect=fail), self.assertRaises(OSError):
                    g.publish(patch,b'patch',report,b'report')
                self.assertFalse(patch.exists()); self.assertFalse(report.exists())
                self.assertEqual(list(root.glob('.publish-*')), [])

    def test_existing_names_never_replaced(self):
        with tempfile.TemporaryDirectory(prefix='dreams-rr-pub-') as td:
            root=Path(td); patch=root/'p'; report=root/'r'; patch.write_bytes(b'untouched')
            with self.assertRaises(FileExistsError): g.publish(patch,b'new',report,b'report')
            self.assertEqual(patch.read_bytes(),b'untouched'); self.assertFalse(report.exists())

    def test_success_is_private_and_complete(self):
        with tempfile.TemporaryDirectory(prefix='dreams-rr-pub-') as td:
            root=Path(td); patch=root/'p'; report=root/'r'
            g.publish(patch,b'patch',report,b'report')
            self.assertEqual(patch.read_bytes(),b'patch'); self.assertEqual(report.read_bytes(),b'report')
            self.assertEqual(patch.stat().st_mode & 0o777,0o600); self.assertEqual(report.stat().st_mode & 0o777,0o600)
            self.assertEqual(list(root.glob('.publish-*')), [])


class CliTests(unittest.TestCase):
    def run_cli(self, *args):
        return subprocess.run([sys.executable, '-I', str(TOOL), *args], capture_output=True, text=True)

    def test_invalid_resolution_is_rejected(self):
        with tempfile.TemporaryDirectory() as td:
            root=Path(td); src=root/'eboot.bin'; src.write_bytes(b'x')
            for value in ('640x360', 'nonsense', '1280X720'):
                with self.subTest(value=value):
                    done=self.run_cli(str(src),'--resolution',value,
                        '--patch',str(root/'p'),'--report',str(root/'r'))
                    self.assertEqual(done.returncode,1,done.stdout+done.stderr)
                    self.assertIn('error:', done.stderr)
                    self.assertEqual(list(root.iterdir()), [src])

    def test_missing_input_is_rejected(self):
        with tempfile.TemporaryDirectory() as td:
            root=Path(td)
            done=self.run_cli(str(root/'absent/eboot.bin'),'--patch',str(root/'p'),'--report',str(root/'r'))
            self.assertEqual(done.returncode,1,done.stdout+done.stderr)
            self.assertIn('error:', done.stderr)
            self.assertFalse((root/'p').exists()); self.assertFalse((root/'r').exists())

    def test_output_directory_must_exist(self):
        with tempfile.TemporaryDirectory() as td:
            root=Path(td); src=root/'eboot.bin'; src.write_bytes(b'x')
            done=self.run_cli(str(src),'--patch',str(root/'missing/p'),'--report',str(root/'missing/r'))
            self.assertEqual(done.returncode,1,done.stdout+done.stderr)
            self.assertIn('output directory does not exist', done.stderr)

    def test_existing_output_is_never_replaced(self):
        with tempfile.TemporaryDirectory() as td:
            root=Path(td); src=root/'eboot.bin'; src.write_bytes(b'x')
            for existing in ('p','r'):
                with self.subTest(existing=existing):
                    patch=root/'p'; report=root/'r'
                    for path in (patch,report):
                        if path.exists(): path.unlink()
                    (root/existing).write_bytes(b'keep')
                    done=self.run_cli(str(src),'--patch',str(patch),'--report',str(report))
                    self.assertEqual(done.returncode,1,done.stdout+done.stderr)
                    self.assertIn('output already exists', done.stderr)
                    self.assertEqual((root/existing).read_bytes(),b'keep')

    def test_patch_and_report_must_differ_and_share_a_directory(self):
        with tempfile.TemporaryDirectory() as td:
            root=Path(td); src=root/'eboot.bin'; src.write_bytes(b'x'); (root/'sub').mkdir()
            same=self.run_cli(str(src),'--patch',str(root/'out'),'--report',str(root/'out'))
            self.assertEqual(same.returncode,1,same.stdout+same.stderr)
            self.assertIn('different files', same.stderr)
            outside=self.run_cli(str(src),'--patch',str(root/'p'),'--report',str(root/'sub/r'))
            self.assertEqual(outside.returncode,1,outside.stdout+outside.stderr)
            self.assertIn('same directory', outside.stderr)
            self.assertFalse((root/'p').exists())

    def test_input_is_never_an_output(self):
        with tempfile.TemporaryDirectory() as td:
            root=Path(td); src=root/'eboot.bin'; src.write_bytes(b'x')
            done=self.run_cli(str(src),'--patch',str(src),'--report',str(root/'r'))
            self.assertEqual(done.returncode,1,done.stdout+done.stderr)
            self.assertIn('overwrite the input', done.stderr)
            self.assertEqual(src.read_bytes(),b'x')

    @unittest.skipUnless(all(BINUTILS), 'GNU binutils (as, objcopy, nm, objdump) required')
    def test_success_publishes_patch_then_report_without_overwrite(self):
        data, syms = build_fixture()
        with tempfile.TemporaryDirectory() as td:
            root=Path(td); src=root/'eboot.bin'; src.write_bytes(data)
            patch=root/'render-resolution.bbpatch'; report=root/'render-resolution.json'
            done=self.run_cli(str(src),'--resolution','1280x720','--patch',str(patch),'--report',str(report))
            self.assertEqual(done.returncode,0,done.stdout+done.stderr)
            self.assertEqual(patch.read_bytes(), g.encode_patch(syms['literal'],1280,720))
            r=json.loads(report.read_text())
            self.assertEqual(int(r['literal'],16), syms['literal'])
            self.assertEqual(r['resolution'], [1280,720])
            self.assertTrue(r['experimental'])
            self.assertEqual(r['patch_sha256'], g.hashlib.sha256(patch.read_bytes()).hexdigest())
            self.assertEqual(patch.stat().st_mode & 0o777, 0o600)
            again=self.run_cli(str(src),'--resolution','1280x720','--patch',str(patch),'--report',str(report))
            self.assertEqual(again.returncode,1,again.stdout+again.stderr)
            self.assertIn('output already exists', again.stderr)


SFO_FIELDS = {'TITLE_ID': 'CUSA08010', 'TITLE': 'Dreams', 'VERSION': '02.52'}


def make_sfo(fields):
    keys, values, entries = bytearray(), bytearray(), bytearray()
    for key, value in fields.items():
        key_offset, value_offset = len(keys), len(values)
        keys.extend(key.encode() + b"\0")
        encoded = value.encode() + b"\0"
        values.extend(encoded)
        entries.extend(struct.pack("<HHIII", key_offset, 0x204, len(encoded), len(encoded), value_offset))
    return struct.pack("<4sIIII", b"\0PSF", 0x101, 20 + len(entries),
                       20 + len(entries) + len(keys), len(fields)) + entries + keys + values


PREPARE_STUB = '''import os, pathlib, sys
out = pathlib.Path(sys.argv[sys.argv.index('--out') + 1])
out.mkdir(parents=True, exist_ok=True)
with open(os.environ['EVENTS'], 'a') as f: f.write('prepare\\n')
for name in ('boot-linked.bin', 'link.json', 'analysis.json', 'content.bin'):
    (out / name).write_text('fixture')
'''

GENERATOR_STUB = '''import json, os, pathlib, sys
args = sys.argv[1:]
opt = lambda name: args[args.index(name) + 1]
resolution = opt('--resolution')
with open(os.environ['EVENTS'], 'a') as f:
    f.write(json.dumps(['render-resolution', resolution, args[0], opt('--patch'), opt('--report')]) + '\\n')
# The real tool rejects unsupported/16:9-invalid values and any generator error exits non-zero.
if os.environ.get('STUB_GENERATOR_FAIL'):
    sys.stderr.write('error: injected generator failure\\n'); sys.exit(1)
if resolution not in ('1280x720', '1920x1080'):
    sys.stderr.write('error: supported values: 1280x720, 1920x1080\\n'); sys.exit(1)
patch, report = pathlib.Path(opt('--patch')), pathlib.Path(opt('--report'))
patch.write_bytes(b'BBPATCH2 fixture')
report.write_text('{}\\n')
'''

PROBE_STUB = '''import json, os, sys
with open(os.environ['EVENTS'], 'a') as f: f.write(json.dumps(['probe', *sys.argv[1:]]) + '\\n')
'''


class LauncherTests(unittest.TestCase):
    """The real run-dreams.sh in a private copy; every dependency is a stub."""

    def setUp(self):
        self._td = tempfile.TemporaryDirectory(prefix='dreams-rr-launch-')
        self.addCleanup(self._td.cleanup)
        base = Path(self._td.name)
        self.port, self.game, self.work = base / 'port', base / 'game', base / 'work'
        (self.port / 'scripts').mkdir(parents=True)
        (self.port / 'out').mkdir()
        (self.port / 'tools').mkdir()
        shutil.copy(PORT / 'run-dreams.sh', self.port)
        for name in ('dreams_game_check.py', 'prepare.py', 'game_check.py', 'dreams_run_lock.py'):
            shutil.copy(PORT / 'scripts' / name, self.port / 'scripts')
        (self.port / 'scripts/dreams_prepare.py').write_text(PREPARE_STUB)
        (self.port / 'scripts/dreams_seed_save.py').write_text(
            "import os\nopen(os.environ['EVENTS'], 'a').write('save\\n')\n")
        (self.port / 'tools/dreams_render_resolution.py').write_text(GENERATOR_STUB)
        probe = self.port / 'out/dreams-probe'
        probe.write_text(f'#!{sys.executable}\n' + PROBE_STUB)
        probe.chmod(0o755)
        (self.port / 'out/gpu-library.txt').write_text('stub')
        self.make_game()
        self.events = base / 'events'

    def make_game(self):
        (self.game / 'sce_sys').mkdir(parents=True, exist_ok=True)
        (self.game / 'sce_sys/param.sfo').write_bytes(make_sfo(SFO_FIELDS))
        for name in ('eboot.bin', 'sce_module/libc.prx', 'sce_module/libSceFios2.prx',
                     'sce_module/libSceJobManager.prx'):
            path = self.game / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b'\x7fELF fixture')

    def launch(self, env=None, forwarded=()):
        environment = {key: value for key, value in os.environ.items()
                       if not key.startswith('DREAMS_') and key != 'EVENTS'}
        environment.update(DREAMS_GAME=str(self.game), DREAMS_WORK=str(self.work),
                           DREAMS_USER_DIR=str(self.work / 'private'), DREAMS_LOG='0',
                           EVENTS=str(self.events))
        environment.update(env or {})
        return subprocess.run(['bash', str(self.port / 'run-dreams.sh'), *forwarded],
                              env=environment, capture_output=True, text=True)

    def records(self):
        if not self.events.exists():
            return []
        return [json.loads(line) if line.startswith('[') else line
                for line in self.events.read_text().splitlines()]

    def probe_args(self):
        probes = [record for record in self.records() if record[0] == 'probe']
        self.assertEqual(len(probes), 1)
        return probes[0][1:]

    def test_unset_leaves_the_launch_unchanged(self):
        done = self.launch(forwarded=['--cpu-only'])
        self.assertEqual(done.returncode, 0, done.stdout + done.stderr)
        self.assertEqual(self.records()[0], 'prepare')
        self.assertFalse(any(isinstance(r, list) and r[0] == 'render-resolution' for r in self.records()))
        args = self.probe_args()
        self.assertNotIn('--patches', args)
        self.assertEqual(args[-1], '--cpu-only')
        self.assertFalse((self.work / 'out/render-resolution').exists())
        self.assertEqual([name for name in ('boot-linked.bin', 'content.bin')
                          if (self.work / 'out' / name).is_file()],
                         ['boot-linked.bin', 'content.bin'])

    def test_set_generates_once_and_forwards_the_patch(self):
        eboot = (self.game / 'eboot.bin').read_bytes()
        done = self.launch({'DREAMS_RENDER_RESOLUTION': '1280x720'})
        self.assertEqual(done.returncode, 0, done.stdout + done.stderr)
        generated = [r for r in self.records() if isinstance(r, list) and r[0] == 'render-resolution']
        self.assertEqual(len(generated), 1, self.records())
        _, value, source, patch, report = generated[0]
        self.assertEqual(value, '1280x720')
        self.assertEqual(source, str(self.game / 'eboot.bin'))
        directory = Path(patch).parent
        self.assertEqual(directory, Path(report).parent)
        self.assertEqual(Path(patch).name, 'render-resolution.bbpatch')
        self.assertEqual(Path(report).name, 'render-resolution.json')
        self.assertTrue(re.fullmatch(r'render-resolution/[0-9]{8}-[0-9]{6}',
                                     directory.relative_to(self.work / 'out').as_posix()))
        self.assertTrue(Path(patch).is_file() and Path(report).is_file())
        args = self.probe_args()
        self.assertEqual(args.count('--patches'), 1)
        self.assertEqual(args[args.index('--patches') + 1], patch)
        self.assertEqual((self.game / 'eboot.bin').read_bytes(), eboot)
        self.assertEqual((self.work / 'out/boot-linked.bin').read_text(), 'fixture')

    def test_invalid_value_stops_before_the_probe(self):
        done = self.launch({'DREAMS_RENDER_RESOLUTION': '640x360'})
        self.assertNotEqual(done.returncode, 0, done.stdout + done.stderr)
        self.assertNotIn('probe', [r[0] for r in self.records() if isinstance(r, list)])
        self.assertIn('supported values', done.stderr)

    def test_generator_failure_stops_before_the_probe(self):
        done = self.launch({'DREAMS_RENDER_RESOLUTION': '1280x720', 'STUB_GENERATOR_FAIL': '1'})
        self.assertNotEqual(done.returncode, 0, done.stdout + done.stderr)
        self.assertNotIn('probe', [r[0] for r in self.records() if isinstance(r, list)])
        self.assertIn('injected generator failure', done.stderr)

    def test_explicit_patches_is_refused_before_preparation(self):
        for forwarded in (['--patches=/tmp/other.bbpatch'], ['--patches', '/tmp/other.bbpatch']):
            with self.subTest(forwarded=forwarded):
                if self.events.exists(): self.events.unlink()
                done = self.launch({'DREAMS_RENDER_RESOLUTION': '1280x720'}, forwarded)
                self.assertEqual(done.returncode, 2, done.stdout + done.stderr)
                self.assertIn('--patches', done.stderr)
                self.assertFalse(self.events.exists())

    def test_resolution_is_disabled_without_the_variable(self):
        # A forwarded --patches is the user's own and stays untouched when the variable is unset.
        done = self.launch(forwarded=['--patches', '/tmp/mine.bbpatch'])
        self.assertEqual(done.returncode, 0, done.stdout + done.stderr)
        args = self.probe_args()
        self.assertEqual(args[args.index('--patches') + 1], '/tmp/mine.bbpatch')
        self.assertFalse(any(isinstance(r, list) and r[0] == 'render-resolution' for r in self.records()))


if __name__ == '__main__': unittest.main()
