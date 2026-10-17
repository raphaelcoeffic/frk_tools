#!/usr/bin/env python3
#
# Extract a flash dump from a raw serial capture of the flashdump firmware.
#
# The dumper repeats rounds framed as:
#   "FDMP" | start (u32 LE) | length (u32 LE) | data | CRC-32 (u32 LE)
#
# Every round with a valid CRC is checked against the others; the first one
# is written out.

import sys
import struct
import zlib
import argparse

MAGIC = b'FDMP'
HEADER = struct.Struct('<4sII')

def find_rounds(capture):
    rounds = []
    pos = capture.find(MAGIC)
    while pos != -1:
        if pos + HEADER.size <= len(capture):
            _, start, length = HEADER.unpack_from(capture, pos)
            data_pos = pos + HEADER.size
            crc_pos = data_pos + length
            if length > 0 and crc_pos + 4 <= len(capture):
                data = capture[data_pos:crc_pos]
                crc, = struct.unpack_from('<I', capture, crc_pos)
                if zlib.crc32(data) == crc:
                    rounds.append((pos, start, data))
        pos = capture.find(MAGIC, pos + 1)
    return rounds

def parseArgs():
    parser = argparse.ArgumentParser(description='Extract a flash dump from a serial capture')

    parser.add_argument('capture', type=argparse.FileType('rb'),
                        help='raw capture of the dumper output')

    parser.add_argument('outfile', type=argparse.FileType('wb'),
                        help='where to write the dumped flash')

    return parser.parse_args()

cmdline = parseArgs()
rounds = find_rounds(cmdline.capture.read())

if not rounds:
    sys.exit('error: no complete round with a valid CRC found')

pos, start, data = rounds[0]
for other_pos, other_start, other_data in rounds[1:]:
    if (other_start, other_data) != (start, data):
        sys.exit('error: rounds at offsets %d and %d differ' % (pos, other_pos))

cmdline.outfile.write(data)
print('0x%08X..0x%08X: %d bytes, %d valid round(s)'
      % (start, start + len(data), len(data), len(rounds)), file=sys.stderr)
