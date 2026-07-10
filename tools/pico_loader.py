#!/usr/bin/env python3
"""Standalone Eigenharp Pico firmware loader.

Loads the Pico runtime firmware (`pico.ihx`) into a pre-load USB device using
the same Cypress vendor control transfers as the original EigenD loader.
"""

from __future__ import annotations

import argparse
import sys
import time
from pathlib import Path

import usb.core
import usb.util


USB_TYPE_VENDOR = 0x40
FIRMWARE_LOAD = 0xA0
CPUCS_ADDR = 0xE600

PRELOAD_IDS = (
    (0x2139, 0x0001),
    (0x04B4, 0x6473),
)

POSTLOAD_IDS = (
    (0x2139, 0x0101),
    (0xBECA, 0x0101),
)


def default_firmware_path() -> Path:
    root = Path(__file__).resolve().parents[1]
    candidates = (
        root / "eigenapi" / "resources" / "firmware" / "ihx" / "pico.ihx",
        root.parent / "EigenD" / "resources" / "pico.ihx",
    )
    for candidate in candidates:
        if candidate.exists():
            return candidate
    raise FileNotFoundError("could not find pico.ihx in EigenLite or EigenD")


def iter_devices(id_pairs: tuple[tuple[int, int], ...]):
    for vendor_id, product_id in id_pairs:
        for dev in usb.core.find(find_all=True, idVendor=vendor_id, idProduct=product_id) or ():
            yield dev


def device_label(dev: usb.core.Device) -> str:
    return f"VID_{dev.idVendor:04X}&PID_{dev.idProduct:04X} bus={getattr(dev, 'bus', '?')} addr={getattr(dev, 'address', '?')}"


def parse_ihx_line(line: str) -> tuple[int, int, bytes]:
    if not line.startswith(":"):
        raise ValueError(f"invalid IHX line: {line!r}")

    line = line.strip()
    byte_count = int(line[1:3], 16)
    address = int(line[3:7], 16)
    record_type = int(line[7:9], 16)

    if record_type == 0x01:
        return record_type, address, b""
    if record_type != 0x00:
        raise ValueError(f"unsupported IHX record type 0x{record_type:02X}")

    data_end = 9 + byte_count * 2
    data = bytes.fromhex(line[9:data_end])
    if len(data) != byte_count:
        raise ValueError("IHX byte count mismatch")
    return record_type, address, data


def write_control(dev: usb.core.Device, address: int, data: bytes) -> None:
    written = dev.ctrl_transfer(USB_TYPE_VENDOR, FIRMWARE_LOAD, address, 0, data, timeout=10000)
    if written != len(data):
        raise IOError(f"short control transfer: expected {len(data)} wrote {written}")


def set_cpucs(dev: usb.core.Device, value: int) -> None:
    write_control(dev, CPUCS_ADDR, bytes((value,)))


def claim_if_needed(dev: usb.core.Device) -> None:
    try:
        if dev.is_kernel_driver_active(0):
            dev.detach_kernel_driver(0)
    except (NotImplementedError, usb.core.USBError):
        pass

    try:
        usb.util.claim_interface(dev, 0)
    except usb.core.USBError:
        # Some bootloader states allow control transfers without a claim.
        pass


def release_if_needed(dev: usb.core.Device) -> None:
    try:
        usb.util.release_interface(dev, 0)
    except usb.core.USBError:
        pass
    usb.util.dispose_resources(dev)


def load_firmware(dev: usb.core.Device, firmware_path: Path) -> None:
    claim_if_needed(dev)
    try:
        set_cpucs(dev, 0x01)
        with firmware_path.open("r", encoding="ascii") as handle:
            for lineno, raw_line in enumerate(handle, start=1):
                line = raw_line.strip()
                if not line:
                    continue
                record_type, address, data = parse_ihx_line(line)
                if record_type == 0x01:
                    break
                write_control(dev, address, data)
        set_cpucs(dev, 0x00)
    finally:
        release_if_needed(dev)


def wait_for_postload(timeout_s: float) -> usb.core.Device | None:
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        for dev in iter_devices(POSTLOAD_IDS):
            return dev
        time.sleep(0.5)
    return None


def choose_device(serial: str | None) -> usb.core.Device:
    matches = list(iter_devices(PRELOAD_IDS))
    if not matches:
        raise RuntimeError("no Pico pre-load device found")

    if serial is None:
        if len(matches) > 1:
            labels = ", ".join(device_label(dev) for dev in matches)
            raise RuntimeError(f"multiple pre-load devices found; use --serial. Found: {labels}")
        return matches[0]

    for dev in matches:
        try:
            dev_serial = usb.util.get_string(dev, dev.iSerialNumber)
        except usb.core.USBError:
            dev_serial = None
        if dev_serial == serial:
            return dev
    raise RuntimeError(f"no pre-load device found with serial {serial!r}")


def cmd_list() -> int:
    found = False
    print("Pre-load devices:")
    for dev in iter_devices(PRELOAD_IDS):
        found = True
        print(f"  {device_label(dev)}")
    print("Post-load devices:")
    for dev in iter_devices(POSTLOAD_IDS):
        found = True
        print(f"  {device_label(dev)}")
    if not found:
        print("  none")
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Load Eigenharp Pico firmware over USB.")
    parser.add_argument("--firmware", type=Path, default=None, help="path to pico.ihx")
    parser.add_argument("--serial", default=None, help="optional USB serial to select a specific pre-load device")
    parser.add_argument("--list", action="store_true", help="list matching USB devices and exit")
    parser.add_argument("--wait", type=float, default=10.0, help="seconds to wait for post-load device to appear")
    args = parser.parse_args(argv)

    if args.list:
        return cmd_list()

    firmware_path = args.firmware or default_firmware_path()
    if not firmware_path.exists():
        raise FileNotFoundError(f"firmware file not found: {firmware_path}")

    print(f"Using firmware: {firmware_path}")
    dev = choose_device(args.serial)
    print(f"Loading firmware to {device_label(dev)}")
    load_firmware(dev, firmware_path)
    print("Firmware upload complete; waiting for re-enumeration...")

    postload = wait_for_postload(args.wait)
    if postload is None:
        print("Firmware upload completed, but no post-load device appeared within the timeout.", file=sys.stderr)
        return 2

    print(f"Device re-enumerated as {device_label(postload)}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:  # pragma: no cover - CLI error path
        print(f"ERROR: {exc}", file=sys.stderr)
        raise SystemExit(1)
