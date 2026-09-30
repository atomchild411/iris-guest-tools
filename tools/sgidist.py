#!/usr/bin/env python3
"""Read SGI inst distributions straight out of CD images (read-only).

A dist directory holds, per product P:  P (product descriptor), P.idb (text
index, one line per installed file) and one archive per image, P.<image>
(e.g. eoe.sw, eoe.man).  An idb line looks like
  f 0444 root sys usr/cpu/sysgen/IP28boot/mgras.a <srcpath> eoe.sw.gfx
    sum(9452) size(346180) off(74728278) nostrip
    mach(CPUBOARD=IP28 GFXBOARD=MGRAS) cmpsize(176268)
The subsystem field (eoe.sw.gfx) names the archive: the idb's stem plus the
subsystem's image component (eoe.idb -> eoe.sw; x_eoe_6530m.idb -> x_eoe_6530m.sw).  The archive starts with a 12-byte magic ("im001V630P00" etc.);
off(N) points at an entry: u16 big-endian name length, the name (the install
path again), then the data: cmpsize(C) bytes of Unix compress (LZW, 0x1f9d)
when cmpsize is present, else size(S) raw bytes.

usage:
  sgidist.py inv IMAGE REGEX            idb lines whose text matches REGEX (TSV)
  sgidist.py get IMAGE IDB PATH MACHRE OUTFILE
      extract the file PATH described in IDB (e.g. /dist/eoe.idb); when a path
      has several variants, MACHRE picks the one whose mach() matches ('' = first)
"""
import os, re, subprocess, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import efs

ATTR = re.compile(r'(\w+)\(([^()]*(?:\([^()]*\)[^()]*)*)\)')


SUBSYS = re.compile(r'^[\w+-]+\.[\w+-]+\.[\w+-]+$')


def parse(line):
    """Fields: type mode owner group path src, then attributes word(...) and
    bare tokens in any order; the subsystem is the bare prod.image.subsys
    token (older idbs put it right after src, 6.5.x overlay idbs at the end)."""
    t = line.split(None, 6)
    if len(t) < 6:
        return None
    d = dict(type=t[0], mode=t[1], owner=t[2], group=t[3], path=t[4], src=t[5], subsys='')
    rest = t[6] if len(t) > 6 else ''
    for k, v in ATTR.findall(rest):
        d.setdefault(k, v)
    for tok in ATTR.sub(' ', rest).split():
        if SUBSYS.match(tok):
            d['subsys'] = tok
            break
    return d


def archive_of(idb, subsys):
    # archive = <idb stem>.<image>: eoe.idb + eoe.sw.gfx -> eoe.sw, and
    # x_eoe_6530m.idb + x_eoe.sw.eoe -> x_eoe_6530m.sw (6.5.x overlay naming)
    return idb[:-4] + '.' + subsys.split('.')[1]


def idbs(img, ino=2, prefix=''):
    for p, mode, size, ci in img.walk(ino, prefix):
        if p.endswith('.idb') and (mode & 0o170000) == 0o100000:
            yield p, ci


def inventory(image, regex):
    img = efs.Image(image)
    rx = re.compile(regex)
    for p, ci in idbs(img):
        for line in img.data(ci).decode('latin1').splitlines():
            if rx.search(line):
                d = parse(line)
                if not d:
                    continue
                arch = archive_of(p, d['subsys'])
                yield dict(d, image=os.path.basename(image), idb=p, archive=arch, line=line)


def bsd_sum(b):
    """BSD 16-bit rotating checksum (sum -r); what idb sum(N) records."""
    c = 0
    for x in b:
        c = ((c >> 1) | ((c & 1) << 15)) + x
        c &= 0xffff
    return c


def read_entry(img, archive, c):
    """Return the decompressed bytes of idb entry c from archive (an EFS path)."""
    ai = img.lookup(archive)
    off = int(c['off'])
    nl = int.from_bytes(img.copy_range(ai, off, 2), 'big')
    name = img.copy_range(ai, off + 2, nl).decode('latin1')
    if name != c['path']:
        raise SystemExit('archive entry name %r != %r' % (name, c['path']))
    size = int(c['size'])
    n = int(c['cmpsize']) if c.get('cmpsize', '0') != '0' else size
    blob = img.copy_range(ai, off + 2 + nl, n)
    if c.get('cmpsize', '0') != '0':
        blob = subprocess.run(['gzip', '-dc'], input=blob, stdout=subprocess.PIPE, check=True).stdout
    if len(blob) != size:
        raise SystemExit('size mismatch for %s: %d != %d' % (c['path'], len(blob), size))
    if 'sum' in c and bsd_sum(blob) != int(c['sum']):
        raise SystemExit('checksum mismatch for %s: %d != %s' % (c['path'], bsd_sum(blob), c['sum']))
    return blob


def extract(image, idb, path, machre, out):
    img = efs.Image(image)
    lines = img.data(img.lookup(idb)).decode('latin1').splitlines()
    cands = [parse(l) for l in lines]
    cands = [c for c in cands if c and c['path'] == path and c['type'] == 'f']
    if machre:
        cands = [c for c in cands if re.search(machre, c.get('mach', ''))]
    if not cands:
        raise SystemExit('no idb entry for %s' % path)
    c = cands[0]
    arch = archive_of(idb, c['subsys'])
    blob = read_entry(img, arch, c)
    os.makedirs(os.path.dirname(out) or '.', exist_ok=True)
    with open(out, 'wb') as o:
        o.write(blob)
    return c, arch


def main():
    cmd = sys.argv[1]
    if cmd == 'inv':
        print('\t'.join(['image', 'idb', 'type', 'path', 'subsys', 'size', 'cmpsize', 'off', 'archive', 'mach', 'sum']))
        for d in inventory(sys.argv[2], sys.argv[3]):
            print('\t'.join([d['image'], d['idb'], d['type'], d['path'], d['subsys'], d.get('size', ''),
                             d.get('cmpsize', ''), d.get('off', ''), d['archive'], d.get('mach', ''), d.get('sum', '')]))
    elif cmd == 'get':
        c, arch = extract(*sys.argv[2:7])
        print('%s <- %s off(%s) size(%s) cmpsize(%s) mach(%s)' % (sys.argv[6], arch, c['off'], c['size'],
                                                                  c.get('cmpsize', '-'), c.get('mach', '')))


if __name__ == '__main__':
    main()
