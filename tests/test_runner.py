import io
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time
import unittest
from unittest.mock import Mock, patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import firmware
import monitor
import run_experiment as run
import usb_identity as usb
from test_safety import image

CONFIG = dict(freq_mhz=2440, tx_interval_ms=10, packet_target=10000, tx_power_dbm=0)
RX = usb.Device(10, "rx-boot", 0x2886, 0x45, 0x100000, "/Volumes/XIAO-SENSE 1")
TX = usb.Device(20, "tx-boot", 0x2886, 0x45, 0x200000, "/Volumes/XIAO-SENSE")
APP = usb.Device(30, "rx-app", 0xcafe, 0x4001, RX.location, "/dev/cu.usbmodem999")

def report(**changes):
    values = dict.fromkeys(monitor.DIAGNOSTICS, 0)
    values.update(CONFIG, radio_ready=1, uptime_ms=1000)
    values.update(changes)
    return values

class Arguments(unittest.TestCase):
    def test_missing_arguments_use_defaults(self):
        self.assertEqual(run.arguments([]), dict(CONFIG, freq_mhz=2402))
        self.assertEqual(run.arguments(["2440"]), CONFIG)
        self.assertEqual(run.arguments(["2440", "20"])["tx_interval_ms"], 20)

    def test_invalid_frequency_and_unbounded_count(self):
        for args in (["2359"], ["2501"], ["x"], ["2440", "0"],
                     ["2440", "10", "0"], ["2440", "10", "1", "extra"]):
            with self.subTest(args=args), patch("sys.stderr", io.StringIO()), self.assertRaises(SystemExit):
                run.arguments(args)

    def test_shell_entrypoint_rejects_invalid_args_without_building(self):
        result = subprocess.run([str(ROOT / "scripts/run_experiment.sh"), "invalid"],
                                cwd="/tmp", capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("RADIO_FREQ_MHZ", result.stderr)

class Identity(unittest.TestCase):
    def test_usb_ancestry_ignores_interface_copies_and_distinguishes_ports(self):
        node = {"IOObjectClass": "IOUSBHostDevice", "IORegistryEntryID": 10,
                "USB Serial Number": "device", "idVendor": 51966, "idProduct": 16385,
                "locationID": 100, "IORegistryEntryChildren": [
                    {"IOObjectClass": "IOUSBHostInterface", "IORegistryEntryID": 11,
                     "idVendor": 51966, "idProduct": 16385,
                     "IORegistryEntryChildren": [{"IOCalloutDevice": "/dev/cu.usbmodem123"},
                                                  {"BSD Name": "disk4s1"}]}]}
        disks, ports = usb.registry_devices([node])
        self.assertEqual(disks["disk4s1"].entry, 10)
        self.assertEqual(ports[0].entry, 10)
        self.assertEqual(ports[0].serial, "device")

    def test_missing_uf2_volumes(self):
        with patch.object(usb, "snapshot", return_value=([], [])), self.assertRaisesRegex(run.RunError, "Missing UF2"):
            run.select_boards(Path("/nonexistent/boards.json"))

    def test_ambiguous_serial_refused(self):
        with self.assertRaisesRegex(run.RunError, "Ambiguous"):
            run.unique_match([RX, RX], RX.key)

    def test_saved_mapping_survives_volume_name_swap(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "boards.json"
            path.write_text(json.dumps({"rx": {"boot_key": RX.key}, "tx": {"boot_key": TX.key}}))
            with patch.object(usb, "snapshot", return_value=([TX, RX], [])), patch.object(run, "identify") as identify:
                boards, _ = run.select_boards(path)
            identify.assert_not_called()
            self.assertEqual(boards["rx"], RX)

    def test_same_board_for_both_roles_refused(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "boards.json"
            path.write_text(json.dumps({role: {"boot_key": RX.key} for role in ("rx", "tx")}))
            with patch.object(usb, "snapshot", return_value=([RX, TX], [])), self.assertRaisesRegex(run.RunError, "same board"):
                run.select_boards(path)

    def test_physical_disconnect_must_match_selected_board(self):
        with patch.object(run, "prompt", side_effect=[RX.path, ""]), \
                patch.object(usb, "snapshot", return_value=([RX], [])), \
                self.assertRaisesRegex(run.RunError, "Disconnect"):
            run.identify("rx", [RX, TX])

    def test_physical_reconnect_updates_mount_and_attachment(self):
        new = usb.Device(40, RX.serial, RX.vendor, RX.product, RX.location, "/Volumes/new name")
        with patch.object(run, "prompt", side_effect=[RX.path, "", ""]), \
                patch.object(usb, "snapshot", side_effect=[([TX], []), ([TX, new], [])]):
            board, _ = run.identify("rx", [RX, TX])
        self.assertEqual(board, new)

    def test_changed_attachment_at_same_mount_refused(self):
        with patch.object(usb, "snapshot", return_value=([TX], [])), self.assertRaisesRegex(ValueError, "identity changed"):
            usb.verify_volume(TX.path, RX.entry)

    def test_rx_enumeration_timeout(self):
        with patch.object(usb, "snapshot", return_value=([RX, TX], [])), \
                self.assertRaisesRegex(run.RunError, "enumeration timeout"):
            run.wait_for_rx(RX, None, timeout=0.001)

    def test_enumeration_uses_topology_and_expected_serial(self):
        other = usb.Device(50, "other", APP.vendor, APP.product, TX.location, "/dev/cu.usbmodem1")
        with patch.object(usb, "snapshot", return_value=([TX], [other, APP])):
            self.assertEqual(run.wait_for_rx(RX, APP.key), APP)
            with self.assertRaisesRegex(run.RunError, "serial differs"):
                run.wait_for_rx(RX, "wrong")

    def test_ambiguous_cdc_refused(self):
        with patch.object(usb, "snapshot", return_value=([TX], [APP, APP])), \
                self.assertRaisesRegex(run.RunError, "Ambiguous RX CDC"):
            run.wait_for_rx(RX, None)

class ReadinessAndCompletion(unittest.TestCase):
    def test_readiness_rejects_each_mismatch_and_old_packets(self):
        run.check_report(report(), CONFIG, clean=True)
        for key, value in (("freq_mhz", 2402), ("tx_interval_ms", 20), ("packet_target", 50),
                           ("tx_power_dbm", 4), ("radio_ready", 0), ("valid", 1),
                           ("crc_fail", 1), ("have_sequence", 1), ("radio_errors", 1)):
            with self.subTest(key=key), self.assertRaises(run.RunError):
                run.check_report(report(**{key: value}), CONFIG, clean=True)

    def test_readiness_timeout(self):
        runner = run.Runner(CONFIG, Path("unused"))
        runner.capture = Mock()
        runner.capture.rows.return_value = []
        with self.assertRaisesRegex(run.RunError, "readiness timeout"):
            runner.ready(timeout=0.001)

    def test_last_sequence_completes_despite_loss(self):
        state = run.Completion(CONFIG, 0)
        for uptime in (101000, 102000):
            self.assertIsNone(state.feed(report(valid=9580, have_sequence=1, first=0, last=9999,
                idle_ms=4000, uptime_ms=uptime), uptime / 1000))
        self.assertEqual(state.feed(report(valid=9580, have_sequence=1, first=0, last=9999,
                idle_ms=5000, uptime_ms=103000), 103), "complete_observed")

    def test_valid_target_alone_does_not_complete(self):
        state = run.Completion(CONFIG, 0)
        for uptime in (1000, 2000, 3000):
            self.assertIsNone(state.feed(report(valid=10000, have_sequence=1, last=9998,
                uptime_ms=uptime, idle_ms=4000), 20))

    def test_missing_last_packet_is_explicitly_incomplete_after_margin(self):
        state = run.Completion(CONFIG, 0)
        for now in (108, 109, 110):
            status = state.feed(report(valid=9900, have_sequence=1, last=9998,
                uptime_ms=now * 1000, idle_ms=10000), now)
        self.assertEqual(status, "incomplete_tail")

    def test_no_packets_never_implies_completion(self):
        state = run.Completion(CONFIG, 0)
        for now in (110, 111, 112):
            self.assertIsNone(state.feed(report(uptime_ms=now * 1000, idle_ms=100000), now))

    def test_experiment_hard_timeout(self):
        runner = run.Runner(CONFIG, Path("unused"))
        runner.capture = Mock()
        with patch.object(run.time, "monotonic", return_value=1000):
            self.assertEqual(runner.collect(0), "timed_out")

    def test_reset_and_out_of_range_sequence_refused(self):
        with self.assertRaisesRegex(run.RunError, "reset"):
            run.check_report(report(uptime_ms=10), CONFIG, report())
        with self.assertRaisesRegex(run.RunError, "Sequence outside"):
            run.check_report(report(have_sequence=1, last=10000), CONFIG)

class Orchestration(unittest.TestCase):
    def test_failed_command_is_reported_with_log(self):
        with tempfile.TemporaryDirectory() as folder:
            runner = run.Runner(CONFIG, Path(folder))
            with self.assertRaisesRegex(run.RunError, "Command failed \\(7\\)") as error:
                runner.command([sys.executable, "-c", "print('copy failed'); raise SystemExit(7)"])
            self.assertIn("copy failed", str(error.exception))
            self.assertIn("copy failed", (Path(folder) / "runner.log").read_text())
            self.assertIsNone(runner.command_process)

    def test_command_timeout_reaps_process(self):
        with tempfile.TemporaryDirectory() as folder:
            runner = run.Runner(CONFIG, Path(folder))
            real_popen = subprocess.Popen
            children = []
            def child(*args, **kwargs):
                process = real_popen(*args, **kwargs)
                children.append(process)
                return process
            with patch.object(run.subprocess, "Popen", side_effect=child), self.assertRaises(subprocess.TimeoutExpired):
                runner.command([sys.executable, "-c", "import time; time.sleep(60)"], timeout=0.05)
            self.assertIsNotNone(children[0].poll())
            with self.assertRaises(ProcessLookupError):
                os.kill(children[0].pid, 0)

    def test_flash_preserves_space_in_mount_and_uses_existing_flasher(self):
        with tempfile.TemporaryDirectory() as folder:
            runner = run.Runner(CONFIG, Path(folder))
            with patch.object(usb, "verify_volume") as verify, patch.object(runner, "command") as command:
                runner.flash("rx", RX)
            verify.assert_called_once_with(RX.path, RX.entry)
            args = command.call_args.args[0]
            self.assertEqual(args[1], str(ROOT / "scripts/flash.py"))
            self.assertEqual(args[args.index("--volume") + 1], RX.path)
            self.assertEqual(args[args.index("--usb-entry-id") + 1], str(RX.entry))

    def test_matching_images_are_required(self):
        raw = image() + firmware.CONFIG_MAGIC
        import struct
        raw += struct.pack("<4I", 2440, 10, 10000, 0)
        self.assertEqual(firmware.image_configuration(raw), CONFIG)
        with self.assertRaises(ValueError):
            firmware.image_configuration(image())
        with self.assertRaises(ValueError):
            firmware.image_configuration(raw + firmware.CONFIG_MAGIC)
        with tempfile.TemporaryDirectory() as folder:
            runner = run.Runner(dict(CONFIG, freq_mhz=2402), Path(folder))
            with patch.object(runner, "command"), patch.object(firmware, "validated_artifact", return_value=firmware.encode_uf2(raw)), \
                    patch.object(Path, "read_text", return_value=json.dumps({"configuration": CONFIG})), \
                    self.assertRaisesRegex(run.RunError, "configuration mismatch"):
                runner.build()

    def exercise(self, folder, fail_tx=False, confirm="FLASH", unconfirmed=None, collection="complete_observed"):
        runner = run.Runner(CONFIG, Path(folder))
        events = []
        capture = Mock()
        capture.rows.return_value = [report()]
        def flash(role, board, dry=False):
            events.append((role, dry))
            if not dry:
                runner.copy_status[role] = "unconfirmed" if role == unconfirmed else "confirmed"
            if role == "tx" and not dry:
                self.assertTrue(capture.rows.called)
                if fail_tx:
                    raise run.RunError("TX flashing failure")
        with patch.object(runner, "build", return_value={}), \
                patch.object(run, "select_boards", return_value=({"rx": RX, "tx": TX}, {"rx": {}, "tx": {}})), \
                patch.object(runner, "flash", side_effect=flash), \
                patch.object(run, "prompt", return_value=confirm), \
                patch.object(run, "wait_for_rx", return_value=APP), \
                patch.object(run, "write_json"), patch.object(usb, "verify_volume"), \
                patch.object(run, "Capture", return_value=capture), \
                patch.object(runner, "collect", return_value=collection) as collect:
            try:
                result = runner.run()
            except run.RunError as exc:
                runner.finish("failed", str(exc))
                result = "failed"
            else:
                runner.finish(result)
        return events, capture, collect, result

    def test_rx_monitor_ready_before_tx_and_cleanup(self):
        with tempfile.TemporaryDirectory() as folder:
            events, capture, collect, result = self.exercise(folder)
        self.assertEqual(events, [("rx", True), ("tx", True), ("rx", False), ("tx", False)])
        self.assertEqual(result, "complete_observed")
        capture.close.assert_called_once()
        collect.assert_called_once()

    def test_tx_flashing_failure_never_collects_or_claims_success(self):
        with tempfile.TemporaryDirectory() as folder:
            _, capture, collect, result = self.exercise(folder, fail_tx=True)
        self.assertEqual(result, "failed")
        collect.assert_not_called()
        capture.close.assert_called_once()

    def test_unconfirmed_late_sync_keeps_monitor_without_claiming_copy_success(self):
        for role in ("rx", "tx"):
            with self.subTest(role=role), tempfile.TemporaryDirectory() as folder:
                events, capture, collect, result = self.exercise(folder, unconfirmed=role)
            self.assertEqual(result, "copy_unconfirmed")
            self.assertEqual(events.count((role, False)), 1)  # No retry.
            collect.assert_called_once()
            capture.close.assert_called_once()

    def test_unconfirmed_copy_without_rf_evidence_still_times_out(self):
        with tempfile.TemporaryDirectory() as folder:
            _, _, collect, result = self.exercise(folder, unconfirmed="tx", collection="timed_out")
        collect.assert_called_once()
        self.assertEqual(result, "timed_out")

    def test_unconfirmed_copy_is_saved_in_summary(self):
        with tempfile.TemporaryDirectory() as folder:
            runner = run.Runner(CONFIG, Path(folder))
            runner.copy_status = {"rx": "confirmed", "tx": "unconfirmed"}
            runner.collection_status = "complete_observed"
            runner.last = report(have_sequence=1, last=9999, valid=9900)
            runner.finish("copy_unconfirmed")
            data = json.loads((Path(folder) / "summary.json").read_text())
            self.assertEqual(data["status"], "copy_unconfirmed")
            self.assertEqual(data["copy_status"]["tx"], "unconfirmed")
            self.assertEqual(data["collection_status"], "complete_observed")

    def test_flash_accepts_only_explicit_late_sync_code(self):
        with tempfile.TemporaryDirectory() as folder:
            runner = run.Runner(CONFIG, Path(folder))
            with patch.object(usb, "verify_volume"), patch.object(runner, "command", return_value=3) as command:
                runner.flash("tx", TX)
                self.assertEqual(command.call_args.kwargs["allowed_codes"], (0, 3))
                self.assertEqual(runner.copy_status["tx"], "unconfirmed")
                runner.flash("tx", TX, dry=True)
                self.assertEqual(command.call_args.kwargs["allowed_codes"], (0,))
            self.assertEqual(runner.command([sys.executable, "-c", "raise SystemExit(3)"], allowed_codes=(0, 3)), 3)
            with self.assertRaises(run.RunError):
                runner.command([sys.executable, "-c", "raise SystemExit(1)"], allowed_codes=(0, 3))

    def test_no_flash_without_explicit_confirmation(self):
        with tempfile.TemporaryDirectory() as folder:
            events, capture, collect, _ = self.exercise(folder, confirm="no")
        self.assertTrue(all(dry for _, dry in events))
        capture.rows.assert_not_called()
        collect.assert_not_called()

    def test_capture_parses_fragmented_csv_and_reaps_child(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder)
            real_popen = subprocess.Popen
            def child(*args, **kwargs):
                return real_popen([sys.executable, "-c", "import time; time.sleep(60)"], **kwargs)
            with patch.object(run.subprocess, "Popen", side_effect=child):
                capture = run.Capture("/unused", path)
            pid = capture.process.pid
            stream = io.StringIO()
            logger = monitor.ExperimentLog(stream, "test")
            logger.feed(("RX " + " ".join(f"{k}={v}" for k, v in report().items()) + "\n").encode())
            data = stream.getvalue()
            try:
                (path / "rx.csv").write_text(data[:-5])
                self.assertEqual(capture.rows(), [])
                with (path / "rx.csv").open("a") as out:
                    out.write(data[-5:])
                self.assertEqual(capture.rows(), [report()])
            finally:
                capture.close()
            with self.assertRaises(ProcessLookupError):
                os.kill(pid, 0)

    def test_cleanup_on_actual_interruption(self):
        with tempfile.TemporaryDirectory() as folder:
            # Run only mocked orchestration in a subprocess, with a real monitor
            # child. This exercises the real signal/finally/exit path, no USB.
            code = '''
import pathlib, sys, time
from unittest.mock import patch
sys.path.insert(0, sys.argv[1])
import run_experiment as r
r.ROOT = pathlib.Path(sys.argv[2])
def work(self):
    real = r.subprocess.Popen
    def child(*args, **kw):
        return real([sys.executable, '-c', 'import time; time.sleep(60)'], **kw)
    with patch.object(r.subprocess, 'Popen', side_effect=child):
        self.capture = r.Capture('/unused', self.folder)
    (r.ROOT / 'child.pid').write_text(str(self.capture.process.pid))
    time.sleep(60)
r.Runner.run = work
sys.exit(r.main([]))
'''
            process = subprocess.Popen([sys.executable, "-c", code, str(ROOT / "scripts"), folder],
                                       stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            try:
                deadline = time.monotonic() + 5
                pid_file = Path(folder) / "child.pid"
                while not pid_file.exists() and time.monotonic() < deadline:
                    time.sleep(0.02)
                self.assertTrue(pid_file.exists())
                child_pid = int(pid_file.read_text())
                process.send_signal(signal.SIGINT)
                stdout, stderr = process.communicate(timeout=5)
                self.assertEqual(process.returncode, 130, stdout + stderr)
                with self.assertRaises(ProcessLookupError):
                    os.kill(child_pid, 0)
                summary = next(Path(folder).glob("experiments/*/summary.json"))
                self.assertEqual(json.loads(summary.read_text())["status"], "interrupted")
            finally:
                if process.poll() is None:
                    process.kill()
                    process.wait()
