#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.9"
# dependencies = ["pyserial>=3.5"]
# ///
#
# Host side of the Pico S.Port uploader (see README.md).
#
#   frkpico.py probe           bootloader handshake only, nothing written
#   frkpico.py flash FILE      upload a .frk/.frsk through the bootloader
#   frkpico.py dump OUT        capture the flashdump/ output and extract it
#   frkpico.py capture OUT     raw capture of the S.Port line
#   frkpico.py power on|off    switch the receiver power
#
# Lines from the Pico:
#   OK ... / ERR ...   end of a command
#   I ...              information
#   W ...              the device did something PROTOCOL.md doesn't describe
#   P off len us       progress, at the start of each 2048-byte block
#   T us dir hex       traced frame (S sent, R received, A aborted) or
#                      bytes outside a frame (J), with a UART error (E)
#   D hex              captured bytes

import os
import sys
import time
import zlib
import argparse

import serial
import serial.tools.list_ports

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'flashdump'))
from extract_dump import find_rounds

PICO_VID = 0x2E8A
BLOCK_SIZE = 2048
DATA_CHUNK = 256

# .frsk files start with a 16-byte FrSkyFirmwareInformation header
FRSK_MAGIC = b'FRSK'
FRSK_HEADER_SIZE = 16

PRIM_NAMES = {
    0x00: 'REQ_POWERUP',
    0x01: 'REQ_VERSION',
    0x03: 'CMD_DOWNLOAD',
    0x04: 'DATA_WORD',
    0x05: 'DATA_EOF',
    0x80: 'ACK_POWERUP',
    0x81: 'ACK_VERSION',
    0x82: 'REQ_DATA_ADDR',
    0x83: 'END_DOWNLOAD',
    0x84: 'DATA_CRC_ERR',
}

DIR_NAMES = {'S': 'host>dev', 'R': 'dev>host', 'A': 'aborted ', 'J': 'stray   ', 'E': 'rx error'}

class PicoError(Exception):
    pass

def crc16_xmodem(data):
    crc = 0
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021 if crc & 0x8000 else crc << 1) & 0xFFFF
    return crc

def decode_frame(raw):
    payload = bytearray()
    esc = False
    for b in raw[2:]:
        if esc:
            payload.append(b ^ 0x20)
            esc = False
        elif b == 0x7D:
            esc = True
        else:
            payload.append(b)

    if len(payload) < 8:
        return 'incomplete (%d payload bytes)' % len(payload)

    crc = crc16_xmodem(payload[:7])
    if payload[7] == crc >> 8:
        crc_half = 'high'
    elif payload[7] == crc & 0xFF:
        crc_half = 'low'
    else:
        crc_half = 'BAD'

    name = PRIM_NAMES.get(payload[1], 'cmd 0x%02X' % payload[1])
    arg = int.from_bytes(payload[2:6], 'little')
    text = '%-13s arg=%08X x=%02X crc=%s' % (name, arg, payload[6], crc_half)
    if raw[1] not in (0xFF, 0x5E):
        text += ' id=%02X' % raw[1]
    if payload[0] != 0x50:
        text += ' type=%02X' % payload[0]
    return text

class TraceLog:
    """Writes the traced frames, decoded, along with the Pico's messages."""

    def __init__(self, f):
        self.f = f
        self.first = None
        self.prev = None
        self.elapsed = 0

    def frame(self, t, direction, raw):
        if self.first is None:
            self.first = self.prev = t
        # the Pico's microsecond counter wraps every 71 minutes
        delta = (t - self.prev) & 0xFFFFFFFF
        self.elapsed += delta
        self.prev = t
        self.f.write('%11.6f %+10.6f %s %-40s %s\n' % (
            self.elapsed / 1e6, delta / 1e6, DIR_NAMES.get(direction, direction),
            raw.hex(' ').upper(), decode_frame(raw) if direction in 'SRA' else ''))

    def message(self, line):
        self.f.write('# %s\n' % line)

class Pico:
    def __init__(self, port, trace=None):
        # the baud rate doesn't matter on USB, except 1200 which reboots the
        # Pico into BOOTSEL
        self.ser = serial.Serial(port, 115200, timeout=0.1)
        self.trace = trace
        self.warnings = []
        self.sync()

    def readline(self, timeout):
        deadline = time.monotonic() + timeout
        buf = b''
        while time.monotonic() < deadline:
            buf += self.ser.readline()
            if buf.endswith(b'\n'):
                return buf.decode('ascii', 'replace').strip()
        return None

    def sync(self):
        # stops a capture or a wait for the bootloader left over from before
        self.ser.write(b'\n')
        time.sleep(0.2)
        self.ser.reset_input_buffer()
        reply = self.command('ping')
        if not reply.startswith('frkpico'):
            raise PicoError('unexpected reply to ping: %r' % reply)

    def handle(self, line):
        kind, _, rest = line.partition(' ')
        if kind == 'T':
            t, direction, raw = rest.split()
            if self.trace:
                self.trace.frame(int(t), direction, bytes.fromhex(raw))
            return
        if self.trace:
            self.trace.message(line)
        if kind == 'I':
            print(rest, file=sys.stderr)
        elif kind == 'W':
            self.warnings.append(rest)
            print('warning:', rest, file=sys.stderr)
        elif kind == 'P':
            off, length, latency = (int(x) for x in rest.split())
            print('  %6d / %d bytes (%3d%%), %6.1f ms since the last word'
                  % (off, length, 100 * off // length, latency / 1000), file=sys.stderr)

    def command(self, cmd, timeout=2.0, handler=None):
        """Sends a command and returns what follows OK. Raises PicoError on
        ERR. The timeout applies between two lines from the Pico."""
        self.ser.write(cmd.encode('ascii') + b'\n')
        try:
            while True:
                line = self.readline(timeout)
                if line is None:
                    raise PicoError('no reply to %r' % cmd.split()[0])
                if line == 'OK' or line.startswith('OK '):
                    return line[3:]
                if line.startswith('ERR'):
                    raise PicoError(line[4:])
                if not (handler and handler(line)):
                    self.handle(line)
        except KeyboardInterrupt:
            # cancels a wait for the bootloader or a capture
            self.ser.write(b'\n')
            raise

    def load(self, data):
        self.command('load %d' % len(data))
        for off in range(0, len(data), DATA_CHUNK):
            self.command('data %d %s' % (off, data[off:off + DATA_CHUNK].hex()))
        crc = int(self.command('crc').split()[0], 16)
        if crc != zlib.crc32(data):
            raise PicoError('image CRC mismatch after loading: %08X != %08X'
                            % (crc, zlib.crc32(data)))

    def capture(self, on_data, timeout):
        """Streams captured bytes to on_data() until it returns True or the
        timeout expires. Returns the Pico's error counters."""
        deadline = time.monotonic() + timeout
        stopping = False

        def handler(line):
            nonlocal stopping
            if not line.startswith('D '):
                return False
            if not stopping and (on_data(bytes.fromhex(line[2:])) or time.monotonic() > deadline):
                self.ser.write(b'\n')
                stopping = True
            return True

        self.ser.write(b'capture\n')
        # nothing comes while the line is silent: poll the deadline here too
        while True:
            line = self.readline(0.5)
            if line is None:
                if not stopping and time.monotonic() > deadline:
                    self.ser.write(b'\n')
                    stopping = True
                continue
            if line.startswith('OK '):
                return dict(kv.split('=') for kv in line[3:].split())
            if line.startswith('ERR'):
                raise PicoError(line[4:])
            if not handler(line):
                self.handle(line)

def find_port():
    ports = [p.device for p in serial.tools.list_ports.comports() if p.vid == PICO_VID]
    if not ports:
        sys.exit('error: no Pico found, use --port')
    if len(ports) > 1:
        sys.exit('error: several Picos found (%s), use --port' % ', '.join(ports))
    return ports[0]

def read_image(path):
    with open(path, 'rb') as f:
        data = f.read()

    if data[:4] == FRSK_MAGIC:
        print('%s: skipping the %d-byte .frsk header' % (path, FRSK_HEADER_SIZE), file=sys.stderr)
        data = data[FRSK_HEADER_SIZE:]

    if len(data) % BLOCK_SIZE:
        print('warning: %s is %d bytes, not a multiple of %d: the bootloader drops the last %d'
              % (path, len(data), BLOCK_SIZE, len(data) % BLOCK_SIZE), file=sys.stderr)
    if len(data) % 4:
        data += b'\xff' * (4 - len(data) % 4)
    return data

def power_cycle(pico, args):
    if args.power:
        pico.command('power 0')
        time.sleep(args.off_time)
        pico.command('power 1')
    else:
        print('Power-cycle the receiver now.', file=sys.stderr)

def run_session(pico, args, verb):
    """Runs 'flash' or 'probe' on the Pico, power-cycling first with --power."""
    start = time.monotonic()
    if args.power:
        off_ms = int(args.off_time * 1000)
        for attempt in range(1, args.attempts + 1):
            try:
                pico.command('%s %d %d' % (verb, args.boot_wait * 1000, off_ms),
                             timeout=args.boot_wait + args.off_time + 5)
                break
            except PicoError as e:
                if str(e) != 'no ACK_POWERUP' or attempt == args.attempts:
                    raise
                print('no ACK_POWERUP, power-cycling again (attempt %d)' % (attempt + 1),
                      file=sys.stderr)
    else:
        print('Power-cycle the receiver now (Ctrl-C to give up).', file=sys.stderr)
        pico.command('%s %d 0' % (verb, args.wait * 1000), timeout=args.wait + 5)

    print('done in %.1f s, %d warning(s)' % (time.monotonic() - start, len(pico.warnings)),
          file=sys.stderr)
    if pico.warnings and args.strict:
        sys.exit(1)

def cmd_flash(pico, args):
    data = read_image(args.file)
    print('loading %d bytes into the Pico' % len(data), file=sys.stderr)
    pico.load(data)
    run_session(pico, args, 'flash')

def cmd_probe(pico, args):
    run_session(pico, args, 'probe')

def cmd_dump(pico, args):
    capture = bytearray()
    last_check = 0
    result = None

    def on_data(data):
        nonlocal last_check, result
        capture.extend(data)
        now = time.monotonic()
        if now - last_check < 0.25:
            return False
        last_check = now

        rounds = find_rounds(bytes(capture))
        print('\r  %d bytes captured, %d valid round(s)' % (len(capture), len(rounds)),
              end='', file=sys.stderr)
        if rounds and any((s, d) != (rounds[0][1], rounds[0][2]) for _, s, d in rounds[1:]):
            result = 'differ'
            return True
        if len(rounds) >= args.rounds:
            result = rounds[0]
            return True
        return False

    power_cycle(pico, args)
    counters = pico.capture(on_data, args.timeout)
    print(file=sys.stderr)

    if args.raw:
        with open(args.raw, 'wb') as f:
            f.write(capture)

    if result is None:
        # the last chunk may have completed a round
        rounds = find_rounds(bytes(capture))
        if len(rounds) >= args.rounds:
            result = rounds[0]

    print('line: %s' % ', '.join('%s=%s' % kv for kv in counters.items()), file=sys.stderr)
    if result == 'differ':
        sys.exit('error: rounds with a valid CRC differ')
    if result is None:
        if not capture:
            hint = 'nothing received: check the wiring, and that the dumper is flashed'
        elif int(counters.get('fe', 0)) + int(counters.get('be', 0)) > len(capture) // 10:
            hint = 'many framing errors: try --no-invert'
        else:
            hint = 'not enough complete rounds, try a longer --timeout'
        sys.exit('error: %s' % hint)

    _, start, data = result
    with open(args.outfile, 'wb') as f:
        f.write(data)
    print('0x%08X..0x%08X: %d bytes written to %s'
          % (start, start + len(data), len(data), args.outfile), file=sys.stderr)

def cmd_capture(pico, args):
    capture = bytearray()

    power_cycle(pico, args)
    counters = pico.capture(lambda data: capture.extend(data), args.seconds)
    with open(args.outfile, 'wb') as f:
        f.write(capture)
    print('%d bytes written to %s (%s)'
          % (len(capture), args.outfile, ', '.join('%s=%s' % kv for kv in counters.items())),
          file=sys.stderr)

def cmd_power(pico, args):
    pico.command('power %d' % (args.state == 'on'))

def parseArgs():
    parser = argparse.ArgumentParser(description='S.Port bootloader uploader using a Raspberry Pi Pico')
    parser.add_argument('-p', '--port', help='serial port of the Pico (default: auto-detect)')
    parser.add_argument('--no-invert', action='store_true',
                        help='non-inverted S.Port signal (idle high)')
    parser.add_argument('--trace', metavar='FILE',
                        help='write every frame on the line, decoded and timestamped, to FILE')
    sub = parser.add_subparsers(dest='command', required=True)

    def power_args(p):
        p.add_argument('--power', action='store_true',
                       help='power-cycle the receiver through the switch on the power pin')
        p.add_argument('--off-time', type=float, default=2.0, metavar='S',
                       help='how long to keep the receiver off (default: 2 s)')

    def session_args(p):
        power_args(p)
        p.add_argument('--attempts', type=int, default=3,
                       help='power cycles to try with --power (default: 3)')
        p.add_argument('--boot-wait', type=int, default=2, metavar='S',
                       help='time to get ACK_POWERUP after power-on with --power (default: 2 s)')
        p.add_argument('--wait', type=int, default=60, metavar='S',
                       help='time to get ACK_POWERUP without --power (default: 60 s)')
        p.add_argument('--strict', action='store_true',
                       help='exit with an error if the device deviated from PROTOCOL.md')

    p = sub.add_parser('flash', help='upload a firmware image through the bootloader')
    p.add_argument('file', help='.frk or .frsk image')
    session_args(p)
    p.set_defaults(func=cmd_flash)

    p = sub.add_parser('probe', help='bootloader handshake only, nothing is written')
    session_args(p)
    p.set_defaults(func=cmd_probe)

    p = sub.add_parser('dump', help='capture the flashdump output and extract the dump')
    p.add_argument('outfile', help='where to write the dumped flash')
    power_args(p)
    p.add_argument('--rounds', type=int, default=2,
                   help='identical valid rounds required (default: 2)')
    p.add_argument('--raw', metavar='FILE', help='also save the raw capture')
    p.add_argument('--timeout', type=float, default=120, metavar='S',
                   help='give up after this long (default: 120 s)')
    p.set_defaults(func=cmd_dump)

    p = sub.add_parser('capture', help='raw capture of the S.Port line')
    p.add_argument('outfile')
    power_args(p)
    p.add_argument('--seconds', type=float, default=10, help='capture duration (default: 10 s)')
    p.set_defaults(func=cmd_capture)

    p = sub.add_parser('power', help='switch the receiver power')
    p.add_argument('state', choices=['on', 'off'])
    p.set_defaults(func=cmd_power)

    return parser.parse_args()

def main():
    args = parseArgs()
    trace_file = open(args.trace, 'w') if args.trace else None

    try:
        pico = Pico(args.port or find_port(), TraceLog(trace_file) if trace_file else None)
        pico.command('invert %d' % (not args.no_invert))
        pico.command('trace %d' % bool(trace_file))
        args.func(pico, args)
    except PicoError as e:
        sys.exit('error: %s' % e)
    except KeyboardInterrupt:
        sys.exit('interrupted')
    finally:
        if trace_file:
            trace_file.close()

if __name__ == '__main__':
    main()
