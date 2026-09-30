#!/usr/bin/env python3
"""Minimal read-only reader for SGI CD/disk images: SGI volume header + EFS.

Opens the image read-only and never writes to it.

usage:
  efs.py IMAGE ls [PATH]          list a directory (default /)
  efs.py IMAGE find               list every file (path, size)
  efs.py IMAGE cat PATH > out     copy a file to stdout
  efs.py IMAGE get PATH OUT       copy a file to OUT
"""
import struct, sys, os

BB = 512


class Image:
    def __init__(self, path):
        self.f = open(path, 'rb')
        self.base = self._find_efs()
        sb = self.read(self.base + BB, 92)
        (self.size, self.firstcg, self.cgfsize, self.cgisize, self.sectors,
         self.heads, self.ncg, dirty) = struct.unpack('>iiihhhhh', sb[:22])
        magic = struct.unpack('>I', sb[28:32])[0]
        if magic not in (0x072959, 0x07295a):
            raise SystemExit('no EFS superblock (magic %#x)' % magic)
        self.inopb = BB // 128
        self.inopcg = self.cgisize * self.inopb

    def read(self, off, n):
        self.f.seek(off)
        return self.f.read(n)

    def _find_efs(self):
        vh = self.read(0, 512)
        if struct.unpack('>I', vh[:4])[0] != 0x0be5a941:
            return 0  # bare EFS
        for i in range(16):
            nblks, first, typ = struct.unpack('>iii', vh[0x138 + 12*i:0x138 + 12*i + 12])
            if typ == 5 and nblks > 0:  # PTYPE_EFS
                return first * BB
        raise SystemExit('volume header has no EFS partition')

    def inode(self, ino):
        cg = ino // self.inopcg
        bn = self.firstcg + cg * self.cgfsize + (ino % self.inopcg) // self.inopb
        raw = self.read(self.base + bn * BB + (ino % self.inopb) * 128, 128)
        mode, nlink, uid, gid, size = struct.unpack('>HhHHi', raw[:12])
        nex = struct.unpack('>h', raw[28:30])[0]
        return mode, size, nex, raw[32:128]

    @staticmethod
    def _ext(b):
        bn = int.from_bytes(b[1:4], 'big')
        length = b[4]
        offset = int.from_bytes(b[5:8], 'big')
        return bn, length, offset

    def extents(self, ino):
        mode, size, nex, u = self.inode(ino)
        if (mode & 0o170000) == 0o120000 and nex == 0:  # inline symlink
            return mode, size, None
        direct = [self._ext(u[8*i:8*i+8]) for i in range(12)]
        if nex <= 12:
            return mode, size, direct[:nex]
        nind = direct[0][2]  # number of indirect extents lives in ex_offset
        out = []
        for bn, length, _ in direct[:nind]:
            blk = self.read(self.base + bn * BB, length * BB)
            for j in range(len(blk) // 8):
                if len(out) == nex:
                    break
                out.append(self._ext(blk[8*j:8*j+8]))
        return mode, size, out

    def data(self, ino):
        mode, size, exts = self.extents(ino)
        if exts is None:
            return self.inode(ino)[3][:size]
        parts = []
        for bn, length, off in exts:
            parts.append((off, self.read(self.base + bn * BB, length * BB)))
        parts.sort()
        buf = bytearray()
        for off, p in parts:
            buf[off*BB:off*BB + len(p)] = p
        return bytes(buf[:size])

    def copy_range(self, ino, start, n):
        """Read n bytes at byte offset start of the file without loading it all."""
        mode, size, exts = self.extents(ino)
        out = bytearray()
        end = min(start + n, size)
        for bn, length, off in sorted(exts, key=lambda e: e[2]):
            es, ee = off * BB, (off + length) * BB
            lo, hi = max(es, start), min(ee, end)
            if lo < hi:
                out += self.read(self.base + bn * BB + (lo - es), hi - lo)
        return bytes(out)

    def listdir(self, ino):
        d = self.data(ino)
        out = []
        for b in range(0, len(d), BB):
            blk = d[b:b+BB]
            magic, first, slots = struct.unpack('>HBB', blk[:4])
            if magic != 0xbeef:
                continue
            for s in range(slots):
                o = blk[4 + s]
                if o == 0:
                    continue
                o *= 2
                ino_ = struct.unpack('>I', blk[o:o+4])[0]
                nl = blk[o+4]
                out.append((blk[o+5:o+5+nl].decode('latin1'), ino_))
        return out

    def lookup(self, path):
        ino = 2
        for part in [p for p in path.split('/') if p]:
            ents = dict(self.listdir(ino))
            if part not in ents:
                raise SystemExit('not found: %s' % path)
            ino = ents[part]
        return ino

    def walk(self, ino=2, prefix=''):
        for name, ci in self.listdir(ino):
            if name in ('.', '..'):
                continue
            mode, size, _, _ = self.inode(ci)
            p = prefix + '/' + name
            yield p, mode, size, ci
            if (mode & 0o170000) == 0o040000:
                yield from self.walk(ci, p)


def main():
    img = Image(sys.argv[1])
    cmd = sys.argv[2] if len(sys.argv) > 2 else 'ls'
    if cmd == 'ls':
        ino = img.lookup(sys.argv[3] if len(sys.argv) > 3 else '/')
        for name, ci in img.listdir(ino):
            mode, size, _, _ = img.inode(ci)
            print('%06o %10d %s' % (mode, size, name))
    elif cmd == 'find':
        for p, mode, size, _ in img.walk():
            print('%06o %10d %s' % (mode, size, p))
    elif cmd == 'cat':
        sys.stdout.buffer.write(img.data(img.lookup(sys.argv[3])))
    elif cmd == 'get':
        with open(sys.argv[4], 'wb') as o:
            o.write(img.data(img.lookup(sys.argv[3])))


if __name__ == '__main__':
    main()
