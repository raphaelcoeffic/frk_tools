# flashdump

A tiny bare-metal firmware that reads the receiver's flash, bootloader
included, and sends it out on the S.Port line. That's how to get
`x4r_orig_bl.bin` (the binary `PROTOCOL.md` is based on), which can't be
distributed here.

The bootloader turns on flash readout protection, so an SWD probe can't read
it. Code running from the chip's own flash can, though. So this dumper is
uploaded through the bootloader like any normal firmware, and reads the flash
from there.

**Uploading it replaces the receiver's firmware.** Keep the original `.frk`
around so you can flash it back afterwards. The bootloader itself isn't
touched.

## Build

Needs `arm-none-eabi-gcc` and `python3`, nothing else (no CMSIS, no vendor
libraries).

```
make
```

This produces `flashdump.frk`. By default it dumps the 8 KB bootloader
(`0x08000000`–`0x08001FFF`). To dump another range:

```
make clean all DUMP_START=0x08002000 DUMP_LENGTH=0xE000
```

## Use

With a Raspberry Pi Pico, `../pico/` does steps 1 to 4 below on its own,
power cycles included if the receiver power goes through a switch. See
`../pico/README.md`. By hand:

1. Flash `flashdump.frk` to the receiver like any receiver firmware, e.g.
   with EdgeTX's "Flash S.Port device" on the file from the SD card.
2. Power-cycle the receiver. The bootloader starts the dumper, which sends the
   dump over and over, with about half a second of silence between rounds. If
   the board has its LED configured, the LED toggles once per round.
3. Capture the S.Port line at 57600 baud, 8N1, for a few seconds (one round
   of 8 KB takes about 1.5 s):

   ```
   (stty 57600 raw; cat > capture.bin) < /dev/cu.usbserial-XXXX
   ```

   S.Port is normally an inverted signal (idle low). If you only get garbage,
   use a USB-serial adapter that can invert RX (e.g. an FT232R), or put an
   inverter in front of it.

4. Extract the dump:

   ```
   python3 extract_dump.py capture.bin x4r_orig_bl.bin
   ```

   This looks for every complete round with a valid CRC-32, checks that they
   all agree, and writes the data out.

5. Flash the original receiver firmware back.

## Output format

Each round is:

| Field   | Size     | Content                         |
|---------|----------|---------------------------------|
| magic   | 4        | `FDMP`                          |
| start   | 4 (LE)   | first address dumped            |
| length  | 4 (LE)   | number of bytes                 |
| data    | `length` | flash contents                  |
| crc     | 4 (LE)   | CRC-32 (zlib) of `data`         |

## How it finds the S.Port line

It reads the bootloader's config page at `0x08001C00` (see `PROTOCOL.md`)
and uses the same USART, TX/RX pins and transmit-enable pin as the
bootloader. If that page doesn't look valid, it falls back to the X4R
settings: USART3 on PB10/PB11, transmit enable on PA2.

It also switches the clock back to the internal 8 MHz oscillator, so the baud
rate doesn't depend on how the bootloader left the clocks.
