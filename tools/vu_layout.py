#!/usr/bin/env python3
# Turns a PS2X_VU_LAYOUT_LOG file into a games/<id>/config/vu_layout.txt table:
#   python3 tools/vu_layout.py <log> <boot ELF> | grep ^vu
# Reads a PS2X_VU_LAYOUT_LOG file and finds each upload's bytes in the game's ELF.
# Prints the layout table: unit, ELF file offset, VU address, length.
import struct, sys, collections
log = open(sys.argv[1], 'rb').read()
elf = open(sys.argv[2], 'rb').read()
recs = []
p = 0
while p + 16 <= len(log):
    magic, unit, dest, length = struct.unpack_from('<IIII', log, p); p += 16
    assert magic == 0x594C5556
    recs.append((unit, dest, log[p:p + length])); p += length
found = notfound = 0
table = set()
for unit, dest, data in recs:
    at = elf.find(data)
    if at < 0:
        notfound += 1
        print('  NOT IN ELF: vu%d dest=0x%04x len=%d' % (unit, dest, len(data)))
        continue
    found += 1
    table.add((unit, at, dest, len(data)))
print('%d uploads, %d found in the ELF, %d not' % (len(recs), found, notfound))
for unit, at, dest, length in sorted(table, key=lambda t: (t[0], t[2], t[1])):
    print('vu%d elf=0x%06x vu=0x%04x len=%d' % (unit, at, dest, length))
