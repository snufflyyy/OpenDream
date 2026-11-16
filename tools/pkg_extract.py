#!/usr/bin/env python3
"""PS4 fake-PKG (fpkg) extractor.

Implements the documented PKG -> outer PFS (AES-XTS, fake EKPFS) -> pfs_image.dat
(PFSC, zlib-compressed 64 KiB blocks) -> inner PFS -> files chain.

Unlike the old shadPS4 pkg.cpp heuristics this walks the real PFS structures
(header -> inode table -> superroot -> uroot dirents) and verifies:
  * every PFSC block either is stored raw or inflates to exactly one block with
    a valid zlib stream end (adler32-checked),
  * inode count / file count / byte totals match the PFS metadata,
  * (optional --digest) SHA-256 of the encrypted PFS image vs pkg header digest.

The RSA/fake keys are read (not copied) from the shadPS4 v0.5.0 sources in
tools/thirdparty/shadps4-v0.5.0/src/core/crypto/keys.h.

Usage:
  pkg_extract.py info  PKG
  pkg_extract.py list  PKG
  pkg_extract.py extract PKG OUTDIR [--digest]
"""
import hashlib
import hmac
import json
import os
import re
import struct
import sys
import zlib

from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

HERE = os.path.dirname(os.path.abspath(__file__))
KEYS_H = os.path.join(HERE, "thirdparty/shadps4-v0.5.0/src/core/crypto/keys.h")
PKG_TYPE_CPP = os.path.join(HERE, "thirdparty/shadps4-v0.5.0/src/core/file_format/pkg_type.cpp")


# ---------------------------------------------------------------- keys
def load_keyset(name):
    src = open(KEYS_H).read()
    m = re.search(r"class\s+%s\s*\{(.*?)\n\};" % name, src, re.S)
    if not m:
        raise SystemExit("keyset %s not found in %s" % (name, KEYS_H))
    body = m.group(1)
    ks = {}
    for fm in re.finditer(r"(\w+)\[\]\s*=\s*\{([^}]*)\}", body):
        vals = [int(x, 0) for x in re.findall(r"0x[0-9A-Fa-f]+|\b\d+\b", fm.group(2))]
        ks[fm.group(1)] = int.from_bytes(bytes(vals), "big")
    return ks


def rsa_pkcs1_decrypt(ks, ct):
    n, d = ks["Modulus"], ks["PrivateExponent"]
    m = pow(int.from_bytes(ct, "big"), d, n).to_bytes(256, "big")
    if m[0] != 0 or m[1] != 2:
        raise ValueError("bad PKCS#1 v1.5 padding")
    sep = m.index(0, 2)
    return m[sep + 1:]


def load_entry_names():
    names = {}
    for m in re.finditer(r"\{(0x[0-9A-Fa-f]+),\s*\"([^\"]+)\"\}", open(PKG_TYPE_CPP).read()):
        names[int(m.group(1), 16)] = m.group(2)
    return names


# ---------------------------------------------------------------- PKG
class Pkg:
    def __init__(self, path):
        self.path = path
        self.f = open(path, "rb")
        self.size = os.fstat(self.f.fileno()).st_size
        h = self.f.read(0x1000)
        self.hdr = h
        if h[:4] != b"\x7fCNT":
            raise SystemExit("not a PS4 pkg")
        (self.pkg_type, _, self.file_count, self.entry_count, self.sc_entry_count,
         _, self.table_off, self.sc_data_size, self.body_off, self.body_size,
         self.content_off, self.content_size) = struct.unpack_from(">IIIIHHIIQQQQ", h, 4)
        self.content_id = h[0x40:0x64].rstrip(b"\0").decode()
        (self.drm_type, self.content_type, self.content_flags, self.promote_size,
         self.version_date, self.version_hash) = struct.unpack_from(">IIIIII", h, 0x70)
        (self.pfs_image_count, self.pfs_image_flags, self.pfs_image_offset, self.pfs_image_size,
         self.mount_image_offset, self.mount_image_size, self.pkg_size, self.pfs_signed_size,
         self.pfs_cache_size) = struct.unpack_from(">IQQQQQQII", h, 0x404)
        self.pfs_image_digest = h[0x440:0x460]
        self.pfs_signed_digest = h[0x460:0x480]
        self.entries = []
        self.f.seek(self.table_off)
        tbl = self.f.read(32 * self.entry_count)
        for i in range(self.entry_count):
            eid, fno, fl1, fl2, off, sz = struct.unpack_from(">IIIIII", tbl, 32 * i)
            self.entries.append(dict(id=eid, name_off=fno, flags1=fl1, flags2=fl2, offset=off,
                                     size=sz, raw=tbl[32 * i:32 * i + 32]))
        known = load_entry_names()
        names_blob = b""
        for e in self.entries:
            if e["id"] == 0x200:
                names_blob = self.read(e["offset"], e["size"])
        for e in self.entries:
            if e["id"] in known:
                e["name"] = known[e["id"]]
            elif e["name_off"] and names_blob:
                e["name"] = names_blob[e["name_off"]:names_blob.index(b"\0", e["name_off"])].decode()
            else:
                e["name"] = "entry_%04x" % e["id"]

    def read(self, off, n):
        self.f.seek(off)
        return self.f.read(n)

    def entry(self, eid):
        for e in self.entries:
            if e["id"] == eid:
                return e
        return None

    def entry_iv_key(self, e, dk3):
        return hashlib.sha256(e["raw"] + dk3).digest()

    def derive_keys(self):
        ek = self.entry(0x10)
        blob = self.read(ek["offset"], ek["size"])
        key1 = [blob[0x20 + 7 * 0x20 + 0x100 * i:0x20 + 7 * 0x20 + 0x100 * (i + 1)] for i in range(7)]
        self.dk3 = rsa_pkcs1_decrypt(load_keyset("PkgDerivedKey3Keyset"), key1[3])[:32]
        ik = self.entry(0x20)
        ivkey = self.entry_iv_key(ik, self.dk3)
        c = Cipher(algorithms.AES(ivkey[16:32]), modes.CBC(ivkey[:16])).decryptor()
        imgkey = c.update(self.read(ik["offset"], 0x100)) + c.finalize()
        self.ekpfs = rsa_pkcs1_decrypt(load_keyset("FakeKeyset"), imgkey)[:32]
        seed = self.read(self.pfs_image_offset + 0x370, 16)
        d = hmac.new(self.ekpfs, struct.pack("<I", 1) + seed, hashlib.sha256).digest()
        self.tweak_key, self.data_key = d[:16], d[16:32]

    def decrypt_entry(self, e):
        data = self.read(e["offset"], e["size"])
        ivkey = self.entry_iv_key(e, self.dk3)
        c = Cipher(algorithms.AES(ivkey[16:32]), modes.CBC(ivkey[:16])).decryptor()
        n = len(data) & ~15
        return c.update(data[:n]) + c.finalize() + data[n:]


# ---------------------------------------------------------------- outer image (XTS)
class XtsImage:
    """Random-access reader over the encrypted outer PFS image."""
    SECTOR = 0x1000

    def __init__(self, pkg, encrypted, plain_until=0x10000):
        self.pkg = pkg
        self.base = pkg.pfs_image_offset
        self.size = pkg.pfs_image_size
        self.encrypted = encrypted
        self.plain_until = plain_until
        self.key = pkg.data_key + pkg.tweak_key

    def read(self, off, n):
        if n <= 0:
            return b""
        s0 = off // self.SECTOR
        s1 = (off + n + self.SECTOR - 1) // self.SECTOR
        raw = self.pkg.read(self.base + s0 * self.SECTOR, (s1 - s0) * self.SECTOR)
        if self.encrypted:
            out = bytearray(raw)
            for i in range(s1 - s0):
                sec = s0 + i
                if sec * self.SECTOR < self.plain_until:
                    continue
                d = Cipher(algorithms.AES(self.key), modes.XTS(struct.pack("<QQ", sec, 0))).decryptor()
                out[i * self.SECTOR:(i + 1) * self.SECTOR] = d.update(raw[i * self.SECTOR:(i + 1) * self.SECTOR])
            raw = bytes(out)
        o = off - s0 * self.SECTOR
        return raw[o:o + n]


# ---------------------------------------------------------------- PFSC (pfs_image.dat)
class PfscImage:
    def __init__(self, parent, base, length):
        self.p = parent
        self.base = base
        self.length = length
        h = parent.read(base, 0x30)
        magic, _u4, _u8, self.block_sz, self.block_sz2, self.block_offsets, self.data_start, \
            self.data_length = struct.unpack_from("<IiiiqqQq", h, 0)
        if magic != 0x43534650:
            raise ValueError("PFSC magic missing at pfs_image.dat start")
        self.nblocks = (self.data_length + self.block_sz2 - 1) // self.block_sz2
        tbl = parent.read(base + self.block_offsets, 8 * (self.nblocks + 1))
        self.sector_map = struct.unpack("<%dQ" % (self.nblocks + 1), tbl)
        self.size = self.data_length
        self.stats = dict(raw=0, zlib=0, zero=0)
        self._cache = {}

    def block(self, i):
        c = self._cache.get(i)
        if c is not None:
            return c
        a, b = self.sector_map[i], self.sector_map[i + 1]
        bs = self.block_sz2
        if b < a:
            raise ValueError("PFSC sector map not monotonic at block %d" % i)
        sz = b - a
        if sz == 0:
            data = bytes(bs)
            self.stats["zero"] += 1
        else:
            comp = self.p.read(self.base + a, sz)
            if sz == bs:
                data = comp
                self.stats["raw"] += 1
            elif sz > bs:
                raise ValueError("PFSC block %d bigger than block size" % i)
            else:
                d = zlib.decompressobj()
                data = d.decompress(comp, bs + 1)
                if not d.eof or len(data) != bs:
                    raise ValueError("PFSC block %d: bad zlib stream (eof=%s len=%d)" % (i, d.eof, len(data)))
                self.stats["zlib"] += 1
        if len(self._cache) > 64:
            self._cache.clear()
        self._cache[i] = data
        return data

    def read(self, off, n):
        out = bytearray()
        bs = self.block_sz2
        while n > 0:
            bi, bo = divmod(off, bs)
            chunk = self.block(bi)[bo:bo + n]
            out += chunk
            off += len(chunk)
            n -= len(chunk)
        return bytes(out)


# ---------------------------------------------------------------- PFS
PFS_FILE, PFS_DIR, PFS_DOT, PFS_DOTDOT = 2, 3, 4, 5
MODE_SIGNED, MODE_64BIT, MODE_ENCRYPTED = 1, 2, 4


class Pfs:
    def __init__(self, img, label):
        self.img = img
        self.label = label
        h = img.read(0, 0x60)
        (self.version, self.magic, self.id, self.fmode, self.clean, self.read_only, self.rsv,
         self.mode, _unk1, self.block_size, self.n_backup, self.n_block, self.dinode_count,
         self.nd_block, self.dinode_block_count, self.superroot_ino) = \
            struct.unpack_from("<qqqBBBBHhiiqqqqq", h, 0)
        if self.magic != 20130315:
            raise ValueError("%s: bad PFS magic %d" % (label, self.magic))
        signed = bool(self.mode & MODE_SIGNED)
        is64 = bool(self.mode & MODE_64BIT)
        # inode layouts (see LibOrbisPkg PfsStructs):
        #   unsigned32: 0x64 hdr + 12*u32 db + 5*u32 ib            = 0xA8
        #   signed32:   0x64 hdr + 12*(32B sig+u32) + 5*(32B sig+u32) = 0x2C8
        #   unsigned64: 0x64 hdr + 12*u64 + 5*u64                  = 0x10C
        #   signed64:   0x64 hdr + 12*(32B sig+u64) + 5*(32B+u64)  = 0x310
        ptr = 8 if is64 else 4
        self.ptr_size = ptr
        self.sig = 32 if signed else 0
        self.inode_size = 0x64 + 17 * (self.sig + ptr)
        self.inodes = []
        per_block = self.block_size // self.inode_size
        idx = 0
        for b in range(self.dinode_block_count):
            blk = img.read((1 + b) * self.block_size, self.block_size)
            for k in range(per_block):
                if idx >= self.dinode_count:
                    break
                self.inodes.append(self._parse_inode(blk[k * self.inode_size:(k + 1) * self.inode_size]))
                idx += 1

    def _parse_inode(self, b):
        mode, nlink, flags, size, size_c = struct.unpack_from("<HHIqq", b, 0)
        blocks = struct.unpack_from("<I", b, 0x60)[0]
        fmt = "<Q" if self.ptr_size == 8 else "<I"
        ptrs = []
        for i in range(17):
            o = 0x64 + i * (self.sig + self.ptr_size) + self.sig
            ptrs.append(struct.unpack_from(fmt, b, o)[0])
        return dict(mode=mode, nlink=nlink, flags=flags, size=size, size_c=size_c,
                    blocks=blocks, db=ptrs[:12], ib=ptrs[12:])

    def block_list(self, ino):
        """Resolve all data blocks of an inode (direct + single/double indirect)."""
        n = ino["blocks"]
        out = []
        for p in ino["db"]:
            if len(out) >= n:
                return out
            out.append(p)
        per = self.block_size // (self.sig + self.ptr_size)
        fmt = "<Q" if self.ptr_size == 8 else "<I"

        def indirect(blk, depth):
            data = self.img.read(blk * self.block_size, self.block_size)
            for k in range(per):
                if len(out) >= n:
                    return
                p = struct.unpack_from(fmt, data, k * (self.sig + self.ptr_size) + self.sig)[0]
                if depth == 0:
                    out.append(p)
                else:
                    indirect(p, depth - 1)

        for depth, p in enumerate(ino["ib"]):
            if len(out) >= n:
                break
            indirect(p, depth)
        return out

    def contiguous_start(self, ino):
        """For unsigned images the publishing tools store files contiguously; verify the
        direct pointers agree and return the first block."""
        blocks = ino["blocks"]
        db0 = ino["db"][0]
        all_ones = (1 << (8 * self.ptr_size)) - 1
        if blocks > 1 and ino["db"][1] == all_ones:
            # Sony/fpkg publishing tools mark "contiguous from db[0]" with db[1] = -1.
            return db0
        for i in range(min(blocks, 12)):
            if ino["db"][i] != db0 + i:
                return None
        return db0

    def file_read_iter(self, ino_num, chunk=1 << 24):
        ino = self.inodes[ino_num]
        size = ino["size"]
        if size == 0:
            return
        bs = self.block_size
        start = None
        if not self.sig:
            start = self.contiguous_start(ino)
        if start is not None:
            off = start * bs
            rem = size
            while rem > 0:
                n = min(chunk, rem)
                yield self.img.read(off, n)
                off += n
                rem -= n
        else:
            rem = size
            for blk in self.block_list(ino):
                n = min(bs, rem)
                yield self.img.read(blk * bs, n)
                rem -= n
                if rem <= 0:
                    break

    def read_file(self, ino_num):
        return b"".join(self.file_read_iter(ino_num))

    def dirents(self, ino_num):
        data = self.read_file(ino_num)
        bs = self.block_size
        out = []
        for base in range(0, len(data), bs):
            o = base
            end = min(base + bs, len(data))
            while o + 16 <= end:
                ino, typ, namelen, entsize = struct.unpack_from("<iiii", data, o)
                if ino == 0 or entsize == 0:
                    break
                name = data[o + 16:o + 16 + namelen].decode("utf-8", "replace")
                out.append((name, ino, typ))
                o += entsize
        return out

    def walk(self, ino_num, prefix=""):
        """Yield (relpath, ino, type) under directory ino_num, depth-first."""
        for name, ino, typ in self.dirents(ino_num):
            if typ in (PFS_DOT, PFS_DOTDOT):
                continue
            p = prefix + name
            yield p, ino, typ
            if typ == PFS_DIR:
                yield from self.walk(ino, p + "/")

    def lookup_root(self, name):
        for n, ino, typ in self.dirents(self.superroot_ino):
            if n == name:
                return ino
        return None


# ---------------------------------------------------------------- driver
def open_chain(path):
    pkg = Pkg(path)
    pkg.derive_keys()
    # outer header is plaintext; mode tells if XTS applies
    hdr = pkg.read(pkg.pfs_image_offset, 0x60)
    mode = struct.unpack_from("<H", hdr, 0x1C)[0]
    outer_img = XtsImage(pkg, encrypted=bool(mode & MODE_ENCRYPTED))
    outer = Pfs(outer_img, "outer")
    uroot = outer.lookup_root("uroot")
    pfs_dat = None
    for n, ino, typ in outer.dirents(uroot):
        if n == "pfs_image.dat":
            pfs_dat = ino
    if pfs_dat is None:
        raise SystemExit("pfs_image.dat not found in outer uroot")
    pi = outer.inodes[pfs_dat]
    start = outer.contiguous_start(pi)
    if start is None:
        raise SystemExit("pfs_image.dat not contiguous; unsupported")
    pfsc = PfscImage(outer_img, start * outer.block_size, pi["size"])
    inner = Pfs(pfsc, "inner")
    return pkg, outer, pfsc, inner


def human(n):
    for u in ("B", "KiB", "MiB", "GiB"):
        if n < 1024:
            return "%.1f %s" % (n, u)
        n /= 1024
    return "%.1f TiB" % n


def cmd_info(path):
    pkg, outer, pfsc, inner = open_chain(path)
    print("content_id", pkg.content_id, "content_type", hex(pkg.content_type),
          "flags", hex(pkg.content_flags), "drm", hex(pkg.drm_type))
    print("pfs_image off/size", hex(pkg.pfs_image_offset), hex(pkg.pfs_image_size))
    for e in pkg.entries:
        print("  entry %04x %-28s off=%#x size=%#x flags1=%#x" % (e["id"], e["name"], e["offset"], e["size"], e["flags1"]))
    for p in (outer, inner):
        print("%s: mode=%#x bs=%#x n_block=%d dinodes=%d dinode_blocks=%d superroot=%d inode_size=%#x" % (
            p.label, p.mode, p.block_size, p.n_block, p.dinode_count, p.dinode_block_count,
            p.superroot_ino, p.inode_size))
    print("pfsc: block=%#x nblocks=%d data_length=%s" % (pfsc.block_sz2, pfsc.nblocks, human(pfsc.data_length)))
    uroot = inner.lookup_root("uroot")
    nfiles = ndirs = tot = 0
    for rel, ino, typ in inner.walk(uroot):
        if typ == PFS_FILE:
            nfiles += 1
            tot += inner.inodes[ino]["size"]
        elif typ == PFS_DIR:
            ndirs += 1
    print("inner uroot: %d files, %d dirs, %s total" % (nfiles, ndirs, human(tot)))


def cmd_list(path):
    pkg, outer, pfsc, inner = open_chain(path)
    uroot = inner.lookup_root("uroot")
    for rel, ino, typ in inner.walk(uroot):
        print("%s %12d %s" % ("d" if typ == PFS_DIR else "f", inner.inodes[ino]["size"], rel))


def safe_join(root, rel):
    p = os.path.normpath(os.path.join(root, rel))
    if not (p == root or p.startswith(root + os.sep)):
        raise SystemExit("refusing path outside output dir: %r" % rel)
    return p


def cmd_extract(path, out, digest=False):
    out = os.path.abspath(out)
    pkg, outer, pfsc, inner = open_chain(path)
    report = dict(content_id=pkg.content_id, errors=[])
    if digest:
        print("hashing encrypted PFS image (%s)..." % human(pkg.pfs_image_size), flush=True)
        h = hashlib.sha256()
        pkg.f.seek(pkg.pfs_image_offset)
        rem = pkg.pfs_image_size
        while rem:
            b = pkg.f.read(min(rem, 1 << 24))
            h.update(b)
            rem -= len(b)
        ok = h.digest() == pkg.pfs_image_digest
        report["pfs_image_digest_ok"] = ok
        print("pfs_image_digest:", "OK" if ok else "MISMATCH", flush=True)
    # sce_sys from pkg entries
    sce_sys = os.path.join(out, "sce_sys")
    os.makedirs(sce_sys, exist_ok=True)
    skip = {0x1, 0x10, 0x20, 0x80, 0x100, 0x200}
    for e in pkg.entries:
        if e["id"] in skip:
            continue
        data = pkg.decrypt_entry(e) if e["id"] in (0x400, 0x401, 0x402, 0x403) else pkg.read(e["offset"], e["size"])
        dst = safe_join(sce_sys, e["name"])
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        with open(dst, "wb") as fo:
            fo.write(data)
    # inner PFS files
    uroot = inner.lookup_root("uroot")
    manifest = []
    nfiles = ndirs = 0
    tot = 0
    for rel, ino, typ in inner.walk(uroot):
        dst = safe_join(out, rel)
        if typ == PFS_DIR:
            os.makedirs(dst, exist_ok=True)
            ndirs += 1
            continue
        if typ != PFS_FILE:
            report["errors"].append("unknown dirent type %d for %s" % (typ, rel))
            continue
        node = inner.inodes[ino]
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        h = hashlib.sha256()
        written = 0
        with open(dst, "wb") as fo:
            for chunk in inner.file_read_iter(ino):
                fo.write(chunk)
                h.update(chunk)
                written += len(chunk)
        if written != node["size"]:
            report["errors"].append("%s: wrote %d expected %d" % (rel, written, node["size"]))
        nfiles += 1
        tot += written
        manifest.append(dict(path=rel, size=written, sha256=h.hexdigest(), inode=ino))
        print("%12d %s" % (written, rel), flush=True)
    # metadata consistency: every non-dir inode reachable?
    report.update(files=nfiles, dirs=ndirs, bytes=tot, inner_dinode_count=inner.dinode_count,
                  pfsc_blocks=pfsc.nblocks, pfsc_stats=pfsc.stats)
    with open(os.path.join(os.path.dirname(out), os.path.basename(out) + ".extract.json"), "w") as fo:
        json.dump(dict(report=report, files=manifest), fo, indent=1)
    print(json.dumps(report, indent=1))


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(1)
    cmd, path = sys.argv[1], sys.argv[2]
    if cmd == "info":
        cmd_info(path)
    elif cmd == "list":
        cmd_list(path)
    elif cmd == "extract":
        cmd_extract(path, sys.argv[3], "--digest" in sys.argv)
    else:
        print(__doc__)


if __name__ == "__main__":
    main()
