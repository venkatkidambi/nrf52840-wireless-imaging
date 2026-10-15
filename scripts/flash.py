#!/usr/bin/env python3
"""Discover and explicitly select ONE mounted XIAO UF2 volume. Never erase."""
import argparse
import os
from pathlib import Path
import struct
import subprocess
from firmware import (APP_START, APP_END, BOARD_FAMILY, FAMILY, block_header,
                      require, validated_artifact)

EXPECTED_BOARD = "Seeed_XIAO_nRF52840_Sense"

def info(volume):
    text = (volume / "INFO_UF2.TXT").read_text()
    fields = dict(line.split(":", 1) for line in text.splitlines() if ":" in line)
    return text.splitlines()[0], {k.strip(): v.strip() for k, v in fields.items()}

def discover(root=Path("/Volumes")):
    return sorted(p for p in root.iterdir() if (p / "INFO_UF2.TXT").is_file()) if root.exists() else []

def check_current(volume):
    """Small read-only probes, not a full flash dump or a persistent board ID."""
    version, fields = info(volume)
    require(fields.get("Board-ID") == EXPECTED_BOARD, "Unexpected Board-ID")
    require(fields.get("Model") == "Seeed XIAO nRF52840", "Unexpected board model")
    require(fields.get("SoftDevice") == "S140 7.3.0", "Unverified SoftDevice version")
    require(version.startswith("UF2 Bootloader 0.6.2-12-g459adc9-dirty "),
            "Unverified bootloader build; re-audit layout before enabling flashing")
    path = volume / "CURRENT.UF2"
    size = path.stat().st_size
    require(size >= 512 and size % 512 == 0, "Invalid CURRENT.UF2 size")
    with path.open("rb") as f:
        first = block_header(f.read(512))
        _, _, flags, start, payload, number, count, family = first
        require(flags == 0x2000 and family in (FAMILY, BOARD_FAMILY), "Unexpected current UF2 family")
        require(start == 0x1000 and payload == 256 and number == 0 and count == size // 512,
                "Unverified CURRENT.UF2 mapping")
        f.seek(-512, 2)
        last = block_header(f.read(512))
        require(last == (first[0], first[1], flags, APP_END - 256, 256, count - 1, count, family)
                and start + count * 256 == APP_END, "Unverified reserved-flash boundary")
        f.seek((0x3000 - start) // 256 * 512)
        b = f.read(512)
        h = block_header(b)
        require(h == (first[0], first[1], flags, 0x3000, 256, 32, count, family),
                "Missing SoftDevice information block")
        magic, sd_size = struct.unpack_from("<II", b, 36)
        sd_id, sd_version = struct.unpack_from("<II", b, 48)
        require(magic == 0x51B1E5DB and sd_size == APP_START,
                "Installed SoftDevice does not confirm application start")
        require(sd_id == 140 and sd_version == 7003000, "SoftDevice metadata version mismatch")
    return version, fields

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--list", action="store_true")
    parser.add_argument("--role", choices=("tx", "rx"))
    parser.add_argument("--volume", type=Path)
    parser.add_argument("--firmware", type=Path)
    parser.add_argument("--dry-run", action="store_true", help="validate target/image without writing")
    parser.add_argument("--usb-entry-id", type=int, help="Runner's verified current USB attachment")
    args = parser.parse_args()
    if args.list:
        for p in discover():
            try:
                version, fields = info(p)
                print(f"{p}\n  {fields.get('Board-ID', '?')} / {fields.get('SoftDevice', '?')}\n  {version}")
            except (OSError, ValueError) as exc:
                print(f"{p}: cannot inspect: {exc}")
        return
    if args.volume is None or args.role is None or args.firmware is None:
        parser.error("--volume, --role and --firmware are required; use --list to discover boards")
    volume = args.volume.absolute()
    try:
        require(volume.parent == Path("/Volumes") and not volume.is_symlink() and os.path.ismount(volume),
                "Select an actual mounted volume directly under /Volumes")
        data = validated_artifact(args.firmware, args.role)
        version, fields = check_current(volume)
        print(f"Target: {volume}\nBoard: {fields['Board-ID']}\n{version}\n"
              f"Firmware role: {args.role.upper()}\nImage: {args.firmware.resolve()} ({len(data)} UF2 bytes)\n"
              f"Verified application region: 0x{APP_START:X}..0x{APP_END:X} (end exclusive)", flush=True)
        if args.dry_run:
            print("Dry run: no bytes written.")
            return
        # Re-check after inspection. Names are never persisted or mapped to roles.
        require(os.path.ismount(volume), "Target was unmounted")
        require(info(volume) == (version, fields), "Target metadata changed")
        if args.usb_entry_id is not None:
            from usb_identity import verify_volume
            verify_volume(volume, args.usb_entry_id)
        destination = volume / f"IMAGING-{args.role.upper()}.UF2"
        require(not destination.is_symlink(), "Destination must not be a symlink")
        with destination.open("wb") as out:
            out.write(data)
            out.flush()
            os.fsync(out.fileno())
        print("UF2 copy completed. Bootloader reboot and application operation require hardware verification.")
    except (OSError, ValueError, KeyError, struct.error, subprocess.SubprocessError) as exc:
        parser.exit(1, f"Flash refused/failed: {exc}\n")

if __name__ == "__main__":
    main()
