#!/usr/bin/env python3
"""dexclass.py <Lpkg/Class;> [method] — lists a class's methods from scratch/dex.txt (dexdump -d), or prints one method's invokes/strings."""
import re, sys
t = open('scratch/dex.txt', encoding='latin-1').read()
c = sys.argv[1]
i = t.find("Class descriptor  : '" + c + "'")
if i < 0:
    print('  (absent)'); sys.exit()
j = t.find("Class descriptor", i + 10)
blk = t[i:j]
if len(sys.argv) < 3:
    for m in re.finditer(r"name\s+: '([^']+)'\n\s+type\s+: '([^']+)'\n\s+access\s+: (\S+) \(([^)]*)\)", blk):
        if '(' in m.group(2):
            print('  ', m.group(4)[:22].ljust(22), m.group(1), m.group(2))
else:
    for p in re.split(r"\n    #\d+\s+: \(in ", blk)[1:]:
        m = re.search(r"name\s+: '([^']+)'\n\s+type\s+: '([^']+)'", p)
        if not m or m.group(1) != sys.argv[2]:
            continue
        print('=====', m.group(1), m.group(2))
        for l in p.split('\n'):
            if re.search(r'\|[0-9a-f]{4}: ', l):
                x = re.sub(r'^.*\|[0-9a-f]+: ', '', l); x = re.sub(r' // \w+@\w+$', '', x)
                if len(sys.argv) > 3 or 'invoke' in x or 'const-string' in x or 'return' in x:
                    print('  ', x[:170])
