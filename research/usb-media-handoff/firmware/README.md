# Prototype firmware binaries

Built firmware of the USB media handoff prototype for **ZuluSCSI RP2040**
boards (PlatformIO environment `ZuluSCSI_RP2040`), kept here so the tested
build does not depend on the `.pio` build folder.

| File | Use | SHA-256 |
|---|---|---|
| `ZuluSCSI_RP2040_handoff-prototype.bin` | SD card update | `af61cf20d0bf9bcff64c530f5ec2ff42c21459edc4feee977d2a3fbbc09e7cd6` |
| `ZuluSCSI_RP2040_handoff-prototype.uf2` | USB bootloader (BOOTSEL) | `7cbc657344d84a3d8c3ccdd1b900185ab13a683b3212eaf1086be17b20109e5c` |

- Built 2026-11-28 02:12 from the source in the commit that carries these
  files. The firmware reports itself as
  `2026.11.24-release 2026-11-28T02:12:46 git:48fce31-dirty`, because it was
  built before that commit existed (`48fce31` is the commit before it).
- The `.uf2` is the build installed on the test board and used for the
  last tests dated 2026-11-28 in
  [../02-prototype-design.md](../02-prototype-design.md). The `.bin` comes
  from the same build but was not installed.
- History of the builds kept here:
  - `a0a0d24`, tested 2026-11-27: first prototype.
  - `a003760`: services the USB stack on every main-loop pass while USB has
    the media. Reads about 16% faster.
  - `48fce31`: buffers USB writes and writes them to the SD card from the
    main loop. Writes about 27% faster.
  - this build: refuses SCSI commands that need the medium while USB has
    it. **Earlier builds do not**, so they must not be used with a SCSI host
    that might read or write during a handoff.

## Installing

- **SD card**: copy the `.bin` to the root of the card and power the board
  on. The bootloader installs it. This replaces the main firmware and keeps
  the bootloader already on the board.
- **USB**: hold BOOTSEL while plugging the board in, or send `u` then `y` on
  the serial console of a recent firmware, and copy the `.uf2` to the drive
  that appears. This replaces the bootloader as well.

Not for other boards. To go back, install an official release the same way.
