import struct, sys
p = sys.argv[1]
d = open(p, 'rb').read()
be = lambda o, n: int.from_bytes(d[o:o+n], 'big')
meta_size, file_size, ver, data_off = struct.unpack('>IIII', d[:16])
print('meta', meta_size, 'size', file_size, 'ver', ver, 'data_off', data_off)
o = 16
endian = d[o]; o += 4
assert endian == 0, 'little endian meta expected'  # 0 = little
e = '<'
end = d.index(b'\0', o); uv = d[o:end].decode(); o = end + 1
plat, = struct.unpack(e + 'i', d[o:o+4]); o += 4
tt = d[o]; o += 1
nt, = struct.unpack(e + 'i', d[o:o+4]); o += 4
print('unity', uv, 'platform', plat, 'typetree', tt, 'types', nt)
types = []
for i in range(nt):
    cid, = struct.unpack(e + 'i', d[o:o+4]); o += 4
    if ver >= 16:
        o += 1
    if ver >= 17:
        o += 2
    if cid == 114:
        o += 16
    o += 16
    types.append(cid)
    if tt:
        raise SystemExit('typetree present, parser does not handle')
cnt, = struct.unpack(e + 'i', d[o:o+4]); o += 4
objs = []
for i in range(cnt):
    o = (o + 3) & ~3
    pid, = struct.unpack(e + 'q', d[o:o+8]); o += 8
    bs, bz, tid = struct.unpack(e + 'IIi', d[o:o+12]); o += 12
    cid, sti = struct.unpack(e + 'hh', d[o:o+4]); o += 4
    o += 1  # stripped
    objs.append((pid, bs, bz, tid, cid))
import collections
print('objects', cnt)
for pid, bs, bz, tid, cid in objs:
    print('  path', pid, 'class', cid, 'off', data_off + bs, 'size', bz)
open('/tmp/ggm_objs.txt', 'w').write('\n'.join('%d %d %d %d' % (pid, cid, data_off + bs, bz) for pid, bs, bz, tid, cid in objs))
