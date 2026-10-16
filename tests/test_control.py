import contextlib
import io
from pathlib import Path
import struct
import subprocess
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import compare_images
import configure
import firmware
import performance_suite
import receive_image
import run_experiment


class Control(unittest.TestCase):
    def test_profiles_preserve_defaults_and_interval(self):
        old = run_experiment.arguments(["--mode", "image"])
        self.assertEqual((old["control_profile"], old["tx_interval_us"]), ("baseline", 1500))
        for profile in configure.CONTROL_PROFILES:
            config = run_experiment.arguments(["--mode", "image", "--interval-us", "1100",
                                               "--control-profile", profile])
            self.assertEqual((config["control_profile"], config["tx_interval_us"]), (profile, 1100))
        for profile in ("balanced", "aggressive"):
            self.assertEqual(run_experiment.arguments(["--mode", "image", "--control-profile", profile])
                             ["tx_interval_us"], 1100)
        for args in (["--control-profile", "balanced"], ["--mode", "image", "--control-profile", "unknown"]):
            with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
                run_experiment.arguments(args)
        with self.assertRaises(ValueError): configure.configuration({"CONTROL_PROFILE": "balanced"})

    def test_guard_boundaries_and_profile_values(self):
        for guard in ("IMAGE_REQUEST_GUARD_US", "IMAGE_REPLY_GUARD_US"):
            for value in (500, 999, 1000, 1500, 3000, 3001):
                result = subprocess.run(["cc", "-std=c11", "-Icommon", f"-D{guard}={value}",
                                         "-x", "c", "-fsyntax-only", "-"], cwd=ROOT,
                                        input='#include "image_control.h"\n', text=True, capture_output=True)
                self.assertEqual(result.returncode == 0, 1000 <= value <= 3000, result.stderr)
        for profile, request, reply in ((0, 3000, 3000), (1, 3000, 1500), (2, 1500, 1000)):
            source = ('#include "image_control.h"\n'
                      f'_Static_assert(IMAGE_REQUEST_GUARD_US == {request}, "request");\n'
                      f'_Static_assert(IMAGE_REPLY_GUARD_US == {reply}, "reply");\n')
            result = subprocess.run(["cc", "-std=c11", "-Icommon", f"-DIMAGE_CONTROL_PROFILE={profile}",
                                     "-x", "c", "-fsyntax-only", "-"], cwd=ROOT,
                                    input=source, text=True, capture_output=True)
            self.assertEqual(result.returncode, 0, result.stderr)

    def test_embedded_profile_roundtrip_and_reject_invalid(self):
        for index, name in enumerate(configure.CONTROL_PROFILES):
            raw = firmware.CONTROL_CONFIG_MAGIC + struct.pack("<8I", 2440, 1, 434, 0, 240, 1100, 7, index)
            self.assertEqual(firmware.image_configuration(raw)["control_profile"], name)
            with self.assertRaises(ValueError): firmware.image_configuration(raw[:-1])
            with self.assertRaises(ValueError): firmware.image_configuration(raw + firmware.IMAGE_CONFIG_MAGIC)
        with self.assertRaises(ValueError):
            firmware.image_configuration(firmware.CONTROL_CONFIG_MAGIC + struct.pack("<8I", *([3] * 8)))

    def test_ready_profile_and_result_lengths(self):
        for profile, name in enumerate(configure.CONTROL_PROFILES):
            body = struct.pack("<14I", 2440, 1, 434, 0, 240, 1100, 7, 1, 0, 0, 0, 0, 0, profile)
            records = receive_image.Parser().feed(receive_image.encode_record(receive_image.READY, 7, body))
            row = receive_image.Receiver(7).accept(*records[0])
            self.assertEqual(row["control_profile"], name)
        for length in (96, 180, 388):
            self.assertEqual(len(receive_image.Parser().feed(
                receive_image.encode_record(receive_image.RESULT, 7, bytes(length)))), 1)
        for length in (184, 384, 392):
            with self.assertRaises(ValueError):
                receive_image.Parser().feed(receive_image.encode_record(receive_image.RESULT, 7, bytes(length)))

    def test_success_latency_excludes_failure_and_groups_profiles(self):
        rows = [dict(control_profile="balanced", interval_us=1100, frequency_mhz=2440,
                     eligible=ok, tx_radio_us=latency, host_delivery_seconds=latency / 1e6)
                for ok, latency in ((True, 500000), (True, 600000), (False, 100))]
        rows.append(dict(rows[0], control_profile="baseline", tx_radio_us=700000))
        groups = compare_images.summarize(rows)
        balanced = next(g for g in groups if g["control_profile"] == "balanced")
        self.assertEqual(balanced["tx_radio_us"], dict(n=2, mean=550000, median=550000, worst=600000))
        self.assertEqual((balanced["failures"], balanced["failure_rate"]), (1, 1/3))
        failed = compare_images.summarize([dict(rows[0], eligible=False)])[0]
        self.assertNotIn("tx_radio_us", failed)

    def test_control_build_plan(self):
        configs = dict(performance_suite.control_configurations())
        self.assertEqual(len(configs), 7)
        for profile in configure.CONTROL_PROFILES:
            self.assertEqual(configs[f"control-{profile}-1100"]["TX_INTERVAL_US"], 1100)
        self.assertEqual(configs["control-baseline-1500"]["TX_INTERVAL_US"], 1500)


if __name__ == "__main__":
    unittest.main()
