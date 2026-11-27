# Prototype firmware binaries

Built firmware of the USB media handoff prototype for **ZuluSCSI RP2040**
boards (PlatformIO environment `ZuluSCSI_RP2040`), kept here so the tested
build does not depend on the `.pio` build folder.

| File | Use | SHA-256 |
|---|---|---|
| `ZuluSCSI_RP2040_handoff-prototype.bin` | SD card update | `fa1ccc0f94e7c1227aaa1d6e130465fd11443e03761f9d5c73f3119c292e1252` |
| `ZuluSCSI_RP2040_handoff-prototype.uf2` | USB bootloader (BOOTSEL) | `9296916fff4dc87009719264d4b090e05941f9915652de00c5507065eb19048b` |

- Built 2026-11-28 01:30 from the source in the commit that carries these
  files. The firmware reports itself as
  `2026.11.24-release 2026-11-28T01:30:09 git:f6511b8-dirty`, because it was
  built before that commit existed (`f6511b8` is the commit before it).
- The `.uf2` is the build installed on the test board and used for the
  write-buffer tests dated 2026-11-28 in
  [../02-prototype-design.md](../02-prototype-design.md). The `.bin` comes
  from the same build but was not installed.
- History of the builds kept here:
  - `a0a0d24`, tested 2026-11-27: first prototype.
  - `a003760`: services the USB stack on every main-loop pass while USB has
    the media. Reads about 16% faster.
  - this build: buffers USB writes and writes them to the SD card from the
    main loop. Writes about 27% faster.

## Installing

- **SD card**: copy the `.bin` to the root of the card and power the board
  on. The bootloader installs it. This replaces the main firmware and keeps
  the bootloader already on the board.
- **USB**: hold BOOTSEL while plugging the board in, or send `u` then `y` on
  the serial console of a recent firmware, and copy the `.uf2` to the drive
  that appears. This replaces the bootloader as well.

Not for other boards. To go back, install an official release the same way.
