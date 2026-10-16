#!/usr/bin/env python3
"""Plan/build the M2 matrix offline, or explicitly run it with safe board prompts."""
import argparse
import csv
import json
import os
import signal
from pathlib import Path
import subprocess
import sys

import configure
import firmware
import run_experiment as runner

ROOT = Path(__file__).resolve().parents[1]

def combinations(payloads=(32, 64, 128, 240), intervals=(1000, 500, 250, 200, 150, 0),
                 frequency=2440, packets=10000):
    rows = []
    for payload in payloads:
        for interval in intervals:
            row = dict(payload_bytes=payload, tx_interval_us=interval,
                       freq_mhz=frequency, packet_target=packets,
                       minimum_interval_us=configure.minimum_interval(payload))
            try:
                configure.configuration({"RADIO_FREQ_MHZ": str(frequency),
                    "APP_PAYLOAD_SIZE": str(payload), "TX_INTERVAL_US": str(interval),
                    "TX_PACKET_COUNT": str(packets)})
                if not packets: raise ValueError("Matrix experiments must be bounded")
                row.update(status="feasible_unmeasured", reason=None)
            except ValueError as exc:
                row.update(status="rejected", reason=str(exc))
            rows.append(row)
    return rows

def save(folder, rows):
    runner.write_json(folder / "matrix.json", rows)
    # Keep scalar measurements in CSV; JSON also preserves artifact details.
    keys = sorted({k for row in rows for k, v in row.items() if not isinstance(v, dict)})
    with (folder / "matrix.csv").open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=keys, extrasaction="ignore")
        writer.writeheader()
        writer.writerows(rows)

def run_interactive(command):
    # Separate process group: forward interruption and allow the runner to save
    # evidence and reap its monitor before this wrapper exits.
    process = subprocess.Popen(command, cwd=ROOT, start_new_session=True)
    try:
        return process.wait()
    except BaseException:
        if process.poll() is None:
            os.killpg(process.pid, signal.SIGINT)
            try:
                process.wait(timeout=8)
            except subprocess.TimeoutExpired:
                os.killpg(process.pid, signal.SIGKILL)
                process.wait()
        raise

def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--build", action="store_true", help="Compile feasible entries; never flash")
    mode.add_argument("--run", action="store_true", help="Run each feasible entry with normal physical-board/FLASH prompts")
    parser.add_argument("--output", type=Path, required=True, help="New output directory")
    parser.add_argument("--payloads", nargs="+", type=int, default=[32, 64, 128, 240])
    parser.add_argument("--intervals-us", nargs="+", type=int, default=[1000, 500, 250, 200, 150, 0])
    parser.add_argument("--frequency", type=int, default=2440)
    parser.add_argument("--packets", type=int, default=10000)
    args = parser.parse_args(argv)
    args.output.mkdir(parents=True, exist_ok=False)
    rows = combinations(args.payloads, args.intervals_us, args.frequency, args.packets)
    save(args.output, rows)
    for row in rows:
        if row["status"] == "rejected":
            print(f"Reject {row['payload_bytes']} B / {row['tx_interval_us']} us: {row['reason']}")
            continue
        label = f"{row['payload_bytes']}B_{row['tx_interval_us']}us"
        command = [str(ROOT / "scripts/run_experiment.sh"), "--frequency", str(args.frequency),
                   "--payload", str(row["payload_bytes"]), "--packets", str(args.packets)]
        command += (["--interval-us", str(row["tx_interval_us"])] if row["tx_interval_us"] else ["--max-throughput"])
        row["command"] = " ".join(command)
        if args.build:
            settings = [f"RADIO_FREQ_MHZ={args.frequency}", f"APP_PAYLOAD_SIZE={row['payload_bytes']}",
                        f"TX_INTERVAL_US={row['tx_interval_us']}", "TX_INTERVAL_MS=", f"TX_PACKET_COUNT={args.packets}", "RUN_ID=1"]
            with (args.output / (label + ".log")).open("w") as log:
                subprocess.run(["make", "build", *settings], cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, check=True)
            artifacts = {}
            for role in ("tx", "rx"):
                source = ROOT / "build" / f"{role}.uf2"
                firmware.validated_artifact(source, role)
                artifacts[role] = json.loads(Path(str(source) + ".json").read_text())
            row.update(status="built_unmeasured", artifacts=artifacts)
        elif args.run:
            runner.prompt(f"Next: {label}. Double-reset BOTH boards into UF2 mode, then press Enter: ")
            before = set((ROOT / "experiments").glob("*/summary.json"))
            code = 130
            try:
                code = run_interactive(command)
            finally:
                after = set((ROOT / "experiments").glob("*/summary.json")) - before
                row.update(status="interrupted" if code == 130 else "collection_failed", exit_code=code)
                if len(after) == 1:
                    summary_path = after.pop()
                    summary = json.loads(summary_path.read_text())
                    row.update(summary.get("metrics", {}), status=summary["status"], summary_path=str(summary_path))
                save(args.output, rows)
            if code not in (0, 2): return code
        print(f"{label}: {row['status']}", flush=True)
        save(args.output, rows)
    save(args.output, rows)
    return 0

if __name__ == "__main__":
    try:
        signal.signal(signal.SIGTERM, runner.interrupted)
        signal.signal(signal.SIGHUP, runner.interrupted)
        sys.exit(main())
    except (ValueError, OSError, subprocess.CalledProcessError, runner.RunError) as exc:
        sys.exit(str(exc))
    except KeyboardInterrupt:
        sys.exit(130)
