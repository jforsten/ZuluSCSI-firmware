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

#include "ZuluSCSI_usb_handoff.h"

#ifdef PLATFORM_HAS_USB_MEDIA_HANDOFF

#include <SdFat.h>
#include <minIni.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include "ZuluSCSI_config.h"
#include "ZuluSCSI_log.h"
#include "ZuluSCSI_msc.h"
#include "ZuluSCSI_disk.h"
#include "ZuluSCSI_gap_layout.h"

extern SdFs SD;
extern bool g_sdcard_present;

#define HANDOFF_SECTOR_SIZE 512

// A give waits until USB has been idle this long, so that it does not cut
// into a transfer the USB host is still in the middle of.
#define HANDOFF_GIVE_QUIET_MS 300

// After this long the give goes ahead even if USB is still busy.
#define HANDOFF_GIVE_TIMEOUT_MS 5000

// A serial command line is abandoned if its characters stop arriving.
#define HANDOFF_LINE_TIMEOUT_MS 2000

// Size of the buffer that collects USB writes before they go to the SD card,
// in sectors. 32 sectors is 16 kB, four of the 4 kB steps a USB transfer is
// made of.
#define HANDOFF_WB_SECTORS 32

// Buffered data is written to the card at the latest this long after the
// last USB write added to it.
#define HANDOFF_WB_IDLE_MS 20

enum handoff_request_t : uint8_t
{
    HANDOFF_REQ_NONE = 0,
    HANDOFF_REQ_TAKE,
    HANDOFF_REQ_GIVE
};

enum handoff_source_t : uint8_t
{
    HANDOFF_SRC_SERIAL = 0,
    HANDOFF_SRC_USB
};

struct handoff_lun_t
{
    uint8_t scsi_id;

    // What the USB callbacks work from. bgn_sector, sector_count and writable
    // are only written while usb_owns is false, and usb_owns is written last
    // on take and first on give. A USB callback runs to completion before the
    // main program continues, so it never sees a half-updated set.
    volatile bool usb_owns;
    uint32_t bgn_sector;
    uint32_t sector_count;
    bool writable;

    // Set on take, cleared when the USB host has been told about new media.
    volatile bool media_changed;

    // Time of the last USB read or write.
    volatile uint32_t last_io_ms;

    // Counted in the USB callbacks, reported from the main loop.
    volatile uint32_t io_errors;
    uint32_t io_errors_reported;

    // Buffered data could not be written to the card, after the USB host had
    // already been told that its write succeeded. write_error_unreported is
    // passed on to the host with its next command, write_failed to the
    // serial side with the reply to "@give".
    volatile bool write_error_unreported;
    volatile bool write_failed;

    // Since the last take: sectors written to the card, in how many SD card
    // writes, and how many of those were done from a USB callback because
    // the main loop had not got to it.
    volatile uint32_t stat_sectors;
    volatile uint32_t stat_writes;
    volatile uint32_t stat_writes_callback;

    // Request waiting for the main loop. Written by the serial parser and by
    // the USB START STOP callback.
    volatile handoff_request_t request;
    volatile handoff_source_t request_source;
    uint32_t request_time_ms;
    bool request_time_valid;
};

static bool g_handoff_active;
static uint8_t g_handoff_lun_count;
static handoff_lun_t g_handoff_luns[S2S_MAX_TARGETS];

// Write-behind buffer: one run of consecutive sectors that USB has written
// and that is not on the SD card yet. A USB write only copies into it, so
// the next step of the transfer can start right away, and the main loop
// writes it to the card while that step is on the wire.
//
// Only touched with the storage lock held: inside a USB callback, or between
// platform_msc_lock_set(true) and (false) in the main program. The two never
// run at the same time, see wb_flush_from_main().
static struct
{
    uint8_t lun;
    uint32_t lba;            // first buffered sector, counted from the image start
    uint32_t count;          // buffered sectors, 0 when empty
    uint32_t chunk;          // sectors added by the last write
    uint32_t last_append_ms;
} g_wb;

static uint8_t g_wb_buf[HANDOFF_WB_SECTORS * HANDOFF_SECTOR_SIZE] __attribute__((aligned(4)));

// Serial command line being received
static char g_handoff_line[32];
static uint8_t g_handoff_line_len;
static bool g_handoff_line_active;
static uint32_t g_handoff_line_time_ms;

static handoff_lun_t *find_lun(int scsi_id)
{
    for (int i = 0; i < g_handoff_lun_count; i++)
    {
        if (g_handoff_luns[i].scsi_id == scsi_id)
        {
            return &g_handoff_luns[i];
        }
    }
    return nullptr;
}

static bool type_can_hand_off(S2S_CFG_TYPE type)
{
    switch (type)
    {
        case S2S_CFG_REMOVABLE:
        case S2S_CFG_MO:
        case S2S_CFG_ZIP100:
        case S2S_CFG_FLOPPY_14MB:
            return true;

        // Fixed disks have no way to tell the host that the media changed.
        // CD-ROM and tape images are not laid out as plain sectors.
        default:
            return false;
    }
}

// Work out which SD card sectors the image occupies. Returns nullptr if the
// image can be handed to USB, otherwise the reason it cannot.
static const char *resolve_image(image_config_t &img, uint32_t *bgn_sector, uint32_t *sector_count)
{
    if (!g_sdcard_present)
    {
        return "no-card";
    }

    if (!img.file.isOpen() || !type_can_hand_off((S2S_CFG_TYPE)img.deviceType))
    {
        return "no-image";
    }

    if (img.bytesPerSector != HANDOFF_SECTOR_SIZE)
    {
        return "sector-size";
    }

    // USB reads and writes card sectors directly, without the file system in
    // between, so the image has to be one plain run of sectors.
    uint32_t bgn = 0;
    uint32_t end = 0;
    uint8_t align_mode = 0;
    if (!img.file.linearSdMapping(&bgn, &align_mode) ||
        align_mode != ALIGN_UNALIGNED_OFF ||
        !img.file.contiguousRange(&bgn, &end) ||
        end < bgn)
    {
        return "not-contiguous";
    }

    uint64_t sectors = (uint64_t)end - bgn + 1;
    uint64_t size_sectors = img.file.size() / HANDOFF_SECTOR_SIZE;
    if (size_sectors < sectors)
    {
        sectors = size_sectors;
    }

    if (sectors == 0)
    {
        return "no-image";
    }

    *bgn_sector = bgn;
    *sector_count = (uint32_t)sectors;
    return nullptr;
}

/*****************************************/
/* Write-behind buffer                   */
/*****************************************/

// Write the buffered sectors to the SD card. The storage lock must be held.
// Returns false if the write failed. The data is dropped in that case and
// the failure is remembered for the host, unless it is the host's current
// command on reporting_lun that will fail because of it.
static bool wb_flush(bool from_callback, uint8_t reporting_lun)
{
    uint32_t count = g_wb.count;
    if (count == 0)
    {
        return true;
    }
    g_wb.count = 0;

    handoff_lun_t &l = g_handoff_luns[g_wb.lun];
    if (!l.usb_owns)
    {
        // Should not happen: the buffer is written or dropped before USB
        // loses the media. Never write to an image USB does not own.
        return true;
    }

    // A SCSI transfer of another device may have its stream callback
    // installed. It must not be called for this write.
    platform_sd_callback_suspend(true);
    LED_ON();
    bool ok = SD.card()->writeSectors(l.bgn_sector + g_wb.lba, g_wb_buf, count);
    LED_OFF();
    platform_sd_callback_suspend(false);

    l.last_io_ms = millis();
    l.stat_sectors += count;
    l.stat_writes++;
    if (from_callback)
    {
        l.stat_writes_callback++;
    }

    if (!ok)
    {
        l.io_errors++;
        l.write_failed = true;
        if (g_wb.lun != reporting_lun)
        {
            l.write_error_unreported = true;
        }
    }
    return ok;
}

// The same from the main program. Taking the storage lock keeps the USB
// timer interrupt from running a storage callback meanwhile, so the buffer
// cannot change under the write.
static bool wb_flush_from_main()
{
    if (g_wb.count == 0)
    {
        return true;
    }

    platform_msc_lock_set(true);
    bool ok = wb_flush(false, 0xFF);
    platform_msc_lock_set(false);
    return ok;
}

// Forget buffered data of a LUN that USB is losing without a give.
// Returns the number of sectors that were lost.
static uint32_t wb_drop_from_main(uint8_t lun)
{
    if (g_wb.count == 0 || g_wb.lun != lun)
    {
        return 0;
    }

    platform_msc_lock_set(true);
    uint32_t lost = (g_wb.lun == lun) ? g_wb.count : 0;
    if (lost)
    {
        g_wb.count = 0;
    }
    platform_msc_lock_set(false);
    return lost;
}

/*****************************************/
/* Take, give and revoke (main program)  */
/*****************************************/

static void do_take(handoff_lun_t &lun)
{
    int id = lun.scsi_id;
    image_config_t &img = scsiDiskGetImageConfig(id);

    uint32_t bgn = 0;
    uint32_t count = 0;
    const char *reason = resolve_image(img, &bgn, &count);
    if (reason)
    {
        logmsg("@ERR take id=", id, " reason=", reason);
        return;
    }

    if (!img.usb_hold)
    {
        // The SCSI host loses the media first. From here on its media access
        // commands are refused, and nothing reinserts until USB gives it back.
        img.ejected = true;
        img.usb_hold = true;
        scsiDiskPrefetchInvalidate(id);

        lun.bgn_sector = bgn;
        lun.sector_count = count;
        lun.writable = img.file.isWritable();
        lun.last_io_ms = millis();
        lun.write_error_unreported = false;
        lun.write_failed = false;
        lun.stat_sectors = 0;
        lun.stat_writes = 0;
        lun.stat_writes_callback = 0;
        lun.media_changed = true;
        lun.usb_owns = true;
    }
    else if (!lun.usb_owns)
    {
        // The USB host ejected and now wants the media again before the give
        // was carried out.
        lun.media_changed = true;
        lun.usb_owns = true;
    }

    if (lun.request_source == HANDOFF_SRC_USB)
    {
        logmsg("@EVENT take id=", id, " source=usb-load");
    }
    logmsg("@OK take id=", id, " sectors=", (int)lun.sector_count, " ro=", lun.writable ? 0 : 1);
}

static void do_give(handoff_lun_t &lun, bool forced)
{
    int id = lun.scsi_id;
    image_config_t &img = scsiDiskGetImageConfig(id);

    // Everything USB wrote has to be on the card before the SCSI host gets
    // the media back.
    wb_flush_from_main();

    bool write_failed = lun.write_failed;
    lun.write_failed = false;
    lun.write_error_unreported = false;

    if (lun.stat_writes > 0)
    {
        logmsg("USB media handoff: SCSI ID ", id, ", ", (int)lun.stat_sectors, " sectors written in ",
               (int)lun.stat_writes, " SD card writes, ", (int)lun.stat_writes_callback, " of them from USB callbacks");
        lun.stat_sectors = 0;
        lun.stat_writes = 0;
        lun.stat_writes_callback = 0;
    }

    if (write_failed && lun.usb_owns && lun.request_source == HANDOFF_SRC_SERIAL)
    {
        // Some of what USB wrote did not reach the card. The media stays
        // with USB, so that the data can be written again. The next "@give"
        // goes through.
        logmsg("@ERR give id=", id, " reason=write-failed");
        return;
    }

    // USB loses the media first.
    lun.usb_owns = false;

    if (img.usb_hold)
    {
        img.usb_hold = false;
        scsiDiskPrefetchInvalidate(id);
        scsiDiskCloseTray(img);
    }

    if (lun.request_source == HANDOFF_SRC_USB)
    {
        logmsg("@EVENT give id=", id, " source=usb-eject");
    }

    if (write_failed)
    {
        // Only reached when the USB host ejected: it has let go of the media
        // already, so it goes back despite the failed write.
        logmsg("@EVENT write-failed id=", id);
    }

    if (forced)
    {
        logmsg("@OK give id=", id, " forced=1");
    }
    else
    {
        logmsg("@OK give id=", id);
    }
}

static void revoke_lun(handoff_lun_t &lun, const char *reason)
{
    image_config_t &img = scsiDiskGetImageConfig(lun.scsi_id);
    bool had_media = lun.usb_owns || img.usb_hold;

    lun.usb_owns = false;
    lun.request = HANDOFF_REQ_NONE;
    lun.request_time_valid = false;
    img.usb_hold = false;

    // The sectors this data was meant for no longer belong to the image USB
    // was given, so it cannot be written anymore.
    uint32_t lost = wb_drop_from_main((uint8_t)(&lun - g_handoff_luns));

    if (had_media)
    {
        logmsg("@EVENT revoke id=", (int)lun.scsi_id, " reason=", reason);
    }

    if (lost)
    {
        logmsg("@EVENT write-lost id=", (int)lun.scsi_id, " sectors=", (int)lost);
    }
}

void usbHandoffRevoke(int scsi_id)
{
    if (!g_handoff_active) return;

    handoff_lun_t *lun = find_lun(scsi_id);
    if (lun)
    {
        revoke_lun(*lun, "image-changed");
    }
}

void usbHandoffRevokeAll()
{
    if (!g_handoff_active) return;

    for (int i = 0; i < g_handoff_lun_count; i++)
    {
        revoke_lun(g_handoff_luns[i], "images-closed");
    }
}

void usbHandoffInit()
{
    if (g_handoff_active)
    {
        return;
    }

    if (!ini_getbool("SCSI", "USBMediaHandoff", false, CONFIGFILE))
    {
        return;
    }

    g_handoff_lun_count = 0;

    for (int id = 0; id < S2S_MAX_TARGETS; id++)
    {
        image_config_t &img = scsiDiskGetImageConfig(id);
        if (!img.file.isOpen() || !type_can_hand_off((S2S_CFG_TYPE)img.deviceType))
        {
            continue;
        }

        uint32_t bgn = 0;
        uint32_t count = 0;
        const char *reason = resolve_image(img, &bgn, &count);
        if (reason)
        {
            logmsg("USB media handoff: SCSI ID ", id, " cannot be handed to USB as it is now (", reason, ")");
        }
        else
        {
            logmsg("USB media handoff: SCSI ID ", id, " is USB LUN ", (int)g_handoff_lun_count,
                   ", ", (int)count, " sectors");
        }

        g_handoff_luns[g_handoff_lun_count].scsi_id = id;
        g_handoff_lun_count++;
    }

    if (g_handoff_lun_count == 0)
    {
        logmsg("USB media handoff: no removable-media images found, not started");
        return;
    }

    if (!platform_msc_handoff_begin(g_handoff_lun_count))
    {
        logmsg("USB media handoff: could not register the USB storage interface");
        g_handoff_lun_count = 0;
        return;
    }

    g_handoff_active = true;
    logmsg("USB media handoff started with ", (int)g_handoff_lun_count,
           " LUN(s). Serial commands: @status, @take [id], @give [id]");
}

void usbHandoffPoll()
{
    if (!g_handoff_active) return;

    bool usb_has_media = false;

    for (int i = 0; i < g_handoff_lun_count; i++)
    {
        handoff_lun_t &lun = g_handoff_luns[i];
        image_config_t &img = scsiDiskGetImageConfig(lun.scsi_id);

        if (lun.usb_owns)
        {
            usb_has_media = true;
        }

        // The image USB owns must still be the ejected, held image it was
        // given. Anything else means some code path changed it without going
        // through usbHandoffRevoke().
        if (lun.usb_owns)
        {
            if (!g_sdcard_present)
            {
                revoke_lun(lun, "no-card");
            }
            else if (!img.file.isOpen() || !img.usb_hold || !img.ejected)
            {
                revoke_lun(lun, "image-state");
            }
        }

        if (lun.io_errors != lun.io_errors_reported)
        {
            lun.io_errors_reported = lun.io_errors;
            logmsg("@EVENT io-error id=", (int)lun.scsi_id, " count=", (int)lun.io_errors_reported);
        }

        handoff_request_t request = lun.request;
        if (request == HANDOFF_REQ_TAKE)
        {
            lun.request = HANDOFF_REQ_NONE;
            lun.request_time_valid = false;
            do_take(lun);
        }
        else if (request == HANDOFF_REQ_GIVE)
        {
            uint32_t now = millis();
            if (!lun.request_time_valid)
            {
                lun.request_time_ms = now;
                lun.request_time_valid = true;
            }

            bool quiet = !lun.usb_owns ||
                         (uint32_t)(now - lun.last_io_ms) >= HANDOFF_GIVE_QUIET_MS;
            bool timeout = (uint32_t)(now - lun.request_time_ms) >= HANDOFF_GIVE_TIMEOUT_MS;

            if (quiet || timeout)
            {
                lun.request = HANDOFF_REQ_NONE;
                lun.request_time_valid = false;
                do_give(lun, !quiet);
            }
        }
    }

    // Write buffered USB data to the card once no further step fits in the
    // buffer, or when nothing has been added for a while. The USB host is
    // sending the next step meanwhile.
    if (g_wb.count > 0)
    {
        bool full = g_wb.count + g_wb.chunk > HANDOFF_WB_SECTORS;
        bool idle = (uint32_t)(millis() - g_wb.last_append_ms) >= HANDOFF_WB_IDLE_MS;
        if (full || idle)
        {
            wb_flush_from_main();
        }
    }

    // A storage transfer is a chain of 4 kB steps, each started by the USB
    // stack when the previous one has finished. Run the stack on every pass
    // while a transfer is possible, so that a step does not sit waiting for
    // the once-per-millisecond timer. Only while USB has media: the SCSI
    // side keeps its usual timing the rest of the time.
    if (usb_has_media)
    {
        platform_msc_handoff_task();
    }
}

/*****************************************/
/* Serial command lines                  */
/*****************************************/

static void serial_status()
{
    for (int i = 0; i < g_handoff_lun_count; i++)
    {
        handoff_lun_t &lun = g_handoff_luns[i];
        image_config_t &img = scsiDiskGetImageConfig(lun.scsi_id);

        uint32_t bgn = 0;
        uint32_t count = 0;
        const char *reason = nullptr;
        bool writable = lun.writable;
        if (lun.usb_owns)
        {
            count = lun.sector_count;
        }
        else
        {
            reason = resolve_image(img, &bgn, &count);
            writable = img.file.isWritable();
        }

        if (reason)
        {
            logmsg("@LUN lun=", i, " id=", (int)lun.scsi_id, " owner=scsi unavailable=", reason);
        }
        else
        {
            logmsg("@LUN lun=", i, " id=", (int)lun.scsi_id,
                   " owner=", lun.usb_owns ? "usb" : "scsi",
                   " sectors=", (int)count, " ro=", writable ? 0 : 1);
        }
    }
    logmsg("@OK status luns=", (int)g_handoff_lun_count);
}

// Parse the optional SCSI ID argument. Returns the LUN, or nullptr after
// having reported the error.
static handoff_lun_t *serial_get_lun(const char *command, const char *arg)
{
    while (*arg == ' ') arg++;

    if (*arg == '\0')
    {
        if (g_handoff_lun_count == 1)
        {
            return &g_handoff_luns[0];
        }
        logmsg("@ERR ", command, " reason=bad-id");
        return nullptr;
    }

    char *end = nullptr;
    long id = strtol(arg, &end, 10);
    while (*end == ' ') end++;
    if (end == arg || *end != '\0' || id < 0 || id >= S2S_MAX_TARGETS)
    {
        logmsg("@ERR ", command, " reason=bad-id");
        return nullptr;
    }

    handoff_lun_t *lun = find_lun((int)id);
    if (!lun)
    {
        logmsg("@ERR ", command, " id=", (int)id, " reason=no-such-lun");
    }
    return lun;
}

static void serial_execute(char *line)
{
    // Split the command word from its argument
    char *arg = line;
    while (*arg != '\0' && *arg != ' ') arg++;
    if (*arg == ' ')
    {
        *arg++ = '\0';
    }

    bool is_status = (strcasecmp(line, "@status") == 0);
    bool is_take = (strcasecmp(line, "@take") == 0);
    bool is_give = (strcasecmp(line, "@give") == 0);

    if (!is_status && !is_take && !is_give)
    {
        logmsg("@ERR unknown-command, use @status, @take [id] or @give [id]");
        return;
    }

    if (!g_handoff_active)
    {
        logmsg("@ERR ", line + 1, " reason=disabled");
        return;
    }

    if (is_status)
    {
        serial_status();
        return;
    }

    handoff_lun_t *lun = serial_get_lun(line + 1, arg);
    if (lun)
    {
        // Carried out by usbHandoffPoll() when the SCSI bus is free. A newer
        // request replaces one that has not been carried out yet.
        lun->request_source = HANDOFF_SRC_SERIAL;
        lun->request_time_valid = false;
        lun->request = is_take ? HANDOFF_REQ_TAKE : HANDOFF_REQ_GIVE;
    }
}

bool usbHandoffSerialInput(char c)
{
    uint32_t now = millis();
    if (g_handoff_line_active && (uint32_t)(now - g_handoff_line_time_ms) > HANDOFF_LINE_TIMEOUT_MS)
    {
        // A line that was started but never finished must not swallow the
        // menu keys typed later.
        g_handoff_line_active = false;
    }
    g_handoff_line_time_ms = now;

    if (!g_handoff_line_active)
    {
        if (c != '@')
        {
            return false;
        }

        g_handoff_line_active = true;
        g_handoff_line_len = 0;
    }

    if (c == '\r' || c == '\n')
    {
        g_handoff_line[g_handoff_line_len] = '\0';
        g_handoff_line_active = false;
        serial_execute(g_handoff_line);
    }
    else if (g_handoff_line_len < sizeof(g_handoff_line) - 1)
    {
        g_handoff_line[g_handoff_line_len++] = c;
    }
    else
    {
        // Too long to be one of our commands, drop the rest of the line
        g_handoff_line_len = 0;
        g_handoff_line[g_handoff_line_len++] = '?';
    }

    return true;
}

/*****************************************/
/* USB mass storage callback backends    */
/*****************************************/

bool usbHandoffActive()
{
    return g_handoff_active;
}

uint8_t usbHandoffLunCount()
{
    return g_handoff_lun_count;
}

bool usbHandoffUnitReady(uint8_t lun, bool *media_changed)
{
    *media_changed = false;
    if (lun >= g_handoff_lun_count) return false;

    handoff_lun_t &l = g_handoff_luns[lun];
    if (!l.usb_owns) return false;

    if (l.media_changed)
    {
        l.media_changed = false;
        *media_changed = true;
    }
    return true;
}

void usbHandoffCapacity(uint8_t lun, uint32_t *block_count, uint16_t *block_size)
{
    *block_size = HANDOFF_SECTOR_SIZE;
    *block_count = 0;

    if (lun < g_handoff_lun_count && g_handoff_luns[lun].usb_owns)
    {
        *block_count = g_handoff_luns[lun].sector_count;
    }
}

bool usbHandoffWritable(uint8_t lun)
{
    return lun < g_handoff_lun_count &&
           g_handoff_luns[lun].usb_owns &&
           g_handoff_luns[lun].writable;
}

// Check a transfer request against the media USB owns. Returns the first SD
// card sector of the transfer and the sector count.
static bool check_transfer(uint8_t lun, uint32_t lba, uint32_t offset, uint32_t bufsize,
                           uint32_t *sd_sector, uint32_t *sectors)
{
    if (lun >= g_handoff_lun_count) return false;

    handoff_lun_t &l = g_handoff_luns[lun];
    if (!l.usb_owns) return false;

    if (offset != 0 || bufsize == 0 || (bufsize % HANDOFF_SECTOR_SIZE) != 0) return false;

    uint32_t count = bufsize / HANDOFF_SECTOR_SIZE;
    if (lba >= l.sector_count || count > l.sector_count - lba) return false;

    *sd_sector = l.bgn_sector + lba;
    *sectors = count;
    return true;
}

int32_t usbHandoffRead(uint8_t lun, uint32_t lba, uint32_t offset, void *buffer, uint32_t bufsize)
{
    uint32_t sd_sector = 0;
    uint32_t sectors = 0;
    if (!check_transfer(lun, lba, offset, bufsize, &sd_sector, &sectors))
    {
        return -1;
    }

    handoff_lun_t &l = g_handoff_luns[lun];

    // What the host reads must include what it has just written
    if (g_wb.count > 0 && g_wb.lun == lun && !wb_flush(true, lun))
    {
        return -1;
    }

    // A SCSI transfer of another device may have its stream callback
    // installed. It must not be called for this transfer.
    platform_sd_callback_suspend(true);
    LED_ON();
    bool ok = SD.card()->readSectors(sd_sector, (uint8_t*)buffer, sectors);
    LED_OFF();
    platform_sd_callback_suspend(false);

    l.last_io_ms = millis();
    if (!ok)
    {
        l.io_errors++;
        return -1;
    }
    return (int32_t)bufsize;
}

int32_t usbHandoffWrite(uint8_t lun, uint32_t lba, uint32_t offset, const uint8_t *buffer, uint32_t bufsize)
{
    uint32_t sd_sector = 0;
    uint32_t sectors = 0;
    if (!check_transfer(lun, lba, offset, bufsize, &sd_sector, &sectors))
    {
        return -1;
    }

    handoff_lun_t &l = g_handoff_luns[lun];
    if (!l.writable)
    {
        return -1;
    }

    if (l.write_error_unreported)
    {
        // An earlier write, already acknowledged, did not reach the card.
        // Fail this one so that the host learns about it.
        l.write_error_unreported = false;
        return -1;
    }

    // The buffer holds one run of consecutive sectors. Anything that does
    // not continue it, or does not fit, goes to the card first.
    if (g_wb.count > 0 &&
        (g_wb.lun != lun || lba != g_wb.lba + g_wb.count || g_wb.count + sectors > HANDOFF_WB_SECTORS))
    {
        if (!wb_flush(true, lun))
        {
            return -1;
        }
    }

    if (sectors > HANDOFF_WB_SECTORS)
    {
        // Larger than the buffer: write it directly
        platform_sd_callback_suspend(true);
        LED_ON();
        bool ok = SD.card()->writeSectors(sd_sector, buffer, sectors);
        LED_OFF();
        platform_sd_callback_suspend(false);

        l.last_io_ms = millis();
        l.stat_sectors += sectors;
        l.stat_writes++;
        l.stat_writes_callback++;
        if (!ok)
        {
            l.io_errors++;
            return -1;
        }
        return (int32_t)bufsize;
    }

    if (g_wb.count == 0)
    {
        g_wb.lun = lun;
        g_wb.lba = lba;
    }
    memcpy(g_wb_buf + g_wb.count * HANDOFF_SECTOR_SIZE, buffer, bufsize);
    g_wb.count += sectors;
    g_wb.chunk = sectors;
    g_wb.last_append_ms = millis();
    l.last_io_ms = g_wb.last_append_ms;
    return (int32_t)bufsize;
}

bool usbHandoffHasMedia(uint8_t lun)
{
    return lun < g_handoff_lun_count && g_handoff_luns[lun].usb_owns;
}

bool usbHandoffTakeWriteError(uint8_t lun)
{
    if (lun >= g_handoff_lun_count || !g_handoff_luns[lun].write_error_unreported)
    {
        return false;
    }

    g_handoff_luns[lun].write_error_unreported = false;
    return true;
}

bool usbHandoffSync(uint8_t lun)
{
    if (g_wb.count > 0 && g_wb.lun == lun)
    {
        return wb_flush(true, lun);
    }
    return true;
}

bool usbHandoffStartStop(uint8_t lun, bool start, bool load_eject)
{
    if (lun >= g_handoff_lun_count || !load_eject) return true;

    handoff_lun_t &l = g_handoff_luns[lun];
    if (start)
    {
        // Load: the USB host asks for the media
        if (!l.usb_owns)
        {
            l.request_source = HANDOFF_SRC_USB;
            l.request = HANDOFF_REQ_TAKE;
        }
    }
    else if (l.usb_owns)
    {
        // What the host wrote goes to the card before it lets go of the
        // media. If that fails the eject fails, and the media stays.
        if (g_wb.count > 0 && g_wb.lun == lun && !wb_flush(true, lun))
        {
            return false;
        }

        // Eject: USB stops using the media at once, the main loop gives it
        // back to the SCSI host.
        l.usb_owns = false;
        l.request_source = HANDOFF_SRC_USB;
        l.request = HANDOFF_REQ_GIVE;
    }

    return true;
}

#endif // PLATFORM_HAS_USB_MEDIA_HANDOFF
