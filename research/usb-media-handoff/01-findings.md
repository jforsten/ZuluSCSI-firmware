# USB access to a disk image while SCSI emulation runs: findings

Date: 2026-11-27. Code base: ZuluSCSI-firmware `main` at `24f1578`.
Target hardware for the prototype: ZuluSCSI Mini RP2040 (build `ZuluSCSI_RP2040`).

## Question

The [USB Mass Storage wiki page](https://github.com/ZuluSCSI/ZuluSCSI-firmware/wiki/USB-Mass-Storage)
says that ZuluSCSI does not act as a SCSI emulator while it is a USB drive, and
that editing the SD card live "is not supported, because of a high risk of
filesystem corruption". Can that limitation be removed, so that a PC can edit
the image a SCSI host (an Ensoniq sampler) is using, without pulling the card?

## Answer in short

- Fully simultaneous access is possible but not worth it. The storage level can
  be made safe, but the SCSI host caches what it has read and has no way to
  learn that the image changed underneath it.
- Exposing the whole SD card (its FAT file system) while emulation runs is not
  safe and should not be attempted.
- A **media handoff** is the design that works: the image is emulated as a
  removable drive (Iomega Zip), and the "disk" is in exactly one place at a
  time. Either the SCSI host has it, or the USB host has it. Moving it uses the
  eject/insert logic the firmware already has, so the SCSI host sees an
  ordinary media change.

The prototype design is in [02-prototype-design.md](02-prototype-design.md).

## Why it does not work today

1. **Card reader mode is a blocking loop at boot.** `zuluscsi_setup()` calls
   `zuluscsi_msc_loop()` before the SCSI main loop starts
   (`src/ZuluSCSI.cpp`, the `PLATFORM_MASS_STORAGE` block). The USB storage
   interface is registered only inside that loop
   (`platform_enter_msc()` in `lib/ZuluSCSI_platform_RP2MCU/ZuluSCSI_platform_msc.cpp`).

2. **USB callbacks can run in the middle of an SD card transaction.** On the
   RP2 boards the arduino-pico core runs `tud_task()` from two places: a 1 kHz
   timer interrupt (`USBClass::usbIRQ`, skipped while `USB.mutex` is held) and
   every `Serial` call made from the main loop. TinyUSB invokes the storage
   callbacks (`tud_msc_read10_cb`, `tud_msc_write10_cb`) synchronously inside
   `tud_task()`, and those callbacks do SD I/O directly. During emulation the
   interrupt could therefore start an SD command while the SCSI code is halfway
   through another one.

3. **A callback cannot say "busy, try again later".** The bundled TinyUSB is
   0.18.0 (arduino-pico fork `v5.4.2-DaynaPORT` -> pico-sdk `5bf7ffd` ->
   tinyusb `86ad6e5`). When `tud_msc_read10_cb` returns 0, TinyUSB re-queues
   the event, and `tud_task_ext()` loops until the queue is empty (the early
   return is compiled out for `OPT_OS_PICO`). Returning 0 from interrupt
   context would spin forever. TinyUSB 0.18 has no asynchronous storage API.
   This was read from the source, not yet confirmed by experiment.

4. **USB and SCSI share one file object.** In image mode the USB callbacks call
   `file.seek()` on the same `ImageBackingStore` the SCSI read path uses, so
   they would move the position under a transfer in progress.

5. **Global SD stream callback.** `platform_set_sd_callback()` installs a
   single global callback that the SD driver calls while a transfer runs, so
   the SCSI bus can be fed at the same time. An SD operation started by USB
   would call the SCSI code's callback from the wrong context
   (`SdioCard::stopTransmission()` calls it unconditionally while waiting).

## How SCSI2SD V6 does it

ZuluSCSI inherits the SCSI command layer from SCSI2SD V6 (`lib/SCSI2SD`), and
SCSI2SD V6 does offer USB storage during emulation. Its approach
([jwakely/SCSI2SD-V6](https://github.com/jwakely/SCSI2SD-V6), files
`src/firmware/main.c`, `usb_device/usbd_composite.c`,
`usb_device/usbd_msc_storage_sd.c`):

- USB endpoint interrupts only set flags. `s2s_usbDevicePoll()` does the
  storage work from the main loop, after `scsiPoll()` and `scsiDiskPoll()`, so
  the two never use the SD card at the same instant.
- There is no file system. Each USB LUN is the same raw SD sector range as the
  SCSI target (`cfg->sdSectorStart + blk_addr`). Nothing is cached.
- There is no locking and no check of the SCSI phase. A host that has read
  data keeps its own copy; SCSI2SD does nothing about that either.
- USB is stopped completely when the "turbo" SCSI speed is selected.

Little of this carries over. ZuluSCSI's USB stack (TinyUSB through
arduino-pico), SD driver (PIO SDIO) and storage layer (image files on
FAT/exFAT through SdFat) are all different.

## "One writer, one reader" is not enough

With USB writing and SCSI only reading there are no write-write conflicts, but
stale and half-updated reads remain:

| Layer | Problem |
|---|---|
| PC operating system | Write cache: edits reach the card when the OS decides. An OS that recognises the volume may write metadata on its own. |
| ZuluSCSI | Read-ahead cache (`g_scsi_prefetch`) holds sectors read earlier. Must be invalidated when USB writes. |
| SD card file system | Only relevant in whole-card mode: the firmware caches where each image lives and writes `zululog.txt`. If the PC changes the allocation, the firmware reads the wrong sectors or the two corrupt each other's FAT. |
| SCSI host | Caches directory and allocation data of the file system inside the image. It can also read a multi-sector update halfway through. |

The last row is the one no storage-level fix addresses. The SCSI host must be
told that the medium changed.

## Existing building blocks

- **Removable device types**: CD-ROM, Zip 100 (`S2S_CFG_ZIP100`, image prefix
  `ZP`), removable disk, MO, floppy, tape.
- **Ejected state**: `image_config_t::ejected`. While set, TEST UNIT READY
  returns NOT READY / MEDIUM NOT PRESENT (`doTestUnitReady()` in
  `src/ZuluSCSI_disk.cpp`), and the CD-ROM code refuses its own commands. The
  image file stays open.
  **Reads and writes of disk type devices (Zip, removable, MO) are not
  refused**: `scsi.c` calls `scsiDiskCommand()` before any readiness check,
  and `scsiDiskStartRead()` / `scsiDiskStartWrite()` do not look at the
  flag. An earlier version of this note claimed that all media access was
  refused; that was wrong, and the prototype relied on it until the check
  described in 02-prototype-design.md ("Ownership") was added.
- **Reinsert**: `scsiDiskCloseTray()` clears the flag and posts UNIT ATTENTION
  "medium may have changed" when `EnableUnitAttention` is on.
- **Auto reinsert**: with default settings the tray closes itself on the
  host's next status poll (`reinsert_after_eject`), and on INQUIRY
  (`ReinsertCDOnInquiry`). A handoff needs to suppress this.
- **`doPerformEject()` switches images**: for a device with several images
  (IMG0..IMGn) an eject loads the next one. A handoff must not use it.
- **USB serial console**: the `m` media menu already ejects and inserts
  (`src/ZuluSCSI_usb_console_media.cpp`), but it is interactive and shares the
  stream with log output.
- **Composite USB device**: serial and storage already coexist on one port.
  Card reader mode accepts `x` on the serial console while storage is active.
- **Raw access to contiguous images**: `ImageBackingStore::linearSdMapping()`
  reports when an image is one contiguous SD sector range accessed without the
  SdFat cache. For such an image USB I/O is plain sector arithmetic, as on
  SCSI2SD.
- **SD writes from interrupt context work**: `rp2040_sdio_tx_poll()` calls the
  DMA interrupt handler itself when it runs inside an interrupt.
- **Existing lock**: `platform_msc_lock_set()` takes `USB.mutex`, which makes
  the USB timer interrupt skip `tud_task()`.

## Host notes (Ensoniq)

- The sampler cannot boot from CD-ROM, so the image is emulated as a Zip drive.
- The Ensoniq file system is proprietary. A PC cannot mount it; Windows shows a
  raw disk and may offer to format it. Sectors are written by a tool derived
  from EpsLin.
- Open question: whether the sampler notices a media change by itself or needs
  the storage device re-selected. To be tested on the hardware.

## Things that were considered and rejected

- **Whole SD card over USB during emulation.** Two parties would have the same
  FAT volume mounted, and the firmware writes to it. Not fixable without
  making the firmware fully read-only on the card and re-resolving image
  locations after every PC change.
- **Deferring storage work to bus-free windows in the main loop** (the SCSI2SD
  model). It works, but USB (including the serial log) is not serviced at all
  during a SCSI command, and with TinyUSB 0.18 the serial code paths would
  have to be rewritten to bypass `Serial`. The handoff design does not need it.
- **CD-ROM emulation with eject/reload.** Same idea as the handoff, but ISO9660
  cannot be edited in place and the sampler cannot boot from it.

## Platform scope

- RP2040 with the built-in PIO SDIO driver (`sd_card_sdio.cpp`): covered.
- RP2350 boards use the external `SDIO_RP2350` library; the same hooks would be
  needed there. Not done.
- GD32 boards (ZuluSCSI v1.x) use the vendor USB stack and have no image mode.
  Separate port, not done.
