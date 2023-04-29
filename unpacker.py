#!/usr/bin/env python

import sys

MAGIC_KEY_OFFSET = 6

#
# Magic sequence #1: used for most of the de-scrambling
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


blocks = 0
infile  = open(sys.argv[1], 'rb')
outfile = open(sys.argv[2], 'wb+')

while True:

    buffer = infile.read(2048)
    if len(buffer) != 2048:
        break

    init_magic_number(get_uint_from_bytes(buffer, MAGIC_KEY_OFFSET))
    out = []

    # for each dword
    for i in range(512):
        r = next_magic_number()

        in_n  = get_uint_from_bytes(buffer, i)
        out_n = 0

        # pass the seed for the magic sequences untouched
        if i == MAGIC_KEY_OFFSET:
            out_n = in_n

        # shuffle some bytes and XOR
        elif i & 0xF == 2:
            out_n = (((in_n << 7) + (in_n >> 25)) ^ r) & 0xFFFFFFFF

        # simple XOR
        else:
            out_n = in_n ^ r

        out.append(out_n)

    # last de-scrambling for the flash memory address
    out[1] = (out[1] + magic2(out[MAGIC_KEY_OFFSET])) & 0xFFFFFFFF

    start_address = out[1]
    block_size    = out[0] # bytes

    # code block start offset
    start_byte = (out[3] & 0x1FF) + 32

    print('addr: 0x%08X; size: %d; pos: 0x%X' % (start_address, block_size, start_byte))

    b = get_bytes_from_uints(out)
    outfile.write(b[start_byte : start_byte + block_size])
    #outfile.write(b)

infile.close()
outfile.close()



