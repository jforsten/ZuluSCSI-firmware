# Prototype firmware binaries

Built firmware of the USB media handoff prototype for **ZuluSCSI RP2040**
boards (PlatformIO environment `ZuluSCSI_RP2040`), kept here so the tested
build does not depend on the `.pio` build folder.

| File | Use | SHA-256 |
|---|---|---|
| `ZuluSCSI_RP2040_handoff-prototype.bin` | SD card update | `80602fed3704184b81372d3ac252537396d9116d62660d1407efb71553f4b7da` |
| `ZuluSCSI_RP2040_handoff-prototype.uf2` | USB bootloader (BOOTSEL) | `0845e9019be8928e87fb9a3cfc62069c99c93ecd3e0f4ad18317768e976b543d` |

- Built 2026-11-28 00:58 from the source in the commit that carries these
  files. The firmware reports itself as
  `2026.11.24-release 2026-11-28T00:58:13 git:a0a0d24-dirty`, because it was
  built before that commit existed (`a0a0d24` is the commit before it).
- The `.uf2` is the build installed on the test board and used for the
  tests dated 2026-11-28 in
  [../02-prototype-design.md](../02-prototype-design.md). The `.bin` comes
  from the same build but was not installed.
- Compared with the first prototype build (commit `a0a0d24`, tested
  2026-11-27), this one services the USB stack on every main-loop pass while
  USB has the media, which makes transfers faster.

## Installing

- **SD card**: copy the `.bin` to the root of the card and power the board
  on. The bootloader installs it. This replaces the main firmware and keeps
  the bootloader already on the board.
- **USB**: hold BOOTSEL while plugging the board in, or send `u` then `y` on
  the serial console of a recent firmware, and copy the `.uf2` to the drive
  that appears. This replaces the bootloader as well.

Not for other boards. To go back, install an official release the same way.
