#!/usr/bin/env python3
"""Exercise AArch32 UEFI boot on U-Boot's QEMU virt target."""

import argparse
import os
import selectors
import subprocess
import sys
import tempfile
import time
from pathlib import Path

BOOTARM = Path("EFI/BOOT/BOOTARM.EFI")
LAUNCH = b"UEFI: launching removable application"
RETURNED = b"UEFI: removable application returned"
FALLBACK = b"UEFI: running built-in probe"
PROBE = (
    b"Mocha UEFI probe (AArch32)",
    b"GetMemoryMap OK",
    # virt RAM is [0x40000000, 0x60000000); PRAM occupies its top 225 MiB.
    b"EFI reserved memory start=0x0000000051f00000, pages=0x000000000000e100",
    b"FDT table and header OK",
)
PASS = b"UEFI probe PASS"
BUILTIN = (FALLBACK, *PROBE, b"EFI boot volume unavailable", PASS)
EXTERNAL = (LAUNCH, *PROBE, b"EFI boot volume open OK", PASS, RETURNED)


def run_smoke(qemu, firmware, media, terminal_marker):
    command = [
        qemu, "-M", "virt", "-cpu", "cortex-a15", "-m", "512M",
        "-display", "none", "-monitor", "none", "-serial", "stdio",
        "-nic", "none", "-no-reboot", "-bios", str(firmware),
    ]
    if media is not None:
        path = str(media.resolve()).replace(",", ",,")
        command += [
            "-drive", f"if=none,id=media,format=raw,readonly=on,file=fat:{path}",
            "-device", "virtio-blk-device,drive=media",
        ]

    serial = bytearray()
    with subprocess.Popen(command, stdin=subprocess.DEVNULL,
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT) as process:
        deadline = time.monotonic() + 30
        try:
            with selectors.DefaultSelector() as selector:
                selector.register(process.stdout, selectors.EVENT_READ)
                while len(serial) < 1024 * 1024:
                    remaining = deadline - time.monotonic()
                    if remaining <= 0 or not selector.select(remaining):
                        break
                    chunk = os.read(process.stdout.fileno(), 4096)
                    if not chunk:
                        break
                    serial.extend(chunk)
                    if terminal_marker in serial or b"=> " in serial:
                        break
        finally:
            if process.poll() is None:
                process.kill()
            process.wait()
    return serial


def check_case(qemu, firmware, label, media, expected):
    print(f"QEMU smoke test: {label}", flush=True)
    serial = run_smoke(qemu, firmware, media, expected[-1])
    sys.stdout.buffer.write(serial)
    sys.stdout.buffer.write(b"\n")
    sys.stdout.buffer.flush()
    offset = 0
    for marker in expected:
        position = serial.find(marker, offset)
        if position == -1:
            raise SystemExit(f"{label}: missing ordered marker {marker.decode('ascii')!r}")
        offset = position + len(marker)
    if serial.count(PASS) != 1 or any(
        marker in serial and marker not in expected for marker in (LAUNCH, FALLBACK, RETURNED)
    ):
        raise SystemExit(f"{label}: unexpected extra boot path")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--qemu", default="qemu-system-arm")
    parser.add_argument("--firmware", type=Path, required=True)
    parser.add_argument("--media", type=Path, help="directory with the ARM32 removable boot file")
    args = parser.parse_args()
    if args.media is not None and not (args.media / BOOTARM).is_file():
        parser.error(f"removable boot file missing: {args.media / BOOTARM}")

    check_case(args.qemu, args.firmware, "no removable disk", None, BUILTIN)
    if args.media is not None:
        check_case(args.qemu, args.firmware, "valid ARM32 EFI application", args.media, EXTERNAL)
        with tempfile.TemporaryDirectory(prefix="mocha-uefi-") as directory:
            media = Path(directory)
            invalid = media / BOOTARM
            invalid.parent.mkdir(parents=True)
            invalid.write_bytes(b"This is not an EFI executable.\n")
            check_case(args.qemu, args.firmware, "invalid EFI image", media, (LAUNCH, *BUILTIN))


if __name__ == "__main__":
    main()
