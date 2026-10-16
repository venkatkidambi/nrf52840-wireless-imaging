import hashlib
import errno
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

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
    def test_copy_regular_file_and_short_host_writes(self):
        with tempfile.TemporaryDirectory() as folder:
            destination = Path(folder) / "IMAGING-TX.UF2"
            flash.copy_uf2(destination, b"test-image")
            self.assertEqual(destination.read_bytes(), b"test-image")
        with patch.object(flash.os, "open", return_value=123), \
                patch.object(flash.os, "write", side_effect=[2, 3]) as write, \
                patch.object(flash.os, "fsync") as sync, patch.object(flash.os, "close") as close:
            flash.copy_uf2("/unused", b"12345")
        self.assertEqual(write.call_count, 2)
        sync.assert_called_once_with(123)
        close.assert_called_once_with(123)

    def test_only_full_write_late_io_errors_are_unconfirmed(self):
        for stage in ("fsync", "close"):
            with self.subTest(stage=stage), patch.object(flash.os, "open", return_value=123), \
                    patch.object(flash.os, "write", return_value=5), patch.object(flash.os, "fsync") as sync, \
                    patch.object(flash.os, "close") as close:
                (sync if stage == "fsync" else close).side_effect = OSError(errno.EIO, "device gone")
                with self.assertRaises(flash.CopyFailure) as error:
                    flash.copy_uf2("/unused", b"12345")
                self.assertTrue(error.exception.unconfirmed)
                self.assertIn(f"UF2 {stage}: 5/5", str(error.exception))
                close.assert_called_once()

    def test_early_write_error_is_failure_even_if_close_also_errors(self):
        with patch.object(flash.os, "open", return_value=123), \
                patch.object(flash.os, "write", side_effect=[2, OSError(errno.EIO, "write failed")]), \
                patch.object(flash.os, "fsync") as sync, \
                patch.object(flash.os, "close", side_effect=OSError(errno.EIO, "close failed")), \
                self.assertRaises(flash.CopyFailure) as error:
            flash.copy_uf2("/unused", b"x" * 1024)
        self.assertFalse(error.exception.unconfirmed)
        self.assertIn("write: 2/1024", str(error.exception))
        sync.assert_not_called()

    def test_single_failed_final_write_has_unknown_delivery_not_zero_delivery(self):
        # The observed 4096-byte syscall failed without an acknowledged count.
        error = flash.CopyFailure("write", 0, 4096, OSError(errno.EIO, "device gone"), attempted=4096)
        self.assertTrue(error.unconfirmed)
        self.assertIn("0/4096 bytes acknowledged", str(error))
        self.assertIn("submitted 4096", str(error))

    def test_final_uf2_block_error_is_unconfirmed_and_never_retried(self):
        data = bytes(range(256)) * 16
        with patch.object(flash.os, "open", return_value=123), \
                patch.object(flash.os, "write", side_effect=[512] * 7 + [OSError(errno.EIO, "device gone")]) as write, \
                patch.object(flash.os, "fsync") as sync, patch.object(flash.os, "close") as close, \
                self.assertRaises(flash.CopyFailure) as error:
            flash.copy_uf2("/unused", data)
        self.assertTrue(error.exception.unconfirmed)
        self.assertIn("write: 3584/4096", str(error.exception))
        self.assertIn("submitted 512", str(error.exception))
        self.assertEqual(write.call_count, 8)
        self.assertEqual(b"".join(bytes(call.args[1]) for call in write.call_args_list), data)
        sync.assert_not_called()
        close.assert_called_once()

    def test_early_block_io_error_is_not_reclassified_as_reboot(self):
        with patch.object(flash.os, "open", return_value=123), \
                patch.object(flash.os, "write", side_effect=OSError(errno.EIO, "device gone")) as write, \
                patch.object(flash.os, "close"), self.assertRaises(flash.CopyFailure) as error:
            flash.copy_uf2("/unused", b"x" * 4096)
        self.assertFalse(error.exception.unconfirmed)
        self.assertIn("write: 0/4096", str(error.exception))
        self.assertIn("submitted 512", str(error.exception))
        write.assert_called_once()

    def test_short_write_finishes_same_block_before_next_block(self):
        with patch.object(flash.os, "open", return_value=123), \
                patch.object(flash.os, "write", side_effect=[256, 256, 512]) as write, \
                patch.object(flash.os, "fsync"), patch.object(flash.os, "close"):
            flash.copy_uf2("/unused", b"x" * 1024)
        self.assertEqual([len(call.args[1]) for call in write.call_args_list], [512, 256, 512])

    def test_zero_write_and_unrelated_sync_errors_are_failures(self):
        for written, sync_error in ((0, None), (5, OSError(errno.ENOSPC, "disk full"))):
            with self.subTest(written=written), patch.object(flash.os, "open", return_value=123), \
                    patch.object(flash.os, "write", return_value=written), \
                    patch.object(flash.os, "fsync", side_effect=sync_error), patch.object(flash.os, "close"), \
                    self.assertRaises(flash.CopyFailure) as error:
                flash.copy_uf2("/unused", b"12345")
            self.assertFalse(error.exception.unconfirmed)

    def test_open_error_is_failure(self):
        with patch.object(flash.os, "open", side_effect=OSError(errno.EACCES, "denied")), \
                patch.object(flash.os, "close") as close, self.assertRaises(flash.CopyFailure) as error:
            flash.copy_uf2("/unused", b"12345")
        self.assertFalse(error.exception.unconfirmed)
        self.assertIn("open: 0/5", str(error.exception))
        close.assert_not_called()

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

    def test_built_wakeup_vectors_if_present(self):
        # Check linked vectors, not only C initializers: a missing handler would
        # route the first timer wakeup into the fault trap and lose USB again.
        for role in ("tx", "rx"):
            path = ROOT / "build" / f"{role}.elf"
            if not path.exists(): self.skipTest("Cross-built images not present")
            elf = path.read_bytes()
            header = struct.unpack_from("<HHIIIIIHHHHHH", elf, 16)
            sections = [struct.unpack_from("<10I", elf, header[5] + i * header[10])
                        for i in range(header[11])]
            symbols = {}
            for section in sections:
                if section[1] != 2: continue  # SHT_SYMTAB
                names = sections[section[6]]
                strings = elf[names[4]:names[4] + names[5]]
                for offset in range(section[4], section[4] + section[5], section[9]):
                    name, value, _, _, _, _ = struct.unpack_from("<IIIBBH", elf, offset)
                    symbols[strings[name:].split(b"\0", 1)[0].decode()] = value
            binary, _ = fw.check_elf(path)
            for index, handler in ((25, "TIMER1_IRQHandler"), (17, "RADIO_IRQHandler")):
                self.assertIn(handler, symbols)
                self.assertNotEqual(symbols[handler], symbols["Default_Handler"])
                self.assertEqual(struct.unpack_from("<I", binary, index * 4)[0], symbols[handler] | 1)
            self.assertNotIn("SysTick_Handler", symbols)

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
