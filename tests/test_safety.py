import hashlib
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import firmware as fw
import flash

def image():
    data = bytearray(300)
    struct.pack_into("<64I", data, 0, fw.RAM_END, *([fw.APP_START + 257] * 63))
    return data

def current_volume(path):
    (path / "INFO_UF2.TXT").write_text(
        "UF2 Bootloader 0.6.2-12-g459adc9-dirty lib/nrfx (v2.0.0)\n"
        "Model: Seeed XIAO nRF52840\nBoard-ID: Seeed_XIAO_nRF52840_Sense\nSoftDevice: S140 7.3.0\n")
    count = (fw.APP_END - 0x1000) // 256
    with (path / "CURRENT.UF2").open("wb") as out:
        for i in range(count):
            block = bytearray(512)
            struct.pack_into("<8I", block, 0, fw.MAGIC0, fw.MAGIC1, 0x2000,
                             0x1000 + i * 256, 256, i, count, fw.BOARD_FAMILY)
            struct.pack_into("<I", block, 508, fw.MAGIC_END)
            if i == 32:
                struct.pack_into("<6I", block, 32, 0xffffff2c, 0x51b1e5db,
                                 fw.APP_START, 0xffff0123, 140, 7003000)
            out.write(block)

class Safety(unittest.TestCase):
    def test_roundtrip(self):
        raw = image()
        decoded = fw.decode_uf2(fw.encode_uf2(raw))
        self.assertEqual(decoded[:len(raw)], raw)
        self.assertEqual(decoded[len(raw):], b"\xff" * (512 - len(raw)))

    def test_unsafe_blocks(self):
        original = fw.encode_uf2(image())
        for offset, value in ((8, 0), (12, 0), (12, 0x1000), (12, fw.APP_END),
                              (12, 0x10001000), (16, 476), (20, 1), (24, 4),
                              (28, 0xd663823c), (508, 0), (512 + 12, fw.APP_START)):
            with self.subTest(offset=offset, value=value):
                data = bytearray(original)
                struct.pack_into("<I", data, offset, value)
                with self.assertRaises(ValueError):
                    fw.decode_uf2(data)
        for data in (b"", original[:-1], original[:512], original + original):
            with self.assertRaises(ValueError):
                fw.decode_uf2(data)

    def test_invalid_vectors_and_oversize(self):
        for offset, value in ((0, 0x20000000), (0, fw.RAM_END - 1),
                              (4, 0x1001), (4, fw.APP_START + 256),
                              (4, fw.APP_END + 1), (55 * 4, 0)):
            data = image()
            struct.pack_into("<I", data, offset, value)
            with self.assertRaises(ValueError):
                fw.encode_uf2(data)
        with self.assertRaises(ValueError):
            fw.encode_uf2(image() + bytes(fw.APP_END - fw.APP_START))

    def test_manifest_role_and_hash(self):
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / "tx.uf2"
            payload = fw.encode_uf2(image())
            path.write_bytes(payload)
            metadata = {"role": "tx", "app_start": fw.APP_START, "app_end": fw.APP_END,
                        "sha256": hashlib.sha256(payload).hexdigest()}
            sidecar = Path(str(path) + ".json")
            sidecar.write_text(json.dumps(metadata))
            self.assertEqual(fw.validated_artifact(path, "tx"), payload)
            with self.assertRaises(ValueError):
                fw.validated_artifact(path, "rx")
            metadata["sha256"] = "0" * 64
            sidecar.write_text(json.dumps(metadata))
            with self.assertRaises(ValueError):
                fw.validated_artifact(path, "tx")

    def test_board_layout_and_changed_softdevice(self):
        with tempfile.TemporaryDirectory() as d:
            path = Path(d)
            current_volume(path)
            flash.check_current(path)
            with (path / "CURRENT.UF2").open("r+b") as f:
                f.seek(32 * 512 + 40)
                f.write(struct.pack("<I", 0x26000))
            with self.assertRaises(ValueError):
                flash.check_current(path)

    def test_changed_reservation_and_wrong_board(self):
        with tempfile.TemporaryDirectory() as d:
            path = Path(d)
            current_volume(path)
            with (path / "CURRENT.UF2").open("r+b") as f:
                f.seek(-512 + 12, 2)
                f.write(struct.pack("<I", 0xf3f00))
            with self.assertRaises(ValueError):
                flash.check_current(path)
            info = path / "INFO_UF2.TXT"
            info.write_text(info.read_text().replace("Seeed_XIAO_nRF52840_Sense", "wrong"))
            with self.assertRaises(ValueError):
                flash.check_current(path)

    def test_explicit_selection_required(self):
        result = subprocess.run([sys.executable, str(ROOT / "scripts/flash.py"),
                                 "--role", "tx", "--firmware", "build/tx.uf2", "--dry-run"],
                                capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("--volume", result.stderr)

    def test_built_images_if_present(self):
        for role in ("tx", "rx"):
            elf = ROOT / "build" / f"{role}.elf"
            if not elf.exists():
                self.skipTest("Cross-built images not present")
            expected, _ = fw.check_elf(elf)
            self.assertEqual(expected, elf.with_suffix(".bin").read_bytes())
            uf2 = fw.validated_artifact(elf.with_suffix(".uf2"), role)
            self.assertEqual(fw.decode_uf2(uf2)[:len(expected)], expected)

if __name__ == "__main__":
    unittest.main()
