# FrSky S.Port bootloader upload protocol

This describes the protocol spoken by the FrSky receiver bootloader found in
`x4r_orig_bl.bin` (X4R, STM32F1, 8 KB at `0x08000000`) to receive a `.frk`
firmware image, as reverse-engineered from that binary. The binary isn't
included here, but can be dumped from a receiver with `flashdump/`. The
uploader side is implemented in EdgeTX
(`radio/src/io/frsky_firmware_update.cpp`,
`FrskyDeviceFirmwareUpdate::uploadFileNormal()`).

It has been checked against a second dump, the bootloader of an XSR. The two
are builds of the same source: the code is the same at different addresses,
except for the version reply and some code nothing uses. See
[Differences between builds](#differences-between-builds). The XSR was also
run through the whole protocol with `pico/`, which checks every frame against
this document and timestamps it; the timings given here come from there.

The same bootloader source serves the ACCST-generation STM32 devices: X4R,
X8R, XSR, R161 and the XJT modules, among others. ACCESS-generation devices (RX6R
ACCESS, G-RX8, ISRM, Archer X…), and later ACCST ones like the XJT-Lite, use
the same `.frk` scrambling. But they run on TI CC26xx chips (flash at
`0x00000000`, application at `0x00008000` or `0x00009000`), with a different
bootloader. See
[ACCESS (CC26xx) bootloader differences](#access-cc26xx-bootloader-differences)
for what is known about it.

Addresses in brackets (e.g. `[0x08000632]`) point to the code in the X4R
build where the behaviour can be checked.
[Differences between builds](#differences-between-builds) gives the matching
addresses in the XSR build.

## Physical layer

- Single-wire, half-duplex S.Port line, 57600 baud `[0x080012a0]`, 8N1.
- The bootloader drives a transmit-enable pin (PA2, from the config page) high
  while sending and low otherwise `[0x080005bc]`.
- It answers fast. Measured on an XSR, the first byte of a reply ends 183 to
  200 µs after the host's last stop bit. One byte takes 174 µs, so the reply
  starts 9 to 26 µs after the host's frame. A host has to release the line
  right after its last stop bit, or it garbles the start of the reply.
- The bootloader listens on one USART (USART3 in this image, from the config
  page). A second USART can be enabled in the config page, but isn't here.

## Frame format

Every message is a fixed 8-byte S.Port-style frame, preceded by a start byte
and a physical ID byte:

```
0x7E  <phys-id>  <8 payload bytes, byte-stuffed>
```

| Direction        | Physical ID byte | Meaning           |
|------------------|------------------|-------------------|
| host → device    | `0xFF`           | physical ID 0x1F  |
| device → host    | `0x5E`           | physical ID 0x1E  |

The physical ID byte is the usual S.Port encoding (5-bit ID plus 3 check
bits). The bootloader rejects any frame whose ID byte isn't `0xFF`
`[0x08000278]`, `[0x08000740]`.

Payload:

| Byte | Content                                                     |
|------|-------------------------------------------------------------|
| 0    | `0x50` (frame type; the bootloader rejects anything else)   |
| 1    | command (see below)                                         |
| 2..5 | 32-bit argument, little-endian                              |
| 6    | extra byte (sequence check during data transfer, else 0)    |
| 7    | checksum (see below)                                        |

**Byte stuffing.** In the 8 payload bytes, `0x7E` and `0x7D` are sent as
`0x7D` followed by the byte XOR `0x20`. The phys-ID byte is never stuffed.
Receiving `0x7E` in the middle of a payload aborts that frame.

**Checksum.** CRC-16/XMODEM (polynomial `0x1021`, initial value 0, no
reflection, no final XOR) over payload bytes 0..6 `[0x0800023c]`. The two
directions don't use the same half of it:

- host → device: byte 7 = **low** byte of the CRC. The bootloader checks it
  and drops the frame if it doesn't match `[0x08000744]`.
- device → host: byte 7 = **high** byte of the CRC `[0x080005aa]`.

EdgeTX sends the low byte (its `frame[7] = crc16(...)` truncates) and does not
check incoming CRCs, so the asymmetry goes unnoticed there.

## Commands

| Cmd    | Name (EdgeTX)        | Direction     | Argument (bytes 2..5)            | Byte 6               |
|--------|----------------------|---------------|----------------------------------|----------------------|
| `0x00` | `PRIM_REQ_POWERUP`   | host → device | 0                                | 0                    |
| `0x01` | `PRIM_REQ_VERSION`   | host → device | 0                                | 0                    |
| `0x03` | `PRIM_CMD_DOWNLOAD`  | host → device | 0                                | 0                    |
| `0x04` | `PRIM_DATA_WORD`     | host → device | 4 bytes of the image at `offset` | `offset & 0xFF`      |
| `0x05` | `PRIM_DATA_EOF`      | host → device | 0                                | 0                    |
| `0x80` | `PRIM_ACK_POWERUP`   | device → host | 0                                | 0                    |
| `0x81` | `PRIM_ACK_VERSION`   | device → host | see [Version reply](#version-reply) | 0                 |
| `0x82` | `PRIM_REQ_DATA_ADDR` | device → host | `offset` (byte offset in image)  | 0                    |
| `0x83` | `PRIM_END_DOWNLOAD`  | device → host | 0                                | 0                    |
| `0x84` | `PRIM_DATA_CRC_ERR`  | device → host | 0                                | 0                    |

The bootloader can build an `0x84` frame `[0x08000582]`, but nothing in this
image ever sends it.

## Session

```
host                                   bootloader
 |   (device powered on)                   |
 |-- 0x00 REQ_POWERUP -------------------->|  must arrive in the boot window
 |<--------------------- 0x80 ACK_POWERUP -|  sent once
 |-- 0x01 REQ_VERSION -------------------->|
 |<--------------------- 0x81 ACK_VERSION -|  sent once
 |-- 0x03 CMD_DOWNLOAD ------------------->|
 |<---------------- 0x82 REQ_DATA_ADDR(0) -|
 |-- 0x04 DATA_WORD(img[0..3], 0x00) ----->|
 |<---------------- 0x82 REQ_DATA_ADDR(4) -|
 |-- 0x04 DATA_WORD(img[4..7], 0x04) ----->|
 |               ...                       |  every 2048 bytes: decode + flash
 |<------------- 0x82 REQ_DATA_ADDR(len) --|
 |-- 0x05 DATA_EOF ----------------------->|
 |<-------------------- 0x83 END_DOWNLOAD -|
```

### Entering the bootloader `[0x0800151c]`

After reset the bootloader:

1. Makes sure flash readout protection is on, and that the first 4 KB
   (`0x08000000`–`0x08000FFF`) are write-protected through the option bytes.
   If either is missing, it sets it and resets the MCU `[0x0800084c]`. A fresh
   chip therefore resets up to twice before doing anything else.
2. Waits for **one** frame on the S.Port USART. If that frame is a valid
   `REQ_POWERUP` (command 0), it enters bootloader mode. The wait gives up
   after about 400 000 polling iterations without a byte (`[0x08000464]`,
   not measured in real time), or after 22 bytes without a complete frame.
3. Otherwise, if the config page says an application is present
   (`magic == 0x9ABCDEF0` and `app_addr == ~app_addr_inv`), it loads MSP from
   the application's vector table at `app_addr` (`0x08002000`) and jumps to its
   reset vector. Interrupts are still disabled at that point, and VTOR is not
   changed: the application has to do both.
4. If there is no valid application, it enters bootloader mode anyway.

So the host has to power-cycle the device and send `REQ_POWERUP` right away,
repeatedly. EdgeTX turns S.Port power off for 2 s, turns it back on, then sends
`REQ_POWERUP` up to 10 times, 100 ms apart.

Measured on an XSR powered through a switch, with `REQ_POWERUP` every 30 ms
from the moment power is on: the first one never gets an answer, and the
second one, 33 ms after power-on, does (same in all three sessions recorded). So the bootloader isn't
listening yet right at power-on, but is within about 30 ms. That first frame
doesn't spoil the attempt either: the bootloader doesn't count it as its one
frame. How long the window stays open wasn't measured.

### Handshake

In bootloader mode, the bootloader sends `ACK_POWERUP` once. It then ignores
every frame until it gets `REQ_VERSION`, answers with `ACK_VERSION` once, and
ignores every frame until it gets `CMD_DOWNLOAD`. It waits forever in both
states.

Because each ACK is sent only once, a host that misses one can't get it back by
repeating its request: the bootloader has already moved on to the next state.
On an XSR, sending `REQ_POWERUP` again after `ACK_POWERUP`, or `REQ_VERSION`
again after `ACK_VERSION`, indeed gets no reply.

#### Version reply

The contents depend on the bootloader build:

| Bytes | X4R build `[0x08000500]`, `[0x08000516]`                       | XSR build `[0x080001ee]` |
|-------|----------------------------------------------------------------|--------------------------|
| 2..3  | low 16 bits of the word at `0x08001E00` (`0xFFFF` in this dump) | low 16 bits of the word at `0x08001C08` (config page, `0x12345678`) |
| 4..5  | low 16 bits of the word at `0x08002020` (in the application's vector table) | high 16 bits of the same word |

So an XSR answers `78 56 34 12` whatever firmware is installed (checked with
an official v2.1.0 installed, whose word at `0x08002020` is `0x0014`). A host
can't rely on the contents. EdgeTX ignores them. See
[What the bootloader reveals](#what-the-bootloader-reveals) for what the X4R
build's values are in practice.

### Data transfer `[0x08000774]`, `[0x080016e8]`

- The bootloader pulls the image one 32-bit word at a time. It sends
  `REQ_DATA_ADDR` with a byte offset into the image. The offset starts at 0
  and grows by 4 after each accepted word. It is an offset into the `.frk`
  stream, **not** a flash address.
- The host answers with `DATA_WORD`: bytes 2..5 are image bytes
  `[offset .. offset+3]` in file order, byte 6 is `offset & 0xFF`.
- If byte 6 doesn't match, the bootloader sends the same request again. This
  is how a host that missed a request catches up: EdgeTX re-sends its last
  word after 80 ms without a request, the mismatch makes the bootloader repeat
  its current request, and the host answers that one.
- Any other failure while the bootloader waits for a data word **aborts the
  whole download**: a timeout, a bad CRC, a wrong ID or frame type, or a
  command other than 4 or 5. The bootloader then sits in an error LED pattern
  until it is power-cycled.
- Every time the offset reaches a multiple of 2048, the bootloader decodes the
  block it has just received and writes it to flash (see below), then
  requests the next word. Erasing and programming happen before that next
  request goes out, so the host should allow for some latency there (EdgeTX
  waits up to 80 ms, with retries). On an XSR it takes 51.05 to 51.08 ms,
  against about 0.2 ms for any other request.

Measured on an XSR, a 64 KB image takes 62 s: 3.8 ms per word, nearly all of
it the two frames on the line (10 bytes each way at 57600 baud).

### End of transfer

After the last word, the host waits for the next `REQ_DATA_ADDR` and answers
with `DATA_EOF` instead of a data word. The bootloader replies `END_DOWNLOAD`.

- A partially filled 2048-byte block is **never written**. The end-of-file
  path only flashes the remaining buffer if a RAM variable (`0x20001458`) is
  non-zero. That variable is zeroed at startup and nothing ever writes it
  `[0x08001714]`. So the image must be a whole number of 2048-byte blocks;
  any trailing bytes are transferred and thrown away.
- After `END_DOWNLOAD` the bootloader does **not** start the application. It
  sits in a "done" LED pattern until the device is power-cycled.

## Image format (`.frk`)

The image is a sequence of 2048-byte blocks. The bootloader handles each one
on its own. See `unpacker.py` / `packer.py` for the reference implementation
of the scrambling; this section gives the layout after descrambling.

Descrambling `[0x080017d4]`: word 6 is the seed of a 32-bit LCG
(`x' = 0x7FF8A3ED * x + 0x2AA01D31`, with 0 replaced by 1 before each step).
For each of the 512 words `i`, the LCG steps once, then:

- word 6 is left alone,
- words with `i & 0xF == 2` are rotated left by 7 bits, then XORed with the
  LCG output,
- every other word is XORed with the LCG output.

After that, word 1 gets `(0xAA * s) % 0x7673` added to it, where `s` is the
low 16 bits of the seed (0 replaced by 1).

Decoded block:

| Word / bytes | Meaning                                                  |
|--------------|----------------------------------------------------------|
| word 0       | number of bytes to write to flash (at most 1024)         |
| word 1       | flash address to write to                                |
| word 3       | bits 0..8: offset of the data minus 32                   |
| word 6       | seed                                                     |
| `(word3 & 0x1FF) + 32` … `+1024` | data (always 1024 bytes copied)      |

Flash programming `[0x0800087c]`:

- Blocks addressed below `0x08001C00` are silently skipped `[0x0800172c]`.
- The bootloader erases `ceil(size / 1024)` 1 KB pages starting at the
  address, then programs 32-bit words from `address` to `address + size`. The
  address should be page-aligned. A size that isn't a multiple of 4 still
  programs the whole last word.
- `0x08001C00`–`0x08001FFF` is the bootloader's own config page (below). It
  is **not** covered by the write protection and is accepted as a target, so
  a `.frk` that writes there can break the jump to the application or the
  port configuration.

## Config page (`0x08001C00`)

The bootloader code is generic and gets its board-specific settings from this
page:

| Offset | Value in the dumps | Meaning                                           |
|--------|--------------------|---------------------------------------------------|
| `0x00` | `0x08002000`       | application start address (vector table)          |
| `0x04` | `0xF7FFDFFF`       | bitwise complement of the above                   |
| `0x08` | `0x12345678`       | sent as the version reply by the XSR build; not used by the X4R build |
| `0x0C` | `0x9ABCDEF0`       | "application valid" magic                         |
| `0x14` | GPIOB, pin 9       | LED                                               |
| `0x1C` | GPIOB, pin 8       | LED                                               |
| `0x24` | GPIOA, pin 2       | S.Port transmit enable                            |
| `0x3C` | `0x40004800`       | primary USART (USART3)                            |
| `0x40` | `0`                | `0x55` enables a second USART, from `0x6C`        |

The bootloader never changes this page itself, so the "application valid"
check always passes unless a `.frk` overwrites the page. A second record with
the same shape at `0x08001C70` (application at `0x08005800`) isn't referenced
by the code.

The X4R and XSR dumps have byte-identical config pages
(`0x08001C00`–`0x08001FFF`).

## What the bootloader reveals

The bootloader has no command for reading memory. The X4R build gives nothing
away about the installed firmware beyond the two 16-bit values in the version
reply, and the XSR build gives nothing at all:

- `0x80` and `0x83` carry only zeros, `0x82` carries the bootloader's own byte
  counter, and `0x84` is never sent. Nothing reports a checksum or verifies
  what was flashed.
- The version reply's two values are the only flash reads the protocol can
  trigger `[0x08000500]`, `[0x08000516]`. Two other flash read/write helpers
  exist in the X4R build (`0x08001a50`, `0x08001a5c`), but nothing in the
  bootloader calls them. See
  [Differences between builds](#differences-between-builds).

What the two values of the X4R build are in practice:

- **Bytes 2..3** (word at `0x08001E00`): inside the bootloader's 8 KB, and
  never written by the bootloader. It is erased (`0xFFFF`) both in the
  bootloader dump and in a full 64 KB dump of an X4R with firmware installed.
- **Bytes 4..5** (word at `0x08002020`): a constant in the application image.
  Across all the official `.frk`/`.frsk` files for this bootloader that were
  checked:

  | Firmware                                                          | Value    |
  |-------------------------------------------------------------------|----------|
  | X4R, X8R, XSR, R161, FPORT receivers (2017 builds to v2.1.0, EU/FCC/LBT) | `0x0014` |
  | XJT / iXJT modules (2015 builds to v2.1.0)                        | `0x0013` |
  | R9M, R9M 2019, R9 Slim+ OTA                                       | `0x0000` |

  It is a product-class marker. It doesn't change between versions or
  regions, and doesn't tell an X4R from an X8R. A device with the XSR build
  doesn't report it at all.

So the installed firmware can't be identified through the bootloader. Reading
more means flashing code onto the device. `flashdump/` does this, but replaces
the installed firmware. A `.frk` could in principle also rewrite the config
page, pointing the application address at a small dumper that then jumps to
the real firmware. That is risky, though: a power cut while the page is erased
leaves the bootloader reading its UART settings from blank flash, and with
readout protection on, recovery needs a mass erase, which also wipes the
bootloader.

## Differences between builds

The XSR's bootloader (dumped with `flashdump/`) differs from the X4R's in
6636 of its 8192 bytes, but almost all of that is the same code at other
addresses. Comparing the instructions while ignoring addresses, the code at
17 of the 20 X4R addresses cited here is identical in the XSR build:

| X4R          | XSR          | What                                   |
|--------------|--------------|----------------------------------------|
| `0x0800023c` | `0x08000618` | CRC-16                                 |
| `0x08000278` | `0x08000654` | frame receive, ID check                |
| `0x08000464` | `0x08000154` | wait for a byte, with timeout          |
| `0x08000500`, `0x08000516` | `0x080001ee` | version reply (differs, see below) |
| `0x08000582` | `0x0800027a` | `DATA_CRC_ERR` frame                   |
| `0x080005aa` | `0x080002a2` | reply CRC (high byte)                  |
| `0x080005bc` | `0x080002b4` | transmit enable                        |
| `0x08000632` | `0x0800032a` | receive a command                      |
| `0x08000740` | `0x08000438` | ID check                               |
| `0x08000744` | `0x0800043c` | CRC check (low byte)                   |
| `0x08000774` | `0x0800046c` | data transfer                          |
| `0x0800084c` | `0x08001418` | readout and write protection           |
| `0x0800087c` | `0x08001448` | flash programming                      |
| `0x080012a0` | `0x08000ef0` | USART setup, 57600 baud                |
| `0x0800151c` | `0x08001270` | entry (differs, see below)             |
| `0x080016e8` | `0x080014c4` | block handling                         |
| `0x08001714` | `0x080014f0` | partial block flag (`0x20001458` in both, never written) |
| `0x0800172c` | `0x08001508` | blocks below `0x08001C00` skipped      |
| `0x080017d4` | `0x080015b0` | descrambling                           |

The constants are the same too: the 400 000-iteration boot timeout, the
22-byte limit, the LCG constants, the `0x9ABCDEF0` magic, and the flash unlock
keys. The config pages are byte-identical.

What differs:

- **Version reply.** The X4R build sends the low halves of the words at
  `0x08001E00` and `0x08002020`, read through a one-instruction helper at
  `0x080008f8`. The XSR build sends the word at `0x08001C08` (config page
  offset `0x08`), read directly. See [Version reply](#version-reply).
- **A table at `0x08000400`, X4R build only.** It holds two magic words
  (`0x55AAAA55`, `0x5A5AA5A5` at `0x08000438`) and pointers to the read/write
  helpers at `0x08001a50` and `0x08001a5c`: probably an entry-point table for
  the application. The entry code `[0x0800151c]` checks the magic words and
  passes the result to the protection setup `[0x0800084c]`, which ignores it.
  The XSR build has neither the table, the helpers, nor that check. The code
  from `0x080008fc` to `0x08000a22` is in the X4R build only as well; it
  hasn't been analysed.
- The C runtime startup code differs slightly.

## Notes on the EdgeTX implementation

- `.frsk` files start with a 16-byte `FrSkyFirmwareInformation` header. EdgeTX
  reads it and doesn't send it. Offsets requested by the bootloader count
  from the first byte after it. Plain `.frk` files have no header.
- EdgeTX reads the file in 1 KB chunks and answers a request for `offset` from
  `buffer[(offset & 1023) >> 2]`. This only works because the bootloader
  requests offsets strictly in order and only ever repeats the current one.
- Its retries are useful only when the host missed a request (see
  [Data transfer](#data-transfer-0x08000774-0x080016e8)). They can't recover
  from a missed `ACK_POWERUP`/`ACK_VERSION`, or from a corrupted data frame:
  those abort on the bootloader side.

## ACCESS (CC26xx) bootloader differences

This section comes from two dumps, both TI CC2650:

- the 36 KB bootloader of an ISRM-S upgrade module from an X10S, application
  at `0x00009000`. Addresses in brackets refer to this dump, except in
  [XJT-Lite](#xjt-lite);
- the 32 KB bootloader of an XJT-Lite module, application at `0x00008000`,
  dumped with `flashdump/` (`make TARGET=cc26x0`). See [XJT-Lite](#xjt-lite).

The two are builds of the same source. `0x00008000` is the usual application
address. The upgrade ISRM moves it to `0x00009000` so that firmware for a
built-in ISRM can't be flashed onto an upgrade module: its bootloader drops
every block below `0x00009000` (see **Flash targets** below).

It speaks the same protocol (same frames, same commands `0x80`–`0x84`, same
`.frk` scrambling) and has the same overall flow. These are the differences
found:

- **Entry** `[0x00001ba8]`: after reset it listens once for `REQ_POWERUP`. If
  that doesn't arrive, it jumps to the application at `0x00009000`
  unconditionally: there is no "application valid" check.
- **Receiving frames** `[0x00001ea4]`: it checks the `0x50` frame type and the
  checksum, but ignores the physical ID byte.
- **Timeouts** `[0x00002a6c]`: while waiting for a data word, a timeout or a
  bad frame doesn't abort the download. It keeps waiting until it gets
  `DATA_WORD` or `DATA_EOF`.
- **Version reply** `[0x000015fc]`: bytes 2..5 are 4 bytes of the bootloader's
  RAM (`0x20001AF9`–`0x20001AFC`), not flash. Nothing in the bootloader writes
  them directly, and the C startup code doesn't initialize that region. The
  matching ISRM application doesn't reference that address either. So the
  reply most likely carries whatever was left in RAM. A write through a
  computed pointer or from ROM code can't be ruled out statically. Either
  way, the value doesn't come from the installed firmware.
- **Flash targets** `[0x000035c0]`: blocks are written if their address is in
  `0x00009000`–`0x0001EFFF`, or is exactly `0x0001FC00`. That is the last
  flash page, which holds the chip's CCFG configuration block, and it gets
  special handling that hasn't been looked into.

Like the STM32 bootloader, it has no command for reading memory, so it can't
identify the installed firmware either.

### XJT-Lite

The XJT-Lite's bootloader (`0x00000000`–`0x00007FFF`) has the same routines
as the ISRM-S one, mostly at nearby addresses. Addresses in brackets refer to
the XJT-Lite dump. What it adds or does differently:

- **Entry** `[0x00001ba8]`: it first runs the anti-cloning check below
  `[0x00002614]`. Then it listens once for a frame `[0x00001eac]`. Only a
  valid `REQ_POWERUP` keeps it in the bootloader. A timeout, garbage or any
  other command makes it jump to the application `[0x00001c34]`: SP from
  `0x00008000`, PC from `0x00008004`.
- **Anti-cloning check** `[0x00002614]`: the same routine as in the RX6R's
  bootloader. It compares a word at `0x00007FF4` with a value computed from
  the chip's IEEE 802.15.4 address (FCFG1 `0x500012F0`). It also checks a
  flag word at `0x0000003C` in the vector table (`0xFFFFFFFA`). If
  `0x00007FF4` is still erased, it is written. If it doesn't match (a
  bootloader copied to another chip), the first application sector
  (`0x00008000`) is erased. Writing a dumped bootloader back to the same chip
  passes the check.
- **Flash targets** `[0x000036a4]`: `0x00008000`–`0x0001EFFF`, or exactly
  `0x0001FC00`, like the ISRM-S with the lower bound moved.
- **S.Port line**: UART0, TX on DIO29, RX on DIO30. These come from the
  TI-RTOS UART attributes `[0x000053a4]` and pin table `[0x00005400]`; there
  is no config page as on the STM32. On the board, DIO29 feeds an inverting
  3-state buffer (marked "C40R", LVC2G240-like), which drives the S.Port pin
  through 68 Ω. The buffer's output enable has a 10 kΩ pull-up and is driven
  by DIO24. To send a frame `[0x000016f0]`, the bootloader clears the UART's
  RXE, drives DIO24 low `[0x00004db4]`, sends, waits for BUSY to clear, then
  drives DIO24 high `[0x00004da4]`. DIO24 isn't in its pin table, but the
  write takes effect: the replies only reach S.Port while DIO24 is low
  (checked with `flashdump/`).
- **CCFG template** `[0x00007fa8]`: the bootloader image contains a CCFG-like
  block with the CPU DAP enabled (TAP_DAP_0 `0xFFC500C5`). The module's real
  CCFG has every TAP disabled (`0xFF000000`). How this template relates to
  the `0x0001FC00` handling hasn't been looked into.
- **Measured with `pico/`**:
  - `ACK_POWERUP` comes 34–35 ms after power-on.
  - Replies start about 230 µs after the host's frame.
  - The version reply is `0x0000 0x0000`.
  - Writing a block takes 5–18 ms.
  - The bootloader misses a few data words per upload; the uploader resends
    them after its timeout.
