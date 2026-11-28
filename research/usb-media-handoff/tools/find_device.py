#!/usr/bin/env python3
"""Find ZuluSCSI boards that run the USB media handoff firmware (Windows).

Lists, for each board, the serial port for the '@' commands and the drive
letter of each USB disk, matched through the USB serial number. Sends nothing
to the board and needs no administrator rights.

    find_device.py

Needs pyserial (pip install pyserial). The method is described in
../04-app-integration.md, "Finding the board".
"""

import ctypes
import string
import sys
from ctypes import wintypes

from serial.tools import list_ports

USB_VID = 0x2E8A
USB_PID = 0x10D7

DRIVE_REMOVABLE = 2
FILE_SHARE_READ = 1
FILE_SHARE_WRITE = 2
OPEN_EXISTING = 3
INVALID_HANDLE_VALUE = wintypes.HANDLE(-1).value
IOCTL_STORAGE_QUERY_PROPERTY = 0x002D1400
IOCTL_STORAGE_GET_DEVICE_NUMBER = 0x002D1080
IOCTL_DISK_GET_LENGTH_INFO = 0x0007405C
GENERIC_READ = 0x80000000

kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
kernel32.CreateFileW.restype = wintypes.HANDLE
kernel32.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, ctypes.c_void_p,
                                 wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]
kernel32.DeviceIoControl.argtypes = [wintypes.HANDLE, wintypes.DWORD, ctypes.c_void_p, wintypes.DWORD,
                                     ctypes.c_void_p, wintypes.DWORD, ctypes.POINTER(wintypes.DWORD),
                                     ctypes.c_void_p]
kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
kernel32.GetDriveTypeW.argtypes = [wintypes.LPCWSTR]


def open_volume(letter, access):
    """Open \\\\.\\X: and return the handle, or None."""
    handle = kernel32.CreateFileW("\\\\.\\%s:" % letter, access, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                  None, OPEN_EXISTING, 0, None)
    return None if handle == INVALID_HANDLE_VALUE else handle


def ioctl(handle, code, in_buffer, out_size):
    out_buffer = ctypes.create_string_buffer(out_size)
    returned = wintypes.DWORD(0)
    ok = kernel32.DeviceIoControl(handle, code, in_buffer, len(in_buffer) if in_buffer else 0,
                                  out_buffer, out_size, ctypes.byref(returned), None)
    return out_buffer.raw[:returned.value] if ok else None


def text_at(data, offset):
    """Zero-terminated ASCII string at an offset of a descriptor (0 = absent)."""
    if not offset or offset >= len(data):
        return ""
    return data[offset:data.index(b"\0", offset)].decode("ascii", errors="replace").strip()


def describe_volume(letter):
    """Vendor, product, USB serial number and disk number of a drive letter.

    Opened with no access rights, which works without media and without
    administrator rights.
    """
    handle = open_volume(letter, 0)
    if handle is None:
        return None
    try:
        # STORAGE_PROPERTY_QUERY: StorageDeviceProperty, PropertyStandardQuery
        descriptor = ioctl(handle, IOCTL_STORAGE_QUERY_PROPERTY, bytes(12), 1024)
        if not descriptor or len(descriptor) < 36:
            return None
        vendor_offset = int.from_bytes(descriptor[12:16], "little")
        product_offset = int.from_bytes(descriptor[16:20], "little")
        serial_offset = int.from_bytes(descriptor[24:28], "little")
        info = {
            "letter": letter,
            "vendor": text_at(descriptor, vendor_offset),
            "product": text_at(descriptor, product_offset),
            "serial": text_at(descriptor, serial_offset),
            "disk_number": None,
        }
        number = ioctl(handle, IOCTL_STORAGE_GET_DEVICE_NUMBER, None, 12)
        if number and len(number) >= 8:
            info["disk_number"] = int.from_bytes(number[4:8], "little")
        return info
    finally:
        kernel32.CloseHandle(handle)


def media_size(letter):
    """Size of the media in bytes, or None if the drive has no media.

    Without media the volume can still be opened; the length is then 0 (or
    the request fails).
    """
    handle = open_volume(letter, GENERIC_READ)
    if handle is None:
        return None
    try:
        length = ioctl(handle, IOCTL_DISK_GET_LENGTH_INFO, None, 8)
        size = int.from_bytes(length, "little") if length and len(length) == 8 else 0
        return size or None
    finally:
        kernel32.CloseHandle(handle)


def find_boards():
    """Return a list of boards: serial number, serial port and drive letters."""
    volumes = []
    for letter in string.ascii_uppercase:
        if kernel32.GetDriveTypeW("%s:\\" % letter) != DRIVE_REMOVABLE:
            continue
        info = describe_volume(letter)
        if info and info["vendor"].lower() == "zuluscsi":
            volumes.append(info)

    boards = []
    for port in list_ports.comports():
        if port.vid != USB_VID or port.pid != USB_PID:
            continue
        serial_number = (port.serial_number or "").upper()
        boards.append({
            "serial": serial_number,
            "port": port.device,
            "volumes": [v for v in volumes if serial_number and serial_number in v["serial"].upper()],
        })
    return boards


def main():
    if sys.platform != "win32":
        print("This helper only knows how to find the disk on Windows.", file=sys.stderr)
        return 2

    boards = find_boards()
    if not boards:
        print("No ZuluSCSI with USB ID %04X:%04X found." % (USB_VID, USB_PID))
        return 1

    for board in boards:
        print("board %s: serial port %s" % (board["serial"] or "(no serial number)", board["port"]))
        if not board["volumes"]:
            print("  no USB disk found; is USBMediaHandoff = 1 set and a removable image present?")
        # Drive letters are listed in the order Windows assigned them. Which
        # one is which USB LUN is not known from here; compare the size after
        # '@take' with the 'sectors' value of the reply.
        for volume in board["volumes"]:
            size = media_size(volume["letter"])
            state = "no media" if size is None else "media, %d sectors" % (size // 512)
            print("  disk %s: (\\\\.\\PhysicalDrive%s, %s %s) %s" % (
                volume["letter"], volume["disk_number"], volume["vendor"], volume["product"], state))
    return 0


if __name__ == "__main__":
    sys.exit(main())
