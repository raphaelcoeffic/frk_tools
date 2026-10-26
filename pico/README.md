# frkpico

A Raspberry Pi Pico used as an S.Port bootloader uploader and line capture
tool. It does two things:

1. Upload a `.frk`/`.frsk` image to a receiver, following `PROTOCOL.md`
   exactly. This is a reference implementation of the host side: it checks
   everything the device sends against `PROTOCOL.md`, and reports what
   differs instead of silently tolerating it. With `--trace` it logs every
   frame on the line, decoded and timestamped to the microsecond. Other
   people can then check the findings in `PROTOCOL.md` against their own
   devices, and other uploaders can be compared against it.
2. Capture the S.Port line, e.g. the output of `flashdump/`, and extract the
   dump.

The Pico runs the whole upload session itself. The host only loads the image
into the Pico beforehand, so the timing on the line doesn't depend on USB
latency. The image is kept in the upper half of the Pico's 2 MB flash, not in
RAM, so it can be up to 512 KB (an SR10+ image is 244 KB).

**Status:** tested with a Waveshare RP2040-Zero, an XSR receiver (`probe`,
`flash`, `dump`) and an Archer Plus SR10+ (`probe`, `flash`, `dump`,
`capture`).

## Wiring

```
Pico                      receiver S.Port connector
GP0 (UART0 TX) ──┬── 1 kΩ ── S   (signal)
GP1 (UART0 RX) ──┘
GND ──────────────────────── −   (ground)
VBUS ──── [power switch] ─── +   (5 V)
GP2 ────── switch control (high = on)
```

- GP0 and GP1 are tied together: S.Port is a single wire. GP0 drives the line
  only while a frame is being sent. GP1 hears everything, including what the
  Pico sends; that echo is checked, which catches collisions and wiring
  problems.
- The signal is inverted (idle low) in the Pico's GPIO block, so no external
  inverter is needed. `--no-invert` is there for a non-inverted line.
- The 1 kΩ resistor limits the current if the device drives the line while
  the Pico does too, or if it drives more than 3.3 V.
- The power switch is optional. Without it, the receiver gets power from
  elsewhere (VBUS through a wire you unplug, a separate supply...) and the
  tool asks you to power-cycle it by hand. With it, `--power` makes the whole
  sequence automatic. See [Power switch](#power-switch).

### Power switch

It has to be on the high side: switching the ground would leave the receiver
finding its way back to ground through the S.Port pin, partly powered, and it
might not reset. With a P-channel MOSFET, driven by a small N-channel one:

```
VBUS ──────────┬────────── S
               │
              10k      IRF9Z34N
               │
               ├────────── G
               │                    D ─── receiver +
               D
GP2 ───────── G   2N7000
    (100k to GND)
               S
               │
GND ───────────┴───────────────────────── receiver −
```

GP2 high turns the 2N7000 on, which pulls the gate to ground: V<sub>GS</sub> is
about −5 V and the IRF9Z34N conducts. Otherwise the 10 kΩ holds the gate at
VBUS and it is off. The optional 100 kΩ keeps it off while the Pico isn't
running. The GPIO can't drive the gate directly: at 3.3 V it would still be
1.7 V below the source, and the MOSFET would stay on.

- The source goes to VBUS and the drain to the receiver. The other way round,
  the body diode powers the receiver all the time.
- TO-220 pinout, facing the label: gate, drain, source. The tab is the drain,
  so it carries the receiver's supply.
- The IRF9Z34N isn't a logic-level part: its R<sub>DS(on)</sub> is only
  specified at −10 V, and its threshold is −2 to −4 V. −5 V is still plenty for
  a receiver's ~100 mA, with a negligible drop. Any other P-channel MOSFET with
  a threshold well under 5 V works the same way (e.g. AO3401).
- Check it with a few hundred ohms as a load before connecting a receiver:
  `./frkpico.py power on` should give about 5 V on the drain, `power off`
  about 0 V.

Without a P-channel MOSFET, a PNP transistor does it for up to about 100 mA:

```
VBUS ───────────┬──────────── E
                │
               10k        BC558 (PNP)
                │
                ├──────────── B
                │                      C ─── receiver +
               470Ω
                │
                D
GP2 ────────── G   2N7000
     (100k to GND)
                S
                │
GND ────────────┴──────────────────────────── receiver −
```

The 2N7000 pulls about 9 mA out of the base, enough to saturate the BC558 up
to roughly 100 mA, with 0.2–0.3 V lost. That's the BC558's rating, so measure
the receiver's current first.

Pins and the UART instance can be changed at build time, e.g.
`-DSPORT_UART=1 -DSPORT_TX_PIN=4 -DSPORT_RX_PIN=5 -DPOWER_PIN=6`.

## Build and install

```
cmake -B build -G Ninja -DPICO_SDK_PATH=~/Src/pico-sdk
cmake --build build
```

Copy `build/frkpico.uf2` to the Pico in BOOTSEL mode, or
`picotool load -fx build/frkpico.uf2`. For another board, pass `PICO_BOARD`,
e.g. `-DPICO_BOARD=pico2` or `-DPICO_BOARD=waveshare_rp2040_zero`.

The host tool needs [uv](https://docs.astral.sh/uv/): it declares its own
dependency (pyserial) and `uv` takes care of it.

## Use

Checking the wiring and the handshake first, without touching the receiver's
firmware:

```
./frkpico.py --trace probe.log probe --power
```

`probe` enters the bootloader and does the handshake, but never sends
`CMD_DOWNLOAD`: nothing is erased or written. It reports when `ACK_POWERUP`
came and the two `ACK_VERSION` values. It also sends `REQ_POWERUP` and
`REQ_VERSION` once more after their ACKs, to check that the bootloader
ignores them as `PROTOCOL.md` says. The bootloader is then left waiting for
`CMD_DOWNLOAD`: power-cycle the receiver to start its firmware again.

Dumping the bootloader of a receiver, with the power switch:

```
./frkpico.py flash --power ../flashdump/flashdump.frk
./frkpico.py dump --power x4r_orig_bl.bin
./frkpico.py flash --power original_firmware.frk
```

Without `--power`, each command asks you to power-cycle the receiver. For
`flash`, the Pico keeps sending `REQ_POWERUP` until the bootloader answers
(60 s by default, `--wait`), so the power cycle can happen any time. Ctrl-C
gives up.

- `probe`: handshake only, see above. Takes the same options as `flash`.
- `flash FILE`: uploads the image. A `.frsk` header is skipped. With
  `--strict`, the command fails if the device deviated from `PROTOCOL.md` in
  any way, even if the upload went through.
- `dump OUT`: power-cycles the receiver (the dumper only starts after a power
  cycle), captures until two identical rounds with a valid CRC are in
  (`--rounds`), and writes the dump. `--raw FILE` also saves the capture,
  which `../flashdump/extract_dump.py` can read.
- `capture OUT`: raw capture for `--seconds`.
- `power on|off`.

`--trace FILE` (before the command) writes the frame log:

```
   0.000000  +0.000000 host>dev 7E FF 50 00 00 00 00 00 00 97            REQ_POWERUP   arg=00000000 x=00 crc=low
   0.000512  +0.000512 dev>host 7E 5E 50 80 00 00 00 00 00 3E            ACK_POWERUP   arg=00000000 x=00 crc=high
```

Columns: time since the first frame, time since the previous frame, direction,
raw bytes as sent on the wire (stuffing included), decoded payload. `crc=`
says which half of the CRC-16 the frame carries. Sent frames are timestamped
at the end of their last byte, received ones at the end of their first byte.
Lines starting with `#` are the Pico's messages.

## What is checked

Against `PROTOCOL.md`, on every frame from the device:

- physical ID `0x5E`, frame type `0x50`;
- CRC: high byte of the CRC-16/XMODEM. A frame carrying the low byte is
  accepted with a warning; one matching neither is dropped;
- byte 6 is 0, and the argument is 0 except in `ACK_VERSION` and
  `REQ_DATA_ADDR`;
- offsets requested in order, 4 bytes at a time, starting at 0, within the
  image; a repeated or out-of-order request is reported;
- no `DATA_CRC_ERR`, no `END_DOWNLOAD` before `DATA_EOF`.

On the line: the echo of every frame sent matches what was sent, and no
device bytes arrived when the Pico started sending (both would mean a
collision). UART framing errors are counted.

It also reports measurements that `PROTOCOL.md` doesn't pin down: how long
after power-on `ACK_POWERUP` comes, how long the bootloader takes to answer a
data word, and how long it takes to flash a block (`block_us` in the final
stats, and the `P` lines).

## How the session is run

What the Pico does on top of the bare protocol, and why:

- `REQ_POWERUP` every 30 ms, leaving 28 ms of silence for the ACK. Nothing is
  sent while the receiver is off (with `--power`), so the line doesn't feed
  it through its signal pin.
- `REQ_VERSION` is retried up to 5 times: once the bootloader has answered
  it, it ignores further ones. If no `ACK_VERSION` ever comes, it goes on
  with `CMD_DOWNLOAD` anyway, with a warning.
- If `CMD_DOWNLOAD` gets no answer, the Pico alternates `DATA_WORD(0)` and
  `CMD_DOWNLOAD`. A second `CMD_DOWNLOAD` would abort the download if only
  the `REQ_DATA_ADDR(0)` got lost, while `DATA_WORD` is ignored by a
  bootloader still waiting for `CMD_DOWNLOAD`.
- During the transfer, a missing request is waited for 50 ms, or 500 ms after
  the last word of a block (the bootloader flashes before asking for more),
  then the last frame is sent again. Waiting that long after a block avoids
  sending while the bootloader is about to answer.
- Requests are answered from the whole image in RAM, by offset, so any
  request order works (and is reported).

## USB protocol

Text lines over the USB serial port, for use from a terminal if needed. Each
command ends with a line `OK ...` or `ERR <reason>`.

| Command                  | Effect                                                    |
|--------------------------|-----------------------------------------------------------|
| `ping`                   | `OK frkpico <version>`                                    |
| `load <len>`             | starts an image of `len` bytes (multiple of 4)            |
| `data <offset> <hex>`    | stores bytes of the image                                 |
| `crc`                    | `OK <crc32> <len>` of the loaded image                    |
| `invert 0\|1`            | line polarity (default 1: idle low)                       |
| `trace 0\|1`             | report every frame as `T <us> <S\|R\|A> <hex>`            |
| `power 0\|1`             | receiver power switch                                     |
| `flash <wait_ms> <off_ms>` | runs the session; with `off_ms` > 0, power-cycles first |
| `probe <wait_ms> <off_ms>` | the same, but stops after the handshake           |
| `capture`                | streams `D <hex>` lines until anything is received        |

During `flash` and `probe`: `I` information lines, `W` warnings, and `P <offset> <len>
<us>` at the start of each block. Any input while waiting for the bootloader
cancels.
