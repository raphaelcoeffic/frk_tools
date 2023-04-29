#!/usr/bin/env python

import sys
import argparse

MAGIC_KEY_OFFSET = 6
MAGIC_KEY_START  = 0x6569B868

#
# Magic sequence #1: used for most of the (de-)scrambling
#
# M(0): int32 @ offset 24 bytes inside each 2KB block
# M(n+1) = (0x7FF8A3ED * M(n) + 0x2AA01D31)  & 0xFFFFFFFF
#
magic_number = 1

def init_magic_number(n):
    global magic_number

    magic_number = n
    if n == 0:
        magic_number = 1
        

def next_magic_number():
    global magic_number

    magic_number = (0x7FF8A3ED * magic_number + 0x2AA01D31)  & 0xFFFFFFFF
    return magic_number

#
# Magic sequence #2: used for the start address of each block
#
# M(0): int32 @ offset 24 bytes inside each 2KB block
# M(1): (0xAA * M(0) + 0x7673) & 0xFFFF
#

def magic2(seed):
    return (0xAA * (seed & 0xFFFF) % 0x7673) & 0xFFFF


def get_uint_from_bytes(b,i):
    return int.from_bytes(b[i*4:(i+1)*4], 'little', signed=False)

def get_uints_from_bytes(b):

    res = []
    for i in range(len(b)>>2):
        n = b[i*4]
        n = n + (b[i*4+1] <<  8)
        n = n + (b[i*4+2] << 16)
        n = n + (b[i*4+3] << 24)
        res.append(n)

    return res

def get_bytes_from_uints(ints):

    l = len(ints)
    res = []

    # generate bytes for little endian
    for i in range(l):
        res.append(ints[i] & 0xFF)
        res.append((ints[i] >> 8) & 0xFF)
        res.append((ints[i] >> 16) & 0xFF)
        res.append((ints[i] >> 24) & 0xFF)

    return bytes(res)

def parseAddr(s):
    return int(s,0)

def parseArgs():
    parser = argparse.ArgumentParser(description='Convert from .bin to .frk')

    parser.add_argument('start_address', type=parseAddr,
                        help='Start Address (ex: 0x08002000, 0x00008000)')

    parser.add_argument('infile', nargs='?', type=argparse.FileType('rb'),
                        default=sys.stdin)
    
    parser.add_argument('outfile', nargs='?', type=argparse.FileType('wb+'),
                        default=sys.stdout)

    args = parser.parse_args()
    return args


#
# Main
#

cmdline = parseArgs()

blocks = 0
infile  = cmdline.infile
outfile = cmdline.outfile
addr    = cmdline.start_address

magic_init = True

while True:

    buffer = infile.read(1024)
    if len(buffer) == 0:
        break

    if len(buffer) < 1024:
        buffer = buffer + bytes(0xFF for i in range(1024 - len(buffer)))

    if magic_init:
        init_magic_number(MAGIC_KEY_START)
        magic_init = False

    out = bytes(0x13E) + buffer + bytes(1024 - 0x13E)
    out = get_uints_from_bytes(out)

    out[0] = 1024       # block size
    out[1] = addr       # start address
    out[3] = 0x13E - 32 # start offset - 32

    out[MAGIC_KEY_OFFSET] = magic_number

    addr = addr + 1024

    out[1] = (out[1] - magic2(magic_number)) & 0xFFFFFFFF
    code_offset = (out[3] + 32) / 4

    # for each dword
    for i in range(512):

        r = next_magic_number()

        in_n  = out[i]
        
        # pass the seed for the magic sequences untouched
        if i == MAGIC_KEY_OFFSET:
            out_n = in_n

        # shuffle some bytes and XOR
        elif i & 0xF == 2:
            in_n = in_n ^ r
            out_n = ((in_n << 25) + (in_n >> 7)) & 0xFFFFFFFF

        # simple XOR
        else:
            out_n = in_n ^ r

        out[i] = out_n

    outfile.write(get_bytes_from_uints(out))

infile.close()
outfile.close()

