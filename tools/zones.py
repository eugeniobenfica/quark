"""Writes std/zones.qk: every IANA time zone's rule as POSIX TZ text (the footer of its TZif file), for std.time where
the system has no zone files (Windows, WebAssembly). Its current rule only: the system's files, when there, hold each
zone's history too.
  python tools/zones.py [/usr/share/zoneinfo] && quark fmt std/zones.qk --write"""
import os
import sys

root = sys.argv[1] if len(sys.argv) > 1 else '/usr/share/zoneinfo'
skip = {'posix', 'right', 'Factory', 'posixrules', 'localtime', 'SECURITY'}
version = '?'
try:
    for line in open(os.path.join(root, 'tzdata.zi'), encoding='utf-8'):
        if line.startswith('# version'):
            version = line.split()[2].replace('-dirty', '')
            break
except OSError:
    pass
rows = []
for d, dirs, files in os.walk(root):
    rel = os.path.relpath(d, root).replace(os.sep, '/')
    if rel.split('/')[0] in skip:
        continue
    for f in files:
        name = f if rel == '.' else rel + '/' + f
        if f in skip or '.' in f:
            continue
        data = open(os.path.join(d, f), 'rb').read()
        if not data.startswith(b'TZif') or not data.endswith(b'\n'):
            continue
        footer = data[data.rindex(b'\n', 0, len(data) - 1) + 1:-1].decode()
        if footer:
            rows.append((name, footer))
rows.sort()
with open('std/zones.qk', 'w', encoding='utf-8', newline='\n') as out:
    out.write("// Made by tools/zones.py from the IANA time zone database (tzdata %s, public domain): each zone's rule now,\n" % version)
    out.write('// as POSIX TZ text, for std.time where the system has no zone files (Windows, WebAssembly).\n')
    out.write('pub const tzdataVersion = "%s"\n\n' % version)
    out.write('// each zone\'s "name rule"\n')
    out.write('pub const zoneRules = [\n')
    for name, footer in rows:
        out.write('    "%s %s",\n' % (name, footer))
    out.write(']\n')
print(len(rows), 'zones', version)
