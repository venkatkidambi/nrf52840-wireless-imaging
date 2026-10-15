#!/usr/bin/env python3
"""Shared strict image validation and UF2 conversion (Python standard library)."""
import hashlib
import json
from pathlib import Path
import struct

APP_START, APP_END = 0x27000, 0xEA000
RAM_START, RAM_END, STACK_SIZE = 0x20008000, 0x20040000, 8192
FAMILY = 0xADA52840
BOARD_FAMILY = 0x28860045
MAGIC0, MAGIC1, MAGIC_END = 0x0A324655, 0x9E5D5157, 0x0AB16F30
CONFIG_MAGIC = b"IMAGING-CONFIG1\0"

def image_configuration(binary):
    require(binary.count(CONFIG_MAGIC) == 1, "Missing or ambiguous embedded experiment configuration")
    offset = binary.index(CONFIG_MAGIC) + len(CONFIG_MAGIC)
    require(offset + 16 <= len(binary), "Truncated embedded experiment configuration")
    values = struct.unpack_from("<4I", binary, offset)
    return dict(zip(("freq_mhz", "tx_interval_ms", "packet_target", "tx_power_dbm"), values))

def require(condition, message):
    if not condition:
        raise ValueError(message)

def vectors(data, end):
    require(len(data) >= 256, "Missing complete vector table")
    sp, reset = struct.unpack_from("<II", data)
    require(sp == RAM_END and sp % 8 == 0, "Unexpected initial stack pointer")
    require(reset & 1 and APP_START + 256 <= (reset & ~1) < end,
            "Reset vector outside image or missing Thumb bit")
    for handler in struct.unpack_from("<63I", data, 4):
        require(handler & 1 and APP_START + 256 <= (handler & ~1) < end,
                "Interrupt vector outside application")

def check_elf(path):
    data = Path(path).read_bytes()
    require(data[:7] == b"\x7fELF\x01\x01\x01", "Expected little-endian ELF32")
    h = struct.unpack_from("<HHIIIIIHHHHHH", data, 16)
    etype, machine, _, entry, phoff, _, _, _, phsize, phnum, _, _, _ = h
    require(etype == 2 and machine == 40 and phsize == 32, "Expected ARM executable")
    flash_ranges = []
    ram_high = RAM_START
    for i in range(phnum):
        kind, off, va, pa, filesz, memsz, _, _ = struct.unpack_from("<8I", data, phoff + phsize * i)
        if kind != 1:
            continue
        require(filesz <= memsz and off + filesz <= len(data), "Malformed ELF segment")
        if filesz:
            require(APP_START <= pa < pa + filesz <= APP_END,
                    "ELF load bytes overlap reserved flash/UICR")
            flash_ranges.append((pa, data[off:off + filesz]))
        if memsz:
            if RAM_START <= va < RAM_END:
                require(va + memsz <= RAM_END - STACK_SIZE, "ELF RAM overlaps reserved stack")
                ram_high = max(ram_high, va + memsz)
            else:
                require(APP_START <= va < va + memsz <= APP_END, "ELF segment outside application")
    require(flash_ranges, "No loadable flash segments")
    flash_ranges.sort()
    require(flash_ranges[0][0] == APP_START, "ELF does not start at application base")
    end = max(addr + len(chunk) for addr, chunk in flash_ranges)
    binary = bytearray(end - APP_START)
    previous_end = APP_START
    for addr, chunk in flash_ranges:
        require(addr >= previous_end, "Overlapping ELF segments")
        binary[addr - APP_START:addr - APP_START + len(chunk)] = chunk
        previous_end = addr + len(chunk)
    vectors(binary, end)
    require(entry == struct.unpack_from("<I", binary, 4)[0], "ELF entry differs from reset vector")
    return bytes(binary), ram_high - RAM_START

def encode_uf2(binary):
    require(APP_START + len(binary) <= APP_END, "Image too large")
    vectors(binary, APP_START + len(binary))
    count = (len(binary) + 255) // 256
    result = bytearray()
    for i in range(count):
        block = bytearray(512)
        struct.pack_into("<8I", block, 0, MAGIC0, MAGIC1, 0x2000,
                         APP_START + i * 256, 256, i, count, FAMILY)
        block[32:288] = binary[i * 256:(i + 1) * 256].ljust(256, b"\xff")
        struct.pack_into("<I", block, 508, MAGIC_END)
        result.extend(block)
    return bytes(result)

def block_header(block):
    require(len(block) == 512, "Truncated UF2 block")
    header = struct.unpack_from("<8I", block)
    require(header[:2] == (MAGIC0, MAGIC1) and struct.unpack_from("<I", block, 508)[0] == MAGIC_END,
            "Invalid UF2 magic")
    return header

def decode_uf2(data):
    require(data and len(data) % 512 == 0, "UF2 length is not a nonempty block multiple")
    count = len(data) // 512
    result = bytearray()
    for i in range(count):
        b = data[i * 512:(i + 1) * 512]
        _, _, flags, addr, size, number, total, family = block_header(b)
        require(flags == 0x2000 and family == FAMILY, "Not an nRF52840 application UF2")
        require(size == 256 and number == i and total == count, "Invalid UF2 block ordering/count/size")
        require(addr == APP_START + i * 256 and addr + size <= APP_END,
                "UF2 address outside safe contiguous application range")
        result.extend(b[32:288])
    vectors(result, APP_START + len(result))
    return bytes(result)

def create_artifacts(elf, binary_path, uf2_path, role):
    expected, ram_used = check_elf(elf)
    binary = Path(binary_path).read_bytes()
    require(binary == expected, "objcopy binary differs from verified ELF load segments")
    payload = encode_uf2(binary)
    decode_uf2(payload)
    Path(uf2_path).write_bytes(payload)
    manifest = {"role": role, "sha256": hashlib.sha256(payload).hexdigest(),
                "configuration": image_configuration(binary),
                "app_start": APP_START, "app_end": APP_END, "flash_bytes": len(binary),
                "static_ram_bytes": ram_used, "stack_bytes": STACK_SIZE}
    Path(str(uf2_path) + ".json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(f"{role.upper()}: flash [0x{APP_START:05X}, 0x{APP_START + len(binary):05X}), "
          f"{len(binary)} bytes; static RAM {ram_used}; stack {STACK_SIZE}; layout OK")

def validated_artifact(path, role):
    data = Path(path).read_bytes()
    decode_uf2(data)
    manifest = json.loads(Path(str(path) + ".json").read_text())
    require(manifest["role"] == role, "Firmware role does not match requested board role")
    require(manifest["app_start"] == APP_START and manifest["app_end"] == APP_END,
            "Manifest layout mismatch")
    require(manifest["sha256"] == hashlib.sha256(data).hexdigest(), "Firmware hash mismatch")
    return data

if __name__ == "__main__":
    import argparse
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--elf", required=True)
    parser.add_argument("--bin", required=True)
    parser.add_argument("--uf2", required=True)
    parser.add_argument("--role", choices=("tx", "rx"), required=True)
    args = parser.parse_args()
    try:
        create_artifacts(args.elf, args.bin, args.uf2, args.role)
    except (ValueError, OSError, struct.error) as exc:
        parser.exit(1, f"Image validation failed: {exc}\n")
