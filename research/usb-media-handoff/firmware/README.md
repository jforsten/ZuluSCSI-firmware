# Prototype firmware binaries

Built firmware of the USB media handoff prototype for **ZuluSCSI RP2040**
boards (PlatformIO environment `ZuluSCSI_RP2040`), kept here so the tested
build does not depend on the `.pio` build folder.

| File | Use | SHA-256 |
|---|---|---|
| `ZuluSCSI_RP2040_handoff-prototype.bin` | SD card update | `60eadeda2b024a07946023eae4967652253746480cc85d1383ccdb20d5002fa5` |
| `ZuluSCSI_RP2040_handoff-prototype.uf2` | USB bootloader (BOOTSEL) | `4e240055018b5762b2484f8cd68bdf749b070b9287774aeec854d51c8a961498` |

- Built 2026-11-27 22:12 from upstream `24f1578` plus the source changes in
  the commit that added this folder. The firmware reports itself as
  `2026.11.24-release ... git:24f1578-dirty`, because it was built before
  that commit existed.
- The `.bin` is the build that was installed on the test board and used for
  all bench tests recorded in
  [../02-prototype-design.md](../02-prototype-design.md).
  The `.uf2` comes from the same build but was not flashed.

## Installing

- **SD card**: copy the `.bin` to the root of the card and power the board
  on. The bootloader installs it. This replaces the main firmware and keeps
  the bootloader already on the board.
- **USB**: hold BOOTSEL while plugging the board in and copy the `.uf2` to
  the drive that appears. This replaces the bootloader as well.

Not for other boards. To go back, install an official release the same way.
