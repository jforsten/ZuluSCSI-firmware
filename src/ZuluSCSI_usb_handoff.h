/**
 * Copyright (c) 2026 Juha Forsten
 *
 * ZuluSCSI™ firmware is licensed under the GPL version 3 or any later version.
 *
 * https://www.gnu.org/licenses/gpl-3.0.html
 * ----
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 **/

// USB media handoff: lets a PC edit a removable-media image over USB while
// SCSI emulation keeps running. The media is in one place at a time: either
// the SCSI host has it, or the USB host has it. Moving it looks like an eject
// and an insert to each host.
//
// Enabled with USBMediaHandoff = 1 in the [SCSI] section of the config file.
// See research/usb-media-handoff/ for the reasoning and the serial protocol.

#pragma once

#include <stdint.h>
#include "ZuluSCSI_platform.h"

#ifdef PLATFORM_HAS_USB_MEDIA_HANDOFF

/* Called from the main program, never from an interrupt */

// Read the setting, find the images that can take part and register the USB
// storage interface. Call once after the images have been opened.
void usbHandoffInit();

// Carry out pending take and give requests and check that the images USB
// owns are still the ones it was given. Call from the main loop, only while
// the SCSI bus is free.
void usbHandoffPoll();

// Feed one character from the USB serial port. Returns true if the character
// belonged to an '@' command line and must not be handled as a menu key.
bool usbHandoffSerialInput(char c);

// Take the media away from USB without giving it back to the SCSI host,
// because the image is about to be closed or replaced.
void usbHandoffRevoke(int scsi_id);
void usbHandoffRevokeAll();

/* Backends for the USB mass storage callbacks. These run inside the USB
   stack, possibly in an interrupt, and must not log or use the file system. */

bool usbHandoffActive();
uint8_t usbHandoffLunCount();

// True if USB owns the media of this LUN. *media_changed is set once after
// each take, so that the USB host can be told that new media arrived.
bool usbHandoffUnitReady(uint8_t lun, bool *media_changed);
void usbHandoffCapacity(uint8_t lun, uint32_t *block_count, uint16_t *block_size);
bool usbHandoffWritable(uint8_t lun);

// Return the number of bytes transferred, or -1 on error.
// Writes are collected in a buffer and reach the SD card a little later:
// when the buffer is full, when the host reads, ejects or sends SYNCHRONIZE
// CACHE, on "@give", or after a few milliseconds without further writes.
int32_t usbHandoffRead(uint8_t lun, uint32_t lba, uint32_t offset, void *buffer, uint32_t bufsize);
int32_t usbHandoffWrite(uint8_t lun, uint32_t lba, uint32_t offset, const uint8_t *buffer, uint32_t bufsize);

// True if USB owns the media of this LUN.
bool usbHandoffHasMedia(uint8_t lun);

// True, once, if buffered data could not be written to the card after the
// host had been told that its write succeeded.
bool usbHandoffTakeWriteError(uint8_t lun);

// Write buffered data of this LUN to the card now. False if that failed.
bool usbHandoffSync(uint8_t lun);

// The USB host sent START STOP UNIT. False if an eject could not be carried
// out because buffered data could not be written to the card.
bool usbHandoffStartStop(uint8_t lun, bool start, bool load_eject);

#else

// Platforms without the feature: everything is a no-op so that callers do
// not need their own #ifdefs.
static inline void usbHandoffInit() {}
static inline void usbHandoffPoll() {}
static inline bool usbHandoffSerialInput(char c) { (void)c; return false; }
static inline void usbHandoffRevoke(int scsi_id) { (void)scsi_id; }
static inline void usbHandoffRevokeAll() {}
static inline bool usbHandoffActive() { return false; }

#endif
