# frk_tools

Tools for FrSky receiver firmware images (`.frk`, `.frsk`) and the S.Port
bootloader that flashes them.

- `unpacker.py`: descrambles an image into a raw binary.

  ```
  python3 unpacker.py firmware.frsk firmware.bin
  ```

  It prints the flash address of each 1 KB block. `.frsk` files start with
  a 16-byte `FRSK` header, which is skipped and whose fields are printed.
- `packer.py`: scrambles a raw binary into a `.frk`, given the flash address
  it starts at (the application start, e.g. `0x08002000` on the STM32F1
  receivers).

  ```
  python3 packer.py 0x08002000 firmware.bin firmware.frk
  ```

  It writes no `FRSK` header.
- `PROTOCOL.md`: the S.Port bootloader protocol and the image format.
- `flashdump/`: a firmware that dumps the receiver's flash, bootloader
  included, over S.Port.
- `pico/`: a Raspberry Pi Pico S.Port uploader and line capture tool.

Firmware images aren't included: FrSky's aren't ours to distribute.
