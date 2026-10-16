import io
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest.mock import Mock, patch
import zlib

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import configure
import firmware
import receive_image as image
import run_experiment as run
from test_runner import RX, TX, APP, CONFIG
from test_safety import image as firmware_image

class ImageReceive(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.pixels = image.expected_image()
        cls.crc = zlib.crc32(cls.pixels)
        cls.metadata = struct.pack("<HHIHHII", 320, 320, 102400, 236, 434, cls.crc, 1)
        stats = dict.fromkeys(image.STAT_FIELDS, 0)
        stats.update(status=1, tx_initial_packets=434, tx_radio_us=700000, tx_initial_us=650000,
                     received_packets=434, crc32=cls.crc)
        cls.stats = stats
        cls.result = struct.pack("<24I", *stats.values())
        cls.records = [(image.START, 1, cls.metadata), (image.PIXELS, 1, cls.pixels), (image.RESULT, 1, cls.result)]
        cls.stream = b"".join(image.encode_record(*r) for r in cls.records)

    def receive(self, records=None):
        receiver = image.Receiver(1)
        for record in records or self.records:
            receiver.accept(*record)
        return receiver

    def test_native_firmware_stream_matches_python_pattern_and_framing(self):
        path = ROOT / "build/test_image_stream.bin"
        self.assertTrue(path.exists(), "Run make test to generate the actual C USB fixture")
        parser, receiver = image.Parser(), image.Receiver(1)
        data = path.read_bytes()
        for offset in range(0, len(data), 13):
            for record in parser.feed(data[offset:offset+13]):
                receiver.accept(*record)
        parser.finish(); receiver.finish()
        self.assertEqual(receiver.pixels, self.pixels)
        self.assertEqual(receiver.result["status"], "pass")

    def test_fragmentation_all_header_boundaries_and_single_bytes(self):
        for stride in (1, 7, 23, 24, 25, 4096, len(self.stream)):
            parser, records = image.Parser(), []
            for pos in range(0, len(self.stream), stride):
                records += parser.feed(self.stream[pos:pos+stride])
            parser.finish()
            self.assertEqual(records, self.records)

    def test_diagnostics_and_magic_inside_pixels_do_not_change_body_ownership(self):
        parser = image.Parser()
        records = parser.feed(b"RX diagnostic\r\n" + self.stream)
        self.assertEqual(records, self.records)
        body = bytearray(self.pixels); body[100:108] = image.MAGIC
        records = image.Parser().feed(image.encode_record(image.PIXELS, 1, body))
        self.assertEqual(records[0][2], body)

    def test_corrupted_headers_crc_and_oversized_lengths_fail(self):
        for offset, value in ((0, 0xff), (8, 2), (9, 99), (10, 1), (16, 0xff), (20, 0xff), (24, 0xff)):
            wire = bytearray(self.stream); wire[offset] = value
            with self.subTest(offset=offset), self.assertRaises(ValueError):
                image.Parser().feed(wire)
        wire = bytearray(self.stream); struct.pack_into("<I", wire, 16, 0xffffffff)
        with self.assertRaisesRegex(ValueError, "length"): image.Parser().feed(wire)
        with self.assertRaises(ValueError): image.Parser().feed(b"x" * 8193)

    def test_truncation_and_missing_completion_fail(self):
        for end in (1, 7, 23, 25, 44+24+100, len(self.stream)-1):
            parser = image.Parser(); parser.feed(self.stream[:end])
            with self.subTest(end=end), self.assertRaises(ValueError): parser.finish()
        r = self.receive(self.records[:2])
        with self.assertRaises(ValueError): r.finish()
        with self.assertRaises(ValueError): image.Receiver(1).finish()

    def test_order_duplicates_and_mixed_frames_fail(self):
        for records in ([self.records[1]], [self.records[2]], self.records[:1] * 2,
                        self.records[:2] + [self.records[1]],
                        [self.records[0], (image.PIXELS, 2, self.pixels)], self.records * 2):
            with self.subTest(kinds=[r[0] for r in records]), self.assertRaises(ValueError): self.receive(records)
        with self.assertRaises(ValueError): image.Receiver(2).accept(*self.records[0])

    def test_wrong_geometry_pattern_and_partial_final_image_fail(self):
        for offset in (0, 2, 4, 8, 10, 16):
            body = bytearray(self.metadata); body[offset] ^= 1
            with self.assertRaises(ValueError): self.receive([(image.START, 1, body)])
        with self.assertRaises(ValueError): self.receive([self.records[0], (image.PIXELS, 1, self.pixels[:-1])])

    def test_independent_pixel_comparison_even_when_crc_is_self_consistent(self):
        pixels = bytearray(self.pixels); pixels[-1] ^= 1
        metadata = bytearray(self.metadata); struct.pack_into("<I", metadata, 12, zlib.crc32(pixels))
        with self.assertRaisesRegex(ValueError, "Pixel mismatch"):
            self.receive([(image.START, 1, metadata), (image.PIXELS, 1, pixels)])
        with self.assertRaisesRegex(ValueError, "CRC32 mismatch"):
            self.receive([self.records[0], (image.PIXELS, 1, pixels)])

    def test_bad_result_and_explicit_failure_never_pass(self):
        for field, value in (("status", 0), ("received_packets", 433), ("crc32", 0),
                             ("tx_initial_packets", 433), ("retransmission_rounds", 17), ("tx_radio_us", 0)):
            stats = dict(self.stats, **{field: value})
            body = struct.pack("<24I", *stats.values())
            with self.subTest(field=field), self.assertRaises(ValueError):
                self.receive(self.records[:2] + [(image.RESULT, 1, body)])
        with self.assertRaisesRegex(ValueError, "failure 3"):
            self.receive([(image.ERROR, 1, struct.pack("<I", 3))])

    def test_pgm_exact_size_json_csv_and_no_overwrite(self):
        receiver = self.receive()
        with tempfile.TemporaryDirectory() as folder:
            image.save_image(Path(folder), receiver)
            self.assertEqual((Path(folder) / "image.pgm").read_bytes(), b"P5\n320 320\n255\n" + self.pixels)
            self.assertEqual(json.loads((Path(folder) / "image.json").read_text())["status"], "pass")
            self.assertTrue((Path(folder) / "image.csv").exists())
            with self.assertRaises(FileExistsError): image.save_image(Path(folder), receiver)

class ImageRunner(unittest.TestCase):
    def test_mode_defaults_and_benchmark_cli_unchanged(self):
        cfg = run.arguments(["--mode", "image", "--frequency", "2440"])
        self.assertEqual(cfg, dict(CONFIG, mode="image", control_profile="baseline", payload_bytes=240, packet_target=434,
                                   tx_interval_us=1500, tx_interval_ms=1))
        self.assertEqual(run.arguments(["--mode", "benchmark"]), CONFIG)
        cfg = run.arguments(["--frequency", "2440", "--payload", "240", "--interval-us", "1500", "--packets", "10000"])
        self.assertEqual(cfg, dict(CONFIG, payload_bytes=240, tx_interval_us=1500, tx_interval_ms=1))
        for opts in (["--payload", "32"], ["--packets", "10000"], ["--interval-us", "3001"]):
            with patch("sys.stderr", io.StringIO()), self.assertRaises(SystemExit):
                run.arguments(["--mode", "image", *opts])

    def test_embedded_image_mode_is_distinct_and_mode_change_invalidates(self):
        raw = firmware_image() + firmware.IMAGE_CONFIG_MAGIC + struct.pack("<7I", 2440, 1, 434, 0, 240, 1500, 1)
        expected = run.arguments(["--mode", "image"])
        expected.pop("control_profile")
        self.assertEqual(firmware.image_configuration(raw), expected)
        with self.assertRaises(ValueError): firmware.image_configuration(raw + firmware.CONFIG_MAGIC)
        with tempfile.TemporaryDirectory() as folder:
            header = Path(folder) / "config.h"
            config = {"IMAGE_MODE": "1", "APP_PAYLOAD_SIZE": "240", "TX_PACKET_COUNT": "434", "TX_INTERVAL_US": "1500"}
            configure.write_config(header, config)
            product = Path(folder) / "tx.elf"; product.touch()
            configure.write_config(header, dict(config, IMAGE_MODE="0"), invalidate=True)
            self.assertFalse(product.exists())

    def test_image_mode_uses_same_preflight_and_rx_first_order(self):
        config = run.arguments(["--mode", "image"])
        ready = dict(config, event="ready", radio_ready=1, active=0, crc_errors=0,
                     invalid=0, queue_drops=0, usb_errors=0)
        for unconfirmed in (None, "rx", "tx"):
            with tempfile.TemporaryDirectory() as folder:
                runner = run.ImageRunner(config, Path(folder)); events = []
                capture = Mock(); capture.rows.return_value = [ready]
                def flash(role, board, dry=False):
                    events.append((role, dry))
                    if not dry: runner.copy_status[role] = "unconfirmed" if role == unconfirmed else "confirmed"
                    if role == "tx" and not dry: self.assertGreaterEqual(capture.rows.call_count, 2)
                with patch.object(runner, "build", return_value={}), patch.object(runner, "flash", side_effect=flash), \
                     patch.object(run, "select_boards", return_value=({"rx": RX, "tx": TX}, {"rx": {}, "tx": {}})), \
                     patch.object(run, "prompt", return_value="FLASH"), patch.object(run, "wait_for_rx", return_value=APP), \
                     patch.object(run, "write_json"), patch.object(run.usb, "verify_volume"), \
                     patch.object(runner, "start_capture", return_value=capture), \
                     patch.object(runner, "collect", return_value="complete_reported"):
                    status = runner.run()
                self.assertEqual(events, [("rx", True), ("tx", True), ("rx", False), ("tx", False)])
                self.assertEqual(status, "copy_unconfirmed" if unconfirmed else "complete_reported")

    def test_readiness_rejects_old_or_mismatched_firmware(self):
        cfg = run.arguments(["--mode", "image"])
        good = dict(cfg, event="ready", radio_ready=1, active=0, crc_errors=0,
                    invalid=0, queue_drops=0, usb_errors=0)
        for key, value in (("mode", "benchmark"), ("run_tag", 2), ("radio_ready", 0),
                           ("active", 1), ("crc_errors", 1), ("event", "start")):
            runner = run.ImageRunner(cfg, Path("unused")); runner.capture = Mock()
            runner.capture.rows.return_value = [dict(good, **{key: value})]
            with self.assertRaises(run.RunError): runner.ready()

    def test_collect_requires_pass_and_same_frame(self):
        cfg = run.arguments(["--mode", "image"])
        for value in ({"status": "fail", "error": "incomplete"}, {"status": "pass", "frame_id": 2}):
            with tempfile.TemporaryDirectory() as folder:
                runner = run.ImageRunner(cfg, Path(folder)); runner.capture = Mock()
                runner.capture.rows.return_value = []
                (Path(folder) / "image.json").write_text(json.dumps(value))
                with self.assertRaises(run.RunError): runner.collect(run.time.monotonic())

class LinkedImage(unittest.TestCase):
    artifact_folder = ROOT / "build"

    def test_memory_dma_alignment_stack_no_heap_or_camera_symbols(self):
        for role in ("tx", "rx"):
            path = self.artifact_folder / f"{role}.elf"
            if not path.exists(): self.skipTest("Build firmware to audit linked symbols")
            elf = path.read_bytes()
            h = struct.unpack_from("<HHIIIIIHHHHHH", elf, 16)
            sections = [struct.unpack_from("<10I", elf, h[5] + i*h[10]) for i in range(h[11])]
            symbols = {}
            for section in sections:
                if section[1] != 2: continue
                names = sections[section[6]]; strings = elf[names[4]:names[4]+names[5]]
                for offset in range(section[4], section[4]+section[5], section[9]):
                    name, value, size, info, other, index = struct.unpack_from("<IIIBBH", elf, offset)
                    symbols[strings[name:].split(b"\0", 1)[0].decode()] = (value, size)
            binary, ram_used = firmware.check_elf(path)
            cfg = firmware.image_configuration(binary)
            self.assertEqual(symbols["__stack_limit"][0], 0x2003e000)
            self.assertEqual(symbols["__stack_top"][0], 0x20040000)
            self.assertLessEqual(symbols["__bss_end"][0], 0x2003e000)
            for name in symbols:
                self.assertNotIn(name, ("malloc", "calloc", "realloc", "free", "_sbrk", "_malloc_r", "nrf_nvmc_write_word"))
                self.assertNotIn("hm01b0", name.lower())
            dma_names = ["dma"]
            if cfg.get("mode") == "image":
                dma_names += ["tx_dma"]
                self.assertEqual(symbols["framebuffer"][1], image.SIZE)
                self.assertLess(ram_used + firmware.STACK_SIZE, 224 * 1024)
            else:
                self.assertNotIn("framebuffer", symbols)
                self.assertNotIn("image_transmit", symbols)
                if role == "tx": dma_names += ["packets"]
            for name in dma_names:
                addr, size = symbols[name]
                self.assertEqual(addr % 4, 0, name)
                self.assertGreaterEqual(addr, firmware.RAM_START)
                self.assertLessEqual(addr + size, symbols["__stack_limit"][0])
            packet_bytes = cfg["payload_bytes"] + 8
            self.assertEqual(symbols["dma"][1], 2 * ((packet_bytes + 3) // 4 * 4))

if __name__ == "__main__":
    unittest.main()
