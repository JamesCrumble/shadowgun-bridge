#!/usr/bin/env python3
"""Turns the 'pcprof' lines of a phone log into per-function totals.

Usage: tools/pcprof.py [log] [module-dir]      (defaults: logs/phone.log, sgdata/lib/armeabi-v7a)

The engine samples itself (src/pc_prof.cpp): every 10 s it logs, per module, the hottest 64-byte blocks as
"0xOFFSET=count". Offsets are relative to the module's text base, i.e. ELF virtual addresses. A block is given to
the function that contains its start: by symbol table when the module has one, else by the ARM unwind index
(.ARM.exidx lists every function's start, so even a stripped libunity.so splits into functions - unnamed, but
with an address to disassemble). Host code (our loader, glibc) is shown by file offset only.
"""
import bisect
import collections
import re
import struct
import subprocess
import sys

log = sys.argv[1] if len(sys.argv) > 1 else "logs/phone.log"
libdir = sys.argv[2] if len(sys.argv) > 2 else "sgdata/lib/armeabi-v7a"
NM = "arm-linux-gnueabihf-nm"
READELF = "arm-linux-gnueabihf-readelf"


def symbols(path):
    out = subprocess.run([NM, "-C", "--defined-only", "-n", path], capture_output=True, text=True).stdout
    syms = []
    for line in out.splitlines():
        p = line.split(None, 2)
        if len(p) == 3 and p[1] in "tTwW":
            syms.append((int(p[0], 16) & ~1, p[2]))
    return syms


def exidx_starts(path):
    """Function start addresses from .ARM.exidx (prel31 entries, 8 bytes each)."""
    hdr = subprocess.run([READELF, "-S", "-W", path], capture_output=True, text=True).stdout
    m = re.search(r"\.ARM\.exidx\s+\S+\s+([0-9a-f]+)\s+([0-9a-f]+)\s+([0-9a-f]+)", hdr)
    if not m:
        return []
    addr, off, size = (int(x, 16) for x in m.groups())
    data = open(path, "rb").read()[off:off + size]
    starts = []
    for i in range(0, len(data) - 7, 8):
        w = struct.unpack_from("<i", data, i)[0]
        w = (w & 0x7FFFFFFF) | (-(w & 0x40000000) & ~0x7FFFFFFF)     # sign-extend prel31
        starts.append((addr + i + w) & ~1)
    return sorted(set(starts))


def load(tag):
    out = collections.defaultdict(collections.Counter)    # module -> block -> count
    for line in open(log, errors="replace"):
        m = re.match(r"TRACE: " + tag + r"\[([^\]]+)\]((?: 0x[0-9a-f]+=\d+)+)", line)
        if m:
            for off, n in re.findall(r"0x([0-9a-f]+)=(\d+)", m.group(2)):
                out[m.group(1)][int(off, 16)] += int(n)
    return out


samples = load("pcprof")
incl = load("pcprof_incl")
waits = collections.Counter()
for line in open(log, errors="replace"):
    if "pcprof_wait:" in line:
        for k, n in re.findall(r"\[([^\]]+)\]=(\d+)", line):
            waits[k] += int(n)

if not samples:
    sys.exit("no pcprof lines in " + log)

resolvers = {}


def resolver(mod):
    if mod in resolvers:
        return resolvers[mod]
    path = "%s/%s" % (libdir, mod.replace(".so.debug", ".so"))
    keys, names = [], []
    if not mod.startswith("host:"):
        try:
            syms = symbols(path)
            if len(syms) > 1000:
                keys = [x[0] for x in syms]
                names = [x[1] for x in syms]
            else:
                keys = exidx_starts(path)
                names = ["sub_%x" % k for k in keys]
        except Exception as e:
            print("   (no symbols: %s)" % e)
    resolvers[mod] = (keys, names)
    return resolvers[mod]


def name_of(mod, off):
    keys, names = resolver(mod)
    i = bisect.bisect_right(keys, off) - 1
    return names[i] if i >= 0 else "0x%x" % off


def per_function(table, mod):
    per = collections.Counter()
    for off, n in table[mod].items():
        per[name_of(mod, off)] += n
    return per


grand = sum(sum(c.values()) for c in samples.values())
print("module shares (blocks above the 0.1%% cut, %d samples):" % grand)
for mod, c in sorted(samples.items(), key=lambda kv: -sum(kv[1].values())):
    print("  %-34s %6.1f%%" % (mod, 100.0 * sum(c.values()) / grand))

for mod, c in sorted(samples.items(), key=lambda kv: -sum(kv[1].values())):
    total = sum(c.values())
    if total * 100 < grand:
        continue
    print("\n== %s: %d samples (%.1f%% of the thread), self time" % (mod, total, 100.0 * total / grand))
    for name, n in per_function(samples, mod).most_common(25):
        print("  %5.1f%% %s" % (100.0 * n / grand, name[:150]))

# Inclusive: a sample counts for every function in its caller chain (lr + return addresses found on the stack;
# heuristic, so recursion and stale stack words can inflate it a little).
if incl:
    print("\n##### INCLUSIVE (function is on the call chain; shares of all samples) #####")
    tot = collections.Counter()
    for mod in incl:
        if mod.startswith("host:"):
            continue
        for name, n in per_function(incl, mod).items():
            tot[(mod, name)] += n
    for (mod, name), n in tot.most_common(40):
        print("  %5.1f%% %-14s %s" % (100.0 * n / grand, mod[:14], name[:120]))

if waits:
    print("\n##### WAITING in a syscall: syscall nr (240=futex,162=nanosleep,263=clock_gettime,...) + game caller #####")
    for k, n in waits.most_common(15):
        m = re.match(r"nr=(\d+) (\S+)\+0x([0-9a-f]+)", k)
        if m:
            k += "   -> " + name_of(m.group(2), int(m.group(3), 16))
        print("  %5.1f%% %s" % (100.0 * n / grand, k))
