#!/usr/bin/env python3
"""Check that IRIX n32 binaries resolve: every undefined, non-weak dynamic
symbol must be defined by some object in the DT_NEEDED closure, as rld would
load it.  Also: `span FILE` prints the lowest and highest address of its
PT_LOADs (memory size, so bss counts).

  undef-check.py [--shim LIBGL] [--lib NAME=PATH]... [--mips4] FILE...
  undef-check.py span FILE

NEEDED names are looked up in: the --lib map, then (libGL.so) --shim, then the
SGI extras ($SGI/usr/lib32), the sysroot's usr/lib32/mips3 (or mips4 with
--mips4), usr/lib32 and lib32.  The libraries loaded are the ones the build
linked against, so this proves the link closure, not the guest's copies.
"""
import os
import struct
import sys

SYSROOT = os.environ['SYSROOT']       # both set by build.sh (clang/common.sh)
SGI = os.environ['SGI']

# Provided by rld or by the main program to the libraries it loads.
RLD = {'_rld_new_interface', '__rld_obj_head', '_DYNAMIC_LINK', '_DYNAMIC_LINKING',
       '_end', '_ftext', '_fdata', '_fbss', '_etext', '_edata', '__Argc', '__Argv',
       '_gp_disp', '__elf_header', '__program_header_table', '__dso_displacement',
       '__start', '_procedure_table', '_procedure_table_size', '_procedure_string_table',
       '_DYNAMIC', '_GLOBAL_OFFSET_TABLE_', '__rld_map', '_lib_version', '__istart'}


class Elf:
    def __init__(self, path):
        self.path = path
        d = open(path, 'rb').read()
        if d[:4] != b'\x7fELF' or d[4] != 1 or d[5] != 2:
            raise SystemExit('%s: not ELF32 big-endian' % path)
        (e_type, e_machine, _, _, e_phoff, e_shoff, e_flags, _, e_phentsize, e_phnum,
         e_shentsize, e_shnum, e_shstrndx) = struct.unpack('>HHIIIIIHHHHHH', d[16:52])
        self.type = e_type
        self.flags = e_flags
        self.loads = []
        for i in range(e_phnum):
            p = struct.unpack('>IIIIIIII', d[e_phoff + i * e_phentsize:e_phoff + i * e_phentsize + 32])
            if p[0] == 1:
                self.loads.append((p[2], p[5]))
        secs = []
        for i in range(e_shnum):
            secs.append(struct.unpack('>IIIIIIIIII', d[e_shoff + i * e_shentsize:e_shoff + i * e_shentsize + 40]))
        self.syms = []   # (name, shndx, bind)
        self.needed, self.soname, self.rpath = [], None, []
        for s in secs:
            if s[1] == 11:      # SHT_DYNSYM
                strtab = secs[s[6]]
                st = d[strtab[4]:strtab[4] + strtab[5]]
                for j in range(s[5] // 16):
                    nm, val, size, info, other, shndx = struct.unpack('>IIIBBH', d[s[4] + j * 16:s[4] + j * 16 + 16])
                    name = st[nm:st.index(b'\0', nm)].decode('latin1')
                    if name:
                        self.syms.append((name, shndx, info >> 4))
            if s[1] == 6:       # SHT_DYNAMIC
                strtab = secs[s[6]]
                st = d[strtab[4]:strtab[4] + strtab[5]]
                for j in range(s[5] // 8):
                    tag, val = struct.unpack('>iI', d[s[4] + j * 8:s[4] + j * 8 + 8])
                    if tag == 0:
                        break
                    if tag in (1, 14, 15, 29):
                        v = st[val:st.index(b'\0', val)].decode('latin1')
                        if tag == 1:
                            self.needed.append(v)
                        elif tag == 14:
                            self.soname = v
                        else:
                            self.rpath.append(v)

    def defined(self):
        return {n for n, shndx, bind in self.syms if shndx != 0}

    def undefined(self):
        return {n for n, shndx, bind in self.syms if shndx == 0 and bind == 1}


def main():
    args = sys.argv[1:]
    if args and args[0] == 'span':
        e = Elf(args[1])
        lo = min(v for v, m in e.loads)
        hi = max(v + m for v, m in e.loads)
        print('0x%08x 0x%08x' % (lo, hi))
        return 0
    libmap = {}
    sub = 'mips3'
    files = []
    while args:
        a = args.pop(0)
        if a == '--shim':
            libmap['libGL.so'] = args.pop(0)
        elif a == '--lib':
            k, v = args.pop(0).split('=', 1)
            libmap[k] = v
        elif a == '--mips4':
            sub = 'mips4'
        else:
            files.append(a)
    dirs = [SGI + '/usr/lib32', SYSROOT + '/usr/lib32/' + sub, SYSROOT + '/usr/lib32', SYSROOT + '/lib32']

    def find(name):
        if name in libmap:
            return libmap[name]
        for d in dirs:
            p = os.path.join(d, name)
            if os.path.exists(p):
                return p
        return None

    cache = {}
    bad = 0
    for f in files:
        root = Elf(f)
        seen, order, missing, missing_t = set(), [], [], []
        queue = list(root.needed)
        direct = set(root.needed)
        while queue:
            n = queue.pop(0)
            if n in seen:
                continue
            seen.add(n)
            p = find(n)
            if not p:
                (missing if n in direct else missing_t).append(n)
                continue
            if p not in cache:
                cache[p] = Elf(p)
            order.append(cache[p])
            queue.extend(cache[p].needed)
        have = root.defined()
        for e in order:
            have |= e.defined()
        unres = sorted(root.undefined() - have - RLD)
        # libraries' own undefineds are their business, except where they
        # expect *this* object to provide something
        tag = os.path.basename(f)
        if missing:
            print('   %s: NEEDED not found for the check: %s' % (tag, ' '.join(missing)))
            bad = 1
        if missing_t:
            # a library's own dependency, not in the link sysroot (e.g.
            # libdmedia -> libmutex.so); the program's symbols are still checked
            print('   %s: (note: indirect dependency not in the sysroot: %s)' % (tag, ' '.join(missing_t)))
        if unres:
            print('   %s: UNRESOLVED: %s' % (tag, ' '.join(unres)))
            bad = 1
        else:
            print('   %s: all %d undefined symbols resolve in %s' % (
                tag, len(root.undefined()), ' '.join(os.path.basename(e.path) for e in order)))
    return bad


if __name__ == '__main__':
    sys.exit(main())
