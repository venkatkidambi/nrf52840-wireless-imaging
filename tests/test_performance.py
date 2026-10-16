import contextlib
import csv
import io
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
import compare_images as compare
import receive_image as image
import run_experiment as runner
import performance_suite as suite


class Performance(unittest.TestCase):
    def test_build_suite_plan_never_builds_or_flashes(self):
        with tempfile.TemporaryDirectory() as tmp:
            output = Path(tmp) / "plan"
            with mock.patch.object(sys, "argv", ["performance_suite", "--output", str(output)]), \
                 mock.patch.object(suite.subprocess, "run") as command, contextlib.redirect_stdout(io.StringIO()):
                suite.main()
            command.assert_not_called()
            records = json.loads((output / "suite.json").read_text())
            self.assertEqual(len(records), 8)
            self.assertTrue(all(r["status"] == "planned" and r["command"][:2] == ["make", "build"] for r in records))

    def test_profiles_and_conflicting_cli(self):
        for name, interval in (("baseline", 1500), ("1200", 1200), ("1100", 1100), ("limit", 1044), ("max", 0)):
            config = runner.arguments(["--mode", "image", "--image-profile", name])
            self.assertEqual(config["tx_interval_us"], interval)
        for args in (["--image-profile", "max"], ["--mode", "image", "--interval-us", "1043"],
                     ["--mode", "image", "--image-profile", "1200", "--max-throughput"]):
            with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
                runner.arguments(args)

    def test_extended_result_and_legacy_lengths(self):
        for words in (24, 45):
            body = bytes(words * 4)
            parser = image.Parser()
            self.assertEqual(parser.feed(image.encode_record(image.RESULT, 1, body)), [(image.RESULT, 1, body)])
        with self.assertRaises(ValueError):
            image.Parser().feed(image.encode_record(image.RESULT, 1, bytes(44 * 4)))

    def test_comparison_keeps_failures_and_never_qualifies_partial_frames(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            for name in ("good", "failed", "unconfirmed", "partial", "mismatched", "malformed"):
                folder = root / name; folder.mkdir()
                (folder / "config.json").write_text(json.dumps({"mode": "image", "tx_interval_us": 1500, "run_tag": 7}))
                stats = {"status": 1, "received_packets": 434, "tx_initial_packets": 434,
                         "crc32": 123, "tx_radio_us": 735221, "tx_initial_us": 650606}
                if name == "partial": stats["received_packets"] = 433
                if name in ("failed", "partial"): stats["tx_radio_us"] = 100
                summary = {"status": "failed" if name == "failed" else "complete_reported",
                           "copy_status": {"tx": "unconfirmed" if name == "unconfirmed" else "confirmed"}}
                (folder / "summary.json").write_text(json.dumps(summary))
                (folder / "image.json").write_text(json.dumps({"status": "pass", "tx_received_complete_ack": True,
                    "bytes": 102400, "crc32": 123, "frame_id": 8 if name == "mismatched" else 7, "measurements": stats}))
            (root / "malformed/image.json").write_text("{")
            rows = compare.compare([root], root / "out.csv", root / "good")
            self.assertEqual(len(rows), 6)
            self.assertEqual(sum(r["eligible"] for r in rows), 1)
            self.assertEqual(next(r for r in rows if r["eligible"])["radio_delta_from_baseline_us"], 0)
            with (root / "out.csv").open() as stream:
                self.assertEqual(len(list(csv.DictReader(stream))), 6)
            with self.assertRaises(FileExistsError): compare.compare([root], root / "out.csv")
            with self.assertRaises(ValueError): compare.compare([root], root / "bad.csv", root / "partial")
