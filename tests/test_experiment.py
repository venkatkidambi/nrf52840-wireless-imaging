import csv
import io
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import configure
import monitor

class Configuration(unittest.TestCase):
    def test_defaults_and_supported_settings(self):
        self.assertEqual(configure.configuration({}), {
            "RADIO_FREQ_MHZ": 2402, "TX_INTERVAL_MS": 10, "TX_PACKET_COUNT": 10000})
        for freq in (2360, 2399, 2400, 2402, 2440, 2476, 2500):
            for interval in (1, 10, 20, 50, 60000):
                with self.subTest(freq=freq, interval=interval):
                    self.assertEqual(configure.configuration({"RADIO_FREQ_MHZ": str(freq),
                        "TX_INTERVAL_MS": str(interval)})["TX_INTERVAL_MS"], interval)
        self.assertEqual(configure.configuration({"TX_PACKET_COUNT": "0"})["TX_PACKET_COUNT"], 0)

    def test_invalid_settings(self):
        cases = {"RADIO_FREQ_MHZ": ("2359", "2501", "2402.5", "02402", "x", "2402;echo"),
                 "TX_INTERVAL_MS": ("0", "-1", "60001", "10.0", "", "1e1"),
                 "TX_PACKET_COUNT": ("-1", "4294967296", "0x100", "x")}
        for key, values in cases.items():
            for value in values:
                with self.subTest(key=key, value=value), self.assertRaises(ValueError):
                    configure.configuration({key: value})

    def test_make_rejects_invalid_configuration(self):
        for setting in ("RADIO_FREQ_MHZ=2359", "RADIO_FREQ_MHZ=2501",
                        "TX_INTERVAL_MS=0", "TX_INTERVAL_MS=60001", "TX_PACKET_COUNT=-1"):
            with self.subTest(setting=setting):
                result = subprocess.run(["make", "build/experiment_config.h", setting],
                                        cwd=ROOT, capture_output=True, text=True)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("must be a decimal integer", result.stderr)

    def test_header_changes_only_with_configuration(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "config.h"
            configure.write_config(path, {})
            before = path.stat().st_mtime_ns
            configure.write_config(path, {})
            self.assertEqual(path.stat().st_mtime_ns, before)
            configure.write_config(path, {"RADIO_FREQ_MHZ": "2476", "TX_INTERVAL_MS": "50"})
            self.assertIn("#define RADIO_FREQ_MHZ 2476", path.read_text())
            self.assertIn("#define TX_INTERVAL_MS 50", path.read_text())

    def test_changed_config_invalidates_generated_products_even_with_same_mtime(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            path = root / "experiment_config.h"
            configure.write_config(path, {})
            products = [root / "rx.elf", root / "tx.uf2", root / "tx.uf2.json",
                        root / "rx/rx/main.o", root / "rx/rx/main.d"]
            for product in products:
                product.parent.mkdir(parents=True, exist_ok=True)
                product.touch()
            keep = root / "rx/notes.txt"
            keep.write_text("not a generated product")
            configure.write_config(path, {}, invalidate=True)
            self.assertTrue(all(p.exists() for p in products))
            configure.write_config(path, {"TX_INTERVAL_MS": "20"}, invalidate=True)
            self.assertTrue(all(not p.exists() for p in products))
            self.assertTrue(keep.exists())

    def test_compiler_encoding_and_guards(self):
        cc = os.environ.get("HOST_CC", "cc")
        source = '#include "experiment.h"\n'
        with tempfile.TemporaryDirectory() as folder:
            for freq, base, low in ((2360, 2360, 1), (2399, 2360, 1), (2400, 2400, 0),
                                    (2402, 2400, 0), (2440, 2400, 0), (2476, 2400, 0),
                                    (2500, 2400, 0)):
                for interval in (10, 20, 50):
                    check = source + f'''
_Static_assert(RADIO_FREQ_OFFSET == {freq-base}, "offset");
_Static_assert(RADIO_FREQ_MAP_LOW == {low}, "map");
_Static_assert(TX_INTERVAL_US == {interval*1000}, "interval");
'''
                    result = subprocess.run([cc, "-std=c11", "-Werror", "-Icommon",
                        f"-DRADIO_FREQ_MHZ={freq}", f"-DTX_INTERVAL_MS={interval}",
                        "-x", "c", "-c", "-", "-o", f"{folder}/check.o"], input=check,
                        cwd=ROOT, capture_output=True, text=True)
                    self.assertEqual(result.returncode, 0, result.stderr)
            for setting in ("RADIO_FREQ_MHZ=2359", "RADIO_FREQ_MHZ=2501",
                            "TX_INTERVAL_MS=0", "TX_INTERVAL_MS=60001", "TX_PACKET_COUNT=-1"):
                result = subprocess.run([cc, "-std=c11", "-Icommon", f"-D{setting}",
                    "-x", "c", "-c", "-", "-o", f"{folder}/check.o"], input=source,
                    cwd=ROOT, capture_output=True, text=True)
                self.assertNotEqual(result.returncode, 0, setting)

def report(**overrides):
    values = dict.fromkeys(monitor.DIAGNOSTICS, 0)
    values.update(freq_mhz=2402, tx_interval_ms=10, packet_target=10000,
                  valid=99, gaps=1, observed_gaps=1, observed_expected=100,
                  gap_rate_valid=1, uptime_ms=1000, timing_n=100,
                  rearm_min_us=140, rearm_mean_us=150, rearm_max_us=300)
    values.update(overrides)
    return "RX " + " ".join(f"{k}={v}" for k, v in values.items()) + "\r\n"

class Logger(unittest.TestCase):
    def test_fragmented_csv_and_session_reset(self):
        stream = io.StringIO()
        log = monitor.ExperimentLog(stream, "channel-test")
        wire = ("banner\r\n" + report() + report(uptime_ms=2000) +
                report(uptime_ms=100, gap_rate_valid=0)).encode()
        for byte in wire:
            log.feed(bytes([byte]))
        rows = list(csv.DictReader(io.StringIO(stream.getvalue())))
        self.assertEqual(len(rows), 3)
        self.assertEqual([r["receiver_session"] for r in rows], ["0", "0", "1"])
        self.assertEqual(rows[0]["run_id"], "channel-test")
        self.assertEqual(float(rows[0]["sequence_gap_percent"]), 1)
        self.assertEqual(rows[0]["rearm_mean_us"], "150")
        self.assertTrue(rows[0]["host_timestamp"].endswith("+00:00"))
        self.assertEqual(rows[2]["sequence_gap_percent"], "")

    def test_malformed_truncated_and_long_lines(self):
        self.assertIsNone(monitor.parse_report("RX valid=100"))
        self.assertIsNone(monitor.parse_report(report().replace("gaps=1", "gaps=nope")))
        self.assertIsNone(monitor.parse_report(report().strip() + " valid=99"))
        stream = io.StringIO()
        log = monitor.ExperimentLog(stream, "recovery")
        log.feed(b"x" * 5000)
        log.feed(report().encode()) # discard tail of overlong line
        log.feed(b"\xff\nRX valid=1\n" + report().encode() + b"RX valid=2")
        self.assertEqual(len(list(csv.DictReader(io.StringIO(stream.getvalue())))), 1)

    def test_actual_formatter_schema_and_capacity(self):
        source = (ROOT / "rx/main.c").read_text()
        fields = re.findall(r'field\("(?:RX | )([a-z_]+)=",', source)
        self.assertEqual(set(fields), set(monitor.DIAGNOSTICS))
        # Even if every field were uint64, the line and USB FIFO must fit.
        worst = "RX " + " ".join(f"{k}={2**64-1}" for k in fields) + "\r\n"
        capacity = int(re.search(r'char line\[(\d+)\]', source)[1])
        fifo = int(re.search(r'CFG_TUD_CDC_TX_BUFSIZE (\d+)',
                            (ROOT / "platform/tusb_config.h").read_text())[1])
        self.assertLess(len(worst), capacity)
        self.assertGreaterEqual(fifo, capacity)

    def test_requires_label_and_refuses_overwrite_before_port_open(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "run.csv"
            path.write_text("keep me")
            command = [sys.executable, "scripts/monitor.py", "--port", "/nonexistent",
                       "--csv", str(path)]
            result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True)
            self.assertIn("--run-id is required", result.stderr)
            result = subprocess.run(command + ["--run-id", "test"], cwd=ROOT,
                                    capture_output=True, text=True)
            self.assertIn("File exists", result.stderr)
            self.assertEqual(path.read_text(), "keep me")
