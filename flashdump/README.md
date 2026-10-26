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

## CC26x0 (CC2650) devices

`main_cc26x0.c` does the same for the TI CC2650 devices running FrSky's
CC26xx bootloader with the application at `0x00008000`, e.g. the XJT-Lite:

```
make TARGET=cc26x0
```

This produces `flashdump_cc26x0.frk`, which dumps the whole 128 KB flash:
the 32 KB bootloader, whatever is left of the application, and the last page
with the CCFG. Reading it from code works even with the debug port locked.
Nothing is written. One round takes about 23 s.

These bootloaders have no config page: the pins are compiled into TI-RTOS
board tables. So they are build options:

| Option      | Default | Meaning                                         |
|-------------|---------|-------------------------------------------------|
| `TX`        | 29      | DIO driven by UART0 TX                          |
| `TX2`       | -1      | second DIO with the same output, -1 for none    |
| `TX_ENABLE` | 24      | DIO held low while sending, -1 for none         |

- **XJT-Lite** (defaults, tested): the bootloader's UART is TX on DIO29, RX
  on DIO30. DIO29 goes through an inverting 3-state buffer (marked "C40R",
  LVC2G240-like) and 68 ohms to the S.Port pin. The buffer's output enable
  has a 10k pull-up and is driven by DIO24: low while sending, as the
  bootloader does. The dump then comes out on S.Port like the bootloader's
  replies (default polarity of `pico/`). With `TX2=25` it also comes out on
  the "TX" test pad, inverted too.
- **RX6R**: `make TARGET=cc26x0 TX=25`, from its bootloader. Not tested with
  this dumper.

The application area is erased only where the dumper is written (the first
4 KB sector), so the rest of the installed firmware is still in the dump.

## EFR32xG22 devices

`main_efr32xg22.c` does the same for the Silicon Labs EFR32xG22 devices
running FrSky's bootloader with the application at `0x00008000`, e.g. the
Archer Plus SR10+:

```
make TARGET=efr32xg22
```

This produces `flashdump_efr32xg22.frk`, which dumps the whole flash, its
size read from DEVINFO (512 KB on the SR10+): the 32 KB bootloader, whatever
is left of the application, and any data pages. One round takes about 91 s,
so give `pico/`'s `dump` a longer `--timeout` (e.g. 300).

The pins are build options, as port × 16 + pin:

| Option      | Default        | Meaning                                  |
|-------------|----------------|------------------------------------------|
| `TX`        | `0x10` (PB00)  | pin driven by USART0 TX                  |
| `TX_ENABLE` | `0x14` (PB04)  | pin held low while sending, -1 for none  |

- **SR10+** (defaults, tested): taken from the 1.0.10 application, which
  sends on USART0 (TX PB00, RX PB01) with PB04 low while sending. The USART
  doesn't invert: the line driver outside the chip does. The dump comes out
  on S.Port with `pico/`'s default polarity.

Nothing about the state the bootloader leaves is assumed: the dumper resets
the stack and its limits, turns off the MPU, interrupts and DMA, unlocks the
clock and GPIO registers, runs from the HFRCO's 19 MHz band with its factory
calibration, and feeds the watchdog if it runs.

The application area is erased only where the dumper is written (the first
8 KB page), so the rest of the installed firmware is still in the dump.
