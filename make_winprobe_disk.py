#!/usr/bin/env python3
"""Package an ARM32 EFI media directory as a reproducible GPT/FAT32 disk."""

import argparse
import gzip
import hashlib
import os
from pathlib import Path
import shutil
import stat
import struct
import subprocess
import tempfile
import uuid


MIB = 1024 * 1024
SECTOR = 512
ESP_START = MIB // SECTOR
ESP_TYPE = "C12A7328-F81F-11D2-BA4B-00A0C93EC93B"
BOOT_PATHS = ("efi/microsoft/boot/bootmgfw.efi", "efi/boot/bootarm.efi")
ARM_MACHINES = {0x01C0, 0x01C2, 0x01C4}  # ARM, Thumb, ARMNT


def is_arm_efi(path: Path) -> bool:
    with path.open("rb") as exe:
        dos = exe.read(64)
        if len(dos) != 64 or dos[:2] != b"MZ":
            return False
        pe_offset, = struct.unpack_from("<I", dos, 0x3C)
        if pe_offset < len(dos):
            return False
        exe.seek(pe_offset)
        coff = exe.read(24)
        if len(coff) != 24 or coff[:4] != b"PE\0\0":
            return False
        machine, sections, _, _, _, optional_size, flags = struct.unpack_from("<HHIIIHH", coff, 4)
        optional = exe.read(optional_size)
        if (machine not in ARM_MACHINES or not sections or not flags & 0x0002
                or len(optional) != optional_size or optional_size < 96):
            return False
        if struct.unpack_from("<H", optional, 0)[0] != 0x010B:  # PE32
            return False
        if struct.unpack_from("<H", optional, 68)[0] != 10:  # EFI application
            return False
        length = os.fstat(exe.fileno()).st_size
        for _ in range(sections):
            section = exe.read(40)
            if len(section) != 40:
                return False
            raw_size, raw_offset = struct.unpack_from("<II", section, 16)
            if raw_size and raw_offset + raw_size > length:
                return False
        return True


def fat_order(path: Path) -> tuple[bool, str]:
    # Reserve explicit DOS aliases before mtools generates aliases for long names.
    stem, _, ext = path.name.partition(".")
    return not ("~" in stem and len(stem) <= 8 and len(ext) <= 3), path.name


def scan_media(media: Path) -> tuple[list[tuple[Path, bool]], int]:
    if not media.is_dir():
        raise ValueError(f"media is not a directory: {media}")
    entries = {}
    directories = [media]
    allocated = SECTOR  # FAT32 root directory
    while directories:
        for entry in sorted(directories.pop().iterdir(), key=fat_order):
            rel = entry.relative_to(media)
            name = entry.name
            if (len(name) > 255 or name.endswith((" ", "."))
                    or any(c in '"*/:<>?\\|' or ord(c) < 32 or 0xD800 <= ord(c) <= 0xDFFF
                           or ord(c) > 0xFFFF for c in name)):
                raise ValueError(f"name cannot be copied unchanged by mtools: {rel}")
            key = rel.as_posix().casefold()
            if key in entries:
                raise ValueError(f"FAT names collide: {entries[key][0]} and {rel}")
            info = entry.lstat()
            is_dir = stat.S_ISDIR(info.st_mode)
            if is_dir:
                directories.append(entry)
                allocated += SECTOR
            elif stat.S_ISREG(info.st_mode):
                allocated += (info.st_size + SECTOR - 1) // SECTOR * SECTOR
            else:
                raise ValueError(f"media entries must be regular files or directories: {rel}")
            entries[key] = rel, is_dir

    boot_files = [entries[key] for key in BOOT_PATHS if key in entries]
    if not boot_files:
        raise ValueError("media needs EFI/Microsoft/Boot/bootmgfw.efi or EFI/BOOT/BOOTARM.EFI")
    for rel, is_dir in boot_files:
        if is_dir or not is_arm_efi(media / rel):
            raise ValueError(f"{rel} is not a valid PE32 ARM EFI application")
    return list(entries.values()), allocated


def executable(name: str, directory: Path | None = None) -> Path:
    search = str(directory.resolve()) if directory is not None else None
    path = shutil.which(name, path=search)
    if path is None:
        raise ValueError(f"could not find {name} in {search or 'PATH'}")
    # Keep the program name in argv[0] for mtools' symlinked commands.
    return Path(path).absolute()


def package(media: Path, output: Path, size_mib: int, mtools_dir: Path | None) -> None:
    if output.is_relative_to(media):
        raise ValueError("output must be outside the media directory")
    entries, allocated = scan_media(media)
    # Leave 1 MiB at either end for alignment and the two GPTs.
    esp_bytes = (size_mib - 2) * MIB
    if allocated >= esp_bytes:
        raise ValueError(f"media is too large for {size_mib} MiB. Increase --size-mib (max 256).")

    mkfs = executable("mkfs.fat")
    mcopy = executable("mcopy", mtools_dir)
    mmd = executable("mmd", mtools_dir)
    sfdisk = executable("sfdisk")
    env = {key: value for key, value in os.environ.items() if not key.startswith("MTOOLS_")}
    env.update(LC_ALL="C.UTF-8", TZ="UTC", SOURCE_DATE_EPOCH="315532800",
               MTOOLSRC=os.devnull, MTOOLS_NAME_NUMERIC_TAIL="1")

    with tempfile.TemporaryDirectory(prefix=".winprobe-", dir=output.parent) as directory:
        work = Path(directory)

        def run(*command: str | Path, data: str = "") -> None:
            result = subprocess.run(command, input=data, capture_output=True, cwd=work,
                                    env=env, encoding="utf-8", errors="replace")
            if result.returncode:
                detail = (result.stderr or result.stdout).strip()
                if "disk full" in detail.lower() or "no space" in detail.lower():
                    raise ValueError(f"media is too large for {size_mib} MiB. "
                                     "Increase --size-mib (max 256).")
                invocation = " ".join(str(arg) for arg in command)
                raise ValueError(f"{invocation} exited with status {result.returncode}"
                                 + (f": {detail}" if detail else ""))

        with (work / "esp.fat").open("xb") as esp:
            esp.truncate(esp_bytes)
        run(mkfs, "--invariant", "-F", "32", "-S", str(SECTOR), "-s", "1",
            "-h", str(ESP_START), "esp.fat")
        for rel, is_dir in entries:
            # mtools treats '[' as a wildcard in directory lookups.
            parent = "::/" + "".join(part.replace("[", "[[]") + "/" for part in rel.parts[:-1])
            if is_dir:
                run(mmd, "-i", "esp.fat", parent + rel.name)
            else:
                run(mcopy, "-i", "esp.fat", media / rel, parent)

        # Give each distinct volume reproducible disk and partition GUIDs.
        digest = hashlib.sha256()
        with (work / "esp.fat").open("rb") as esp:
            for chunk in iter(lambda: esp.read(MIB), b""):
                digest.update(chunk)
        disk_id = uuid.uuid5(uuid.NAMESPACE_URL, f"mocha-uefi:winprobe:{digest.hexdigest()}")
        esp_id = uuid.uuid5(disk_id, "esp")
        with (work / "disk.img").open("xb") as disk:
            disk.truncate(size_mib * MIB)
        run(sfdisk, "--quiet", "--no-reread", "--no-tell-kernel", "disk.img", data=(
            f"label: gpt\nlabel-id: {disk_id}\nfirst-lba: 34\n"
            f"start={ESP_START}, size={esp_bytes // SECTOR}, type={ESP_TYPE}, "
            f'uuid={esp_id}, name="EFI System Partition"\n'
        ))

        archive = work / "disk.img.gz"
        with archive.open("xb") as out, gzip.GzipFile(filename="", mode="wb", fileobj=out, mtime=0) as gz:
            with (work / "disk.img").open("rb") as disk, (work / "esp.fat").open("rb") as esp:
                gz.write(disk.read(MIB))
                shutil.copyfileobj(esp, gz, MIB)
                disk.seek(MIB + esp_bytes)
                shutil.copyfileobj(disk, gz, MIB)
        archive.replace(output)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, allow_abbrev=False)
    parser.add_argument("--media", type=Path, required=True, help="EFI media directory")
    parser.add_argument("--output", type=Path, required=True, help="output gzip-compressed disk image")
    parser.add_argument("--mtools-dir", type=Path, help="directory containing mcopy and mmd (default: PATH)")
    parser.add_argument("--size-mib", type=int, default=64, help="raw disk size in MiB (35–256, default: 64)")
    args = parser.parse_args()
    if not 35 <= args.size_mib <= 256:
        parser.error("--size-mib must be between 35 and 256 for FAT32")
    try:
        media = args.media.resolve(strict=True)
        output = args.output.parent.resolve(strict=True) / args.output.name
        package(media, output, args.size_mib, args.mtools_dir)
    except (OSError, ValueError) as exc:
        parser.error(str(exc))
    print(f"{output} ({args.size_mib} MiB raw)")


if __name__ == "__main__":
    main()
