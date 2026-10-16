#!/usr/bin/env python3
"""Interactive, local two-board experiment runner; no third-party dependencies."""
import argparse
from dataclasses import asdict
from datetime import datetime, timezone
import csv
import fcntl
import json
import os
from pathlib import Path
import signal
import secrets
import subprocess
import sys
import time

import configure
from benchmark_metrics import metrics, telemetry_consistent
import firmware
import monitor
import usb_identity as usb

ROOT = Path(__file__).resolve().parents[1]
SETTINGS = (*configure.FIELDS.values(), "tx_power_dbm")
ACTIVITY = ("valid", "crc_fail", "payload_fail", "last", "gaps", "timing_n",
            "duplicates", "backwards", "radio_errors", "rearm_failures", "tx_stats_mask")

class RunError(Exception):
    pass

def require(condition, message):
    if not condition:
        raise RunError(message)

def arguments(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mode", choices=("benchmark", "image"), default="benchmark")
    parser.add_argument("frequency", nargs="?", help="Legacy frequency MHz")
    parser.add_argument("interval", nargs="?", help="Legacy interval in milliseconds")
    parser.add_argument("count", nargs="?", help="Legacy bounded packet count")
    parser.add_argument("--frequency", dest="frequency_option")
    parser.add_argument("--payload", help="Application bytes, 32..247; image mode uses 240")
    schedule = parser.add_mutually_exclusive_group()
    schedule.add_argument("--interval-us", help="START interval in microseconds")
    schedule.add_argument("--max-throughput", action="store_true")
    parser.add_argument("--packets")
    args = parser.parse_args(argv)
    try:
        if args.frequency and args.frequency_option or args.count and args.packets:
            raise ValueError("Do not mix positional and named values for the same setting")
        if args.interval and (args.interval_us is not None or args.max_throughput):
            raise ValueError("Do not mix legacy milliseconds with microsecond/max scheduling")
        env = {"RADIO_FREQ_MHZ": args.frequency_option or args.frequency or "2440",
               "APP_PAYLOAD_SIZE": args.payload or ("240" if args.mode == "image" else "32"),
               "TX_PACKET_COUNT": args.packets or args.count or ("434" if args.mode == "image" else "10000")}
        if args.mode == "image":
            env["IMAGE_MODE"] = "1"
            if not args.interval and args.interval_us is None and not args.max_throughput:
                env["TX_INTERVAL_US"] = "1500"
        if args.interval: env["TX_INTERVAL_MS"] = args.interval
        if args.interval_us is not None: env["TX_INTERVAL_US"] = args.interval_us
        if args.max_throughput: env["TX_INTERVAL_US"] = "0"
        config = configure.configuration(env)
        if config["TX_PACKET_COUNT"] == 0:
            raise ValueError("Experiment runner requires a bounded packet count greater than zero")
    except ValueError as exc:
        parser.error(str(exc))
    return configure.metadata(config)

def write_json(path, data):
    temp = path.with_suffix(path.suffix + ".tmp")
    temp.write_text(json.dumps(data, indent=2) + "\n")
    temp.replace(path)

def prompt(text):
    require(sys.stdin.isatty(), "Interactive terminal required for physical board identification and confirmation")
    try:
        return input(text).strip()
    except EOFError as exc:
        raise RunError("Confirmation cancelled") from exc

def unique_match(devices, key):
    matches = [d for d in devices if key and d.key == key]
    require(len(matches) <= 1, "Ambiguous board identities: duplicate USB serial; physically identify boards again")
    return matches[0] if matches else None

def identify(role, volumes):
    label = "RX (no camera)" if role == "rx" else "TX (camera attached)"
    print(f"Identify physical {label}. Current bootloader mounts:")
    for d in volumes:
        print(f"  {d.path}  serial={d.serial or '(absent)'} location=0x{d.location:x}")
    selected = prompt(f"Enter the current full volume path for {label}: ")
    matches = [d for d in volumes if d.path == selected]
    require(len(matches) == 1, "Selected volume is missing or ambiguous")
    selected = matches[0]
    prompt(f"Unplug ONLY physical {label}, then press Enter: ")
    remaining, _ = usb.snapshot()
    require({d.entry for d in remaining} == {d.entry for d in volumes} - {selected.entry},
            "Disconnect did not uniquely verify the selected physical board; refusing to flash")
    prompt(f"Reconnect {label}, double-reset into UF2 mode, wait for its mount, then press Enter: ")
    current, _ = usb.snapshot()
    added = [d for d in current if d.entry not in {v.entry for v in remaining}]
    require(len(added) == 1 and {d.entry for d in remaining}.issubset({d.entry for d in current}),
            "Reconnect is ambiguous or another board changed; restart identification")
    board = added[0]
    require(not selected.key or selected.key == board.key, "A different USB identity was reconnected")
    print(f"Verified {label}: {board.path}")
    return board, current

def select_boards(mapping_path):
    volumes, _ = usb.snapshot()
    require(len(volumes) >= 2, "Missing UF2 volumes: connect both boards and double-reset both into bootloader mode")
    saved = json.loads(mapping_path.read_text()) if mapping_path.exists() else {}
    boards = {}
    for role in ("rx", "tx"):
        key = saved.get(role, {}).get("boot_key")
        # Duplicate serials can still be physically identified for this session,
        # but must never be accepted as reusable identities.
        matches = [d for d in volumes if key and d.key == key]
        if len(matches) == 1:
            boards[role] = unique_match(volumes, key)
        else:
            boards[role], volumes = identify(role, volumes)
    require(boards["rx"].entry != boards["tx"].entry, "Ambiguous identities: TX and RX refer to the same board")
    for role, board in boards.items():
        require(any(d.entry == board.entry for d in volumes), "Board changed during identification")
        reusable = board.key if sum(d.key == board.key for d in volumes) == 1 else None
        previous = saved.get(role, {})
        saved[role] = {"boot_key": reusable,
                       "app_key": previous.get("app_key") if previous.get("boot_key") == reusable else None}
    return boards, saved

def wait_for_rx(board, expected_key, timeout=30):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        volumes, ports = usb.snapshot()
        # The original bootloader attachment must have gone away. Pair the new
        # application to the same physical USB topology, never a cu.* suffix.
        candidates = [p for p in ports if p.vendor == 0xcafe and p.product == 0x4001
                      and board.location and p.location == board.location
                      and p.entry != board.entry]
        require(len(candidates) <= 1, "Ambiguous RX CDC ports at the verified USB location")
        if candidates and not any(d.entry == board.entry for d in volumes):
            device = candidates[0]
            require(device.key is not None, "RX application has no verifiable USB serial")
            require(sum(p.key == device.key for p in ports) == 1,
                    "Ambiguous RX application USB serial across CDC ports")
            require(not expected_key or device.key == expected_key,
                    "RX application USB serial differs from the saved physical board")
            return device
        time.sleep(0.25)
    raise RunError("RX enumeration timeout: no verified CDC port appeared at its USB location; keep cables in place")

def check_report(row, config, previous=None, clean=False):
    for key in SETTINGS:
        require(row[key] == config[key], f"RX {key} mismatch: expected {config[key]}, received {row[key]}")
    require(row["radio_ready"] == 1, "RX radio_ready is not 1")
    if previous:
        require(row["uptime_ms"] >= previous["uptime_ms"], "RX reset during experiment")
        require(all(row[k] >= previous[k] for k in ("valid", "crc_fail", "payload_fail", "timing_n")),
                "RX counters reset during experiment")
    require(not row["backwards"] and not row["duplicates"], "Unexpected duplicate/backwards sequences; run identity is uncertain")
    if clean:
        require(not any(row[k] for k in ("valid", "crc_fail", "payload_fail", "have_sequence", "timing_n", "tx_stats_mask")),
                "RX already received packets before TX flashing; reset and isolate previous transmitters")
        require(not any(row[k] for k in ("radio_errors", "rearm_failures", "usb_errors")),
                "RX startup diagnostics report errors")
    if row["tx_stats_mask"] == 7:
        require(0 <= row["tx_completions"] <= row["tx_starts"] <= row["tx_attempts"] <= config["packet_target"],
                "Inconsistent TX telemetry counters")
        require(row["valid"] - row["duplicates"] <= row["tx_starts"], "RX count exceeds measured TX starts")
        if row["tx_complete"]:
            require(row["tx_completions"] == config["packet_target"] and not row["tx_failures"]
                    and row["tx_duration_us"] > 0, "Inconsistent TX completion telemetry")
    if not clean and row["have_sequence"]:
        require(row["first"] <= row["last"] < config["packet_target"], "Sequence outside configured bounded run")

class Capture:
    """Existing monitor owns CDC and CSV; the parent tails complete CSV rows."""
    def __init__(self, port, folder):
        self.folder = folder
        self.pending = ""
        self.reader = None
        self.header = None
        self.output = (folder / "rx.log").open("xb")
        try:
            self.process = subprocess.Popen([sys.executable, str(ROOT / "scripts/monitor.py"),
                "--port", port, "--csv", str(folder / "rx.csv"), "--run-id", folder.name],
                stdout=self.output, stderr=self.output, start_new_session=True)
        except BaseException:
            self.output.close()
            raise

    def rows(self):
        require(self.process.poll() is None, "RX monitor stopped; see rx.log for serial/USB error")
        path = self.folder / "rx.csv"
        if self.reader is None:
            if not path.exists():
                return []
            self.reader = path.open()
        self.pending += self.reader.read()
        lines = self.pending.split("\n")
        self.pending = lines.pop()
        rows = []
        for line in lines:
            values = next(csv.reader([line]))
            if self.header is None:
                self.header = values
                require(self.header == monitor.CSV_FIELDS, "Unexpected monitor CSV schema")
                continue
            require(len(values) == len(self.header), "Malformed monitor CSV row")
            data = dict(zip(self.header, values))
            require(data["receiver_session"] == "0", "RX restarted or changed configuration")
            rows.append({key: int(data[key]) for key in monitor.DIAGNOSTICS})
        return rows

    def close(self):
        if self.process.poll() is None:
            self.process.send_signal(signal.SIGINT)
            try:
                self.process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait()
        if self.reader:
            self.reader.close()
        self.output.close()

class Completion:
    def __init__(self, config, start):
        self.config, self.start = config, start
        self.nominal = config["packet_target"] * (config["tx_interval_us"] or
            configure.minimum_interval(config["payload_bytes"])) / 1000000
        self.idle_seconds = max(3, 3 * config["tx_interval_us"] / 1000000)
        self.deadline = start + self.nominal * 1.25 + self.idle_seconds + 30
        self.previous = None
        self.stable = 0

    def feed(self, row, now):
        check_report(row, self.config, self.previous)
        unchanged = self.previous and all(row[k] == self.previous[k] for k in ACTIVITY)
        self.stable = self.stable + 1 if unchanged else 1
        self.previous = row
        quiet = self.stable >= 3 and row["idle_ms"] >= self.idle_seconds * 1000
        if row["tx_stats_mask"] == 7 and self.stable >= 3:
            if row["tx_complete"] and (quiet or not row["have_sequence"]):
                return "complete_reported"
            if not row["tx_complete"]:
                return "tx_failed"
        if row["have_sequence"] and quiet:
            if row["last"] == self.config["packet_target"] - 1:
                return "complete_observed"
            if now >= self.start + self.nominal + 10:
                return "incomplete_tail"
        return None

class Runner:
    def __init__(self, config, folder):
        self.config, self.folder = config, folder
        self.capture = None
        self.last = None
        self.command_process = None
        self.copy_status = {}
        self.collection_status = None

    def command(self, command, timeout=120, allowed_codes=(0,)):
        # The shell execs us; each child is explicitly reaped on errors/signals.
        with (self.folder / "runner.log").open("ab") as log:
            log_start = log.tell()
            self.command_process = subprocess.Popen(command, cwd=ROOT, stdout=log,
                                                    stderr=log, start_new_session=True)
            try:
                code = self.command_process.wait(timeout=timeout)
                if code not in allowed_codes:
                    with (self.folder / "runner.log").open("rb") as output:
                        output.seek(max(log_start, os.fstat(output.fileno()).st_size - 2048))
                        detail = output.read().decode(errors="replace").strip()
                    raise RunError(f"Command failed ({code}): {' '.join(command)}; see runner.log\n{detail}")
                return code
            finally:
                if self.command_process.poll() is None:
                    os.killpg(self.command_process.pid, signal.SIGTERM)
                    try:
                        self.command_process.wait(timeout=3)
                    except subprocess.TimeoutExpired:
                        os.killpg(self.command_process.pid, signal.SIGKILL)
                        self.command_process.wait()
                self.command_process = None

    def build(self):
        print("Building and validating matching TX/RX firmware...", flush=True)
        settings = [f"{name}={self.config[key]}" for name, key in configure.FIELDS.items()
                    if name != "TX_INTERVAL_MS"]
        # Override legacy environment settings so sub-ms builds are unambiguous.
        settings.append("TX_INTERVAL_MS=")
        settings.append(f"IMAGE_MODE={int(self.config.get('mode') == 'image')}")
        self.command(["make", "build", *settings], timeout=600)
        manifests = {}
        for role in ("rx", "tx"):
            source = ROOT / "build" / f"{role}.uf2"
            data = firmware.validated_artifact(source, role)
            embedded = firmware.image_configuration(firmware.decode_uf2(data))
            manifest = json.loads(Path(str(source) + ".json").read_text())
            require(embedded == self.config and manifest.get("configuration") == self.config,
                    f"{role.upper()} firmware configuration mismatch; refusing mixed images")
            # Freeze this run's verified images against subsequent make invocations.
            (self.folder / source.name).write_bytes(data)
            write_json(self.folder / (source.name + ".json"), manifest)
            manifests[role] = manifest
        return manifests

    def flash(self, role, board, dry=False):
        usb.verify_volume(board.path, board.entry)
        command = [sys.executable, str(ROOT / "scripts/flash.py"), "--role", role,
                   "--firmware", str(self.folder / f"{role}.uf2"), "--volume", board.path,
                   "--usb-entry-id", str(board.entry)]
        code = self.command(command + (["--dry-run"] if dry else []), timeout=45,
                            allowed_codes=(0,) if dry else (0, 3))
        if not dry:
            self.copy_status[role] = "unconfirmed" if code == 3 else "confirmed"
            if code == 3:
                print(f"{role.upper()} copy UNCONFIRMED: final write/sync/close returned an I/O error. "
                      "The board may have received data before disconnecting. "
                      "Preserving RX evidence; no copy retry. See runner.log.", flush=True)

    def ready(self, timeout=20):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            rows = self.capture.rows()
            for row in rows:
                check_report(row, self.config, self.last, clean=True)
                self.last = row
            if rows:
                return
            time.sleep(0.1)
        raise RunError("RX readiness timeout: no complete valid status report; TX was not flashed")

    def start_capture(self, port):
        return Capture(port, self.folder)

    def collect(self, start):
        completion = Completion(self.config, start)
        completion.previous = self.last
        while time.monotonic() < completion.deadline:
            status = None
            for row in self.capture.rows():
                self.last = row
                status = completion.feed(row, time.monotonic())
            if status:
                return status
            time.sleep(0.1)
        return "timed_out"

    def run(self):
        metadata = {**self.config, "timestamp": datetime.now(timezone.utc).isoformat(),
                    "firmware": self.build()}
        write_json(self.folder / "config.json", metadata)
        mapping_path = ROOT / "experiments/boards.json"
        boards, mapping = select_boards(mapping_path)
        require(boards["rx"].location != 0, "RX USB topology is unavailable; cannot safely pair application CDC")
        for role in ("rx", "tx"):
            self.flash(role, boards[role], dry=True)
        metadata["boards"] = {role: asdict(d) for role, d in boards.items()}
        write_json(self.folder / "config.json", metadata)
        print(f"RX: {boards['rx'].path}\nTX: {boards['tx'].path}\n"
              f"Configuration: {self.config}\nResults: {self.folder}")
        require(prompt("Keep cables/USB ports fixed. Type FLASH to flash RX, verify readiness, then flash TX: ") == "FLASH",
                "Flashing cancelled; no firmware written")
        print("Flashing RX...", flush=True)
        self.flash("rx", boards["rx"])
        metadata["copy_status"] = self.copy_status
        write_json(self.folder / "config.json", metadata)
        device = wait_for_rx(boards["rx"], mapping["rx"].get("app_key"))
        mapping["rx"]["app_key"] = device.key
        write_json(mapping_path, mapping)
        metadata["rx_application"] = asdict(device)
        write_json(self.folder / "config.json", metadata)
        self.capture = self.start_capture(device.path)
        self.ready()
        # Verify TX still has the same attachment, then require another fresh,
        # clean RX report immediately before the copy. No user delay remains.
        usb.verify_volume(boards["tx"].path, boards["tx"].entry)
        self.ready()
        print("RX ready with matching configuration and zero packets. Flashing TX...", flush=True)
        metadata["tx_flash_started"] = datetime.now(timezone.utc).isoformat()
        write_json(self.folder / "config.json", metadata)
        self.tx_flash_monotonic = time.monotonic()
        self.flash("tx", boards["tx"])
        start = time.monotonic()
        metadata["tx_copy_returned"] = datetime.now(timezone.utc).isoformat()
        if self.copy_status.get("tx") == "confirmed":
            metadata["tx_copy_completed"] = metadata["tx_copy_returned"]
        write_json(self.folder / "config.json", metadata)
        print("Collecting RX evidence...", flush=True)
        self.collection_status = self.collect(start)
        if self.collection_status in ("complete_observed", "complete_reported") and "unconfirmed" in self.copy_status.values():
            return "copy_unconfirmed"
        return self.collection_status

    def finish(self, status, error=None):
        if self.capture:
            self.capture.close()
        row = self.last or {}
        rate = (100 * row["observed_gaps"] / row["observed_expected"]
                if row.get("gap_rate_valid") and row.get("observed_expected") else None)
        summary = {"status": status, "error": error, "final_diagnostics": self.last,
                   "copy_status": self.copy_status, "collection_status": self.collection_status,
                   "observed_sequence_gap_percent": rate,
                   "tx_completion_independently_verified": telemetry_consistent(self.config, row) and bool(row.get("tx_complete")),
                   "metrics": metrics(self.config, row),
                   "finished_at": datetime.now(timezone.utc).isoformat(),
                   "completion_basis": {
                       "complete_reported": "Complete matching TX telemetry and stable RX counters; includes measured TX starts and duration",
                       "tx_failed": "TX telemetry reports an aborted run",
                       "complete_observed": "Final target sequence observed; at least three stable reports and idle margin",
                       "incomplete_tail": "Nominal duration plus margin elapsed with stable idle RX; final TX sequence unobserved",
                       "timed_out": "Hard collection deadline elapsed without completion evidence",
                       "copy_unconfirmed": "RF completion evidence received, but host could not confirm UF2 final write/sync/close",
                   }.get(status, error)}
        write_json(self.folder / "summary.json", summary)
        flat = {"status": status, **summary["metrics"]}
        with (self.folder / "summary.csv").open("w", newline="") as stream:
            writer = csv.DictWriter(stream, fieldnames=list(flat))
            writer.writeheader()
            writer.writerow(flat)
        titles = {"complete_reported": "Experiment complete (TX telemetry received)",
                  "tx_failed": "Experiment incomplete (TX reported failure)",
                  "complete_observed": "Experiment complete (final sequence observed)",
                  "incomplete_tail": "Experiment collection ended — INCOMPLETE: final sequence unobserved",
                  "timed_out": "Experiment TIMED OUT", "failed": "Experiment FAILED",
                  "interrupted": "Experiment INTERRUPTED"}
        titles["copy_unconfirmed"] = "Experiment collected — UF2 COPY UNCONFIRMED (RF completion evidence received)"
        print(titles[status])
        for label, value in (("Frequency", f"{self.config['freq_mhz']} MHz"),
                             ("TX interval", f"{self.config['tx_interval_us']} us (0=max)"),
                             ("Target packets", self.config["packet_target"]),
                             ("Valid packets", row.get("valid", "n/a")),
                             ("Sequence gaps", row.get("observed_gaps", "n/a")),
                             ("Observed gap rate", f"{rate:.2f}%" if rate is not None else "n/a"),
                             ("CRC failures", row.get("crc_fail", "n/a")),
                             ("Payload failures", row.get("payload_fail", "n/a")),
                             ("RX rearm mean", f"{row['rearm_mean_us']} us" if row.get("timing_n") else "n/a"),
                             ("RX rearm max", f"{row['rearm_max_us']} us" if row.get("timing_n") else "n/a")):
            print(f"{label + ':':22} {value}")
        measured = summary["metrics"]
        if measured["application_mbps"] is not None:
            print(f"Application goodput:   {measured['application_mbps']:.6f} Mbps")
            print(f"Packet delivery:       {100 * measured['packet_delivery_ratio']:.3f}%")
        else:
            print("Application goodput:   unavailable (missing TX telemetry or invalid sequence history)")
        if error:
            print(error, file=sys.stderr)
        if "unconfirmed" in self.copy_status.values():
            print(f"UF2 copy status: {self.copy_status}; inspect runner.log. No successful-copy claim is made.")
        print(f"Results: {self.folder}")

class ImageRunner(Runner):
    def start_capture(self, port):
        from image_capture import ImageCapture
        return ImageCapture(port, self.folder, self.config["run_tag"])

    def ready(self, timeout=20):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            rows = self.capture.rows()
            for row in rows:
                require(row.get("event") == "ready", "RX image already started before TX flashing")
                require(all(row.get(k) == v for k, v in self.config.items()), "RX image configuration mismatch")
                require(row.get("radio_ready") == 1 and not any(row.get(k, 1) for k in
                        ("active", "crc_errors", "invalid", "queue_drops", "usb_errors")),
                        "RX image startup is not clean/ready")
                self.last = row
            if rows: return
            time.sleep(0.1)
        raise RunError("RX image readiness timeout; TX was not flashed")

    def collect(self, start):
        deadline = start + 90
        while time.monotonic() < deadline:
            for row in self.capture.rows():
                self.last = row
            path = self.folder / "image.json"
            if path.exists():
                result = json.loads(path.read_text())
                require(result.get("status") == "pass", result.get("error", "Image verification failed"))
                require(result.get("frame_id") == self.config["run_tag"], "Host image frame mismatch")
                if hasattr(self, "tx_flash_monotonic"):
                    result["host_launch_to_delivery_seconds"] = result["host_completed_monotonic"] - self.tx_flash_monotonic
                    result["host_launch_timing_basis"] = "Host TX flash invocation to verified RESULT; includes UF2 copy, boot, generation, RF, USB and verification"
                    write_json(path, result)
                return "complete_reported"
            time.sleep(0.1)
        return "timed_out"

    def finish(self, status, error=None):
        if self.capture: self.capture.close()
        path = self.folder / "image.json"
        result = json.loads(path.read_text()) if path.exists() else None
        summary = {"status": status, "error": error, "mode": "image", "image": result,
                   "copy_status": self.copy_status, "collection_status": self.collection_status,
                   "final_diagnostics": self.last, "finished_at": datetime.now(timezone.utc).isoformat()}
        write_json(self.folder / "summary.json", summary)
        flat = {"status": status, "mode": "image", **self.config}
        if result and result.get("status") == "pass":
            flat.update(result["measurements"], host_delivery_seconds=result["host_delivery_seconds"],
                        reliable_radio_mbps=result["reliable_radio_mbps"], host_effective_mbps=result["host_effective_mbps"],
                        host_usb_receive_seconds=result.get("host_usb_receive_seconds"),
                        host_launch_to_delivery_seconds=result.get("host_launch_to_delivery_seconds"))
            flat["status"] = status
        with (self.folder / "summary.csv").open("w", newline="") as stream:
            writer = csv.DictWriter(stream, fieldnames=list(flat)); writer.writeheader(); writer.writerow(flat)
        label = "PASS: all 102400 image bytes independently verified" if status == "complete_reported" else f"Image experiment {status}"
        print(f"{label}\nResults: {self.folder}")
        if error: print(error, file=sys.stderr)
        if "unconfirmed" in self.copy_status.values():
            print(f"UF2 copy status: {self.copy_status}; no successful-copy claim is made.")

def interrupted(signum, frame):
    raise KeyboardInterrupt

def main(argv=None):
    config = arguments(argv)
    config["run_tag"] = secrets.randbits(32)
    signal.signal(signal.SIGTERM, interrupted)
    signal.signal(signal.SIGHUP, interrupted)
    experiments = ROOT / "experiments"
    experiments.mkdir(exist_ok=True)
    with (experiments / ".runner.lock").open("a") as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            print("Another experiment runner is active", file=sys.stderr)
            return 1
        name = datetime.now().astimezone().strftime("%Y-%m-%d_%H%M%S_%f%z")
        folder = experiments / f"{name}_{config['freq_mhz']}MHz_{config['payload_bytes']}B_{config['tx_interval_us']}us_{config['packet_target']}"
        folder.mkdir()
        write_json(folder / "config.json", {**config, "timestamp": datetime.now(timezone.utc).isoformat()})
        runner = (ImageRunner if config.get("mode") == "image" else Runner)(config, folder)
        status, error = "failed", None
        try:
            status = runner.run()
        except KeyboardInterrupt:
            status, error = "interrupted", "Interrupted; children stopped. Boards may need double-reset before another run."
        except (RunError, ValueError, OSError, KeyError, subprocess.SubprocessError) as exc:
            error = str(exc)
        finally:
            # Further Ctrl-C must not interrupt child reaping / evidence saving.
            for sig in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP):
                signal.signal(sig, signal.SIG_IGN)
            runner.finish(status, error)
        return {"complete_reported": 0, "tx_failed": 2, "complete_observed": 0, "incomplete_tail": 2, "timed_out": 2, "copy_unconfirmed": 2,
                "interrupted": 130, "failed": 1}[status]

if __name__ == "__main__":
    sys.exit(main())
