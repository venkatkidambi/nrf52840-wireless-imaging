import csv
import io
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import benchmark_matrix as matrix
from benchmark_metrics import metrics
import configure
import run_experiment as run
from test_runner import CONFIG, report

class Benchmark(unittest.TestCase):
    def test_matrix_admission_and_exact_boundary(self):
        rows = matrix.combinations()
        self.assertEqual(len(rows), 24)
        self.assertEqual(sum(r["status"] != "rejected" for r in rows), 10)
        for payload, minimum in ((32, 212), (64, 340), (128, 596), (240, 1044), (247, 1072)):
            self.assertEqual(configure.minimum_interval(payload), minimum)
            for interval in (0, minimum, 60000000):
                configure.configuration({"APP_PAYLOAD_SIZE": str(payload), "TX_INTERVAL_US": str(interval)})
            with self.assertRaises(ValueError):
                configure.configuration({"APP_PAYLOAD_SIZE": str(payload), "TX_INTERVAL_US": str(minimum-1)})
        for payload in (31, 248, 255, 256):
            with self.assertRaises(ValueError): configure.configuration({"APP_PAYLOAD_SIZE": str(payload)})
        with self.assertRaises(ValueError): configure.configuration({"TX_INTERVAL_US": "250", "TX_INTERVAL_MS": "10"})

    def test_named_cli_and_legacy(self):
        c = run.arguments(["--frequency", "2440", "--payload", "32", "--interval-us", "250", "--packets", "100"])
        self.assertEqual((c["payload_bytes"], c["tx_interval_us"], c["packet_target"]), (32, 250, 100))
        self.assertEqual(run.arguments(["--payload", "240", "--max-throughput"])["tx_interval_us"], 0)
        self.assertEqual(run.arguments(["2440", "10", "100"])["tx_interval_us"], 10000)
        for args in (["--payload", "240", "--interval-us", "500"], ["2440", "10", "--max-throughput"],
                     ["--max-throughput", "--interval-us", "500"], ["2440", "--frequency", "2476"]):
            with patch("sys.stderr", io.StringIO()), self.assertRaises(SystemExit): run.arguments(args)

    def test_c_guards_match_python(self):
        with tempfile.TemporaryDirectory() as folder:
            for row in matrix.combinations():
                result = subprocess.run(["cc", "-std=c11", "-Icommon", "-x", "c", "-c", "-",
                    f"-DAPP_PAYLOAD_SIZE={row['payload_bytes']}", f"-DTX_INTERVAL_US={row['tx_interval_us']}",
                    "-o", str(Path(folder) / "check.o")], cwd=ROOT,
                    input='#include "experiment.h"\n_Static_assert(RADIO_PACKET_SIZE <= 255, "length");\n',
                    text=True, capture_output=True)
                self.assertEqual(result.returncode == 0, row["status"] != "rejected", result.stderr)

    def test_measured_goodput_excludes_headers_and_includes_tail_losses(self):
        config = dict(CONFIG, packet_target=100)
        row = report(packet_target=100, valid=90, have_sequence=1, tx_stats_mask=7, tx_starts=100,
                     tx_attempts=100, tx_completions=100, tx_complete=1,
                     tx_duration_us=100000, observation_us=80000)
        result = metrics(config, row)
        self.assertEqual(result["application_bytes_per_second"], 28800)
        self.assertEqual(result["packet_delivery_ratio"], 0.9)
        self.assertEqual(result["rx_observation_bytes_per_second"], 89 * 32 / .08)
        for mask in (0, 1, 3, 6):
            result = metrics(config, dict(row, tx_stats_mask=mask))
            self.assertIsNone(result["application_mbps"])
            self.assertIsNone(result["tx_attempts_measured"])
        self.assertIsNone(metrics(config, dict(row, backwards=1))["application_mbps"])
        self.assertEqual(metrics(config, dict(row, valid=0, have_sequence=0))["application_mbps"], 0)
        self.assertIsNone(metrics(config, dict(row, tx_completions=101))["application_mbps"])
        self.assertIsNone(metrics(config, dict(row, tx_complete=0))["application_mbps"])
        self.assertIsNone(metrics(config, dict(row, run_tag=999))["application_mbps"])

    def test_telemetry_completes_without_last_data_packet(self):
        state = run.Completion(CONFIG, 0)
        for now in (101, 102, 103):
            result = state.feed(report(valid=9000, have_sequence=1, last=9998,
                idle_ms=4000, uptime_ms=now*1000, tx_stats_mask=7, tx_complete=1,
                tx_attempts=10000, tx_starts=10000, tx_completions=10000, tx_duration_us=100000000), now)
        self.assertEqual(result, "complete_reported")
        with self.assertRaises(run.RunError):
            run.check_report(report(tx_stats_mask=7, tx_complete=1), CONFIG)
        with self.assertRaises(run.RunError):
            run.check_report(report(tx_stats_mask=1), CONFIG, clean=True)

    def test_matrix_plan_never_runs_build_or_flash(self):
        with tempfile.TemporaryDirectory() as folder, patch.object(matrix.subprocess, "run") as build, \
                patch.object(matrix, "run_interactive") as flash, patch("sys.stdout", io.StringIO()):
            dest = Path(folder) / "plan"
            matrix.main(["--output", str(dest)])
            build.assert_not_called(); flash.assert_not_called()
            rows = list(csv.DictReader(io.StringIO((dest / "matrix.csv").read_text())))
            self.assertEqual(len(rows), 24)
