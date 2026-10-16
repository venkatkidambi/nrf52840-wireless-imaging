#!/usr/bin/env python3
"""Plan or compile the M4 regression suite. No discovery, serial access or flashing."""
import argparse
import json
from pathlib import Path
import shutil
import subprocess
import configure
import firmware

ROOT = Path(__file__).resolve().parents[1]


def configurations():
    for profile, interval in configure.IMAGE_PROFILES.items():
        yield f"image-{profile}", dict(IMAGE_MODE=1, APP_PAYLOAD_SIZE=240, TX_PACKET_COUNT=434, TX_INTERVAL_US=interval)
    yield "benchmark-default", dict(IMAGE_MODE=0, APP_PAYLOAD_SIZE=32, TX_PACKET_COUNT=10000, TX_INTERVAL_US=10000)
    yield "benchmark-240-limit", dict(IMAGE_MODE=0, APP_PAYLOAD_SIZE=240, TX_PACKET_COUNT=10000, TX_INTERVAL_US=1044)
    yield "benchmark-247-max", dict(IMAGE_MODE=0, APP_PAYLOAD_SIZE=247, TX_PACKET_COUNT=10000, TX_INTERVAL_US=0)


def control_configurations():
    for profile in configure.CONTROL_PROFILES:
        yield f"control-{profile}-1100", dict(IMAGE_MODE=1, APP_PAYLOAD_SIZE=240, TX_PACKET_COUNT=434,
                                            TX_INTERVAL_US=1100, CONTROL_PROFILE=profile)
    yield "control-baseline-1500", dict(IMAGE_MODE=1, APP_PAYLOAD_SIZE=240, TX_PACKET_COUNT=434,
                                      TX_INTERVAL_US=1500, CONTROL_PROFILE="baseline")
    yield from ((name, dict(config, CONTROL_PROFILE="baseline"))
                for name, config in configurations() if name.startswith("benchmark"))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", action="store_true")
    parser.add_argument("--control-profiles", action="store_true", help="M4.1 control profiles and benchmark regression builds")
    parser.add_argument("--output", type=Path, required=True, help="New directory")
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    records = []
    for name, config in (control_configurations() if args.control_profiles else configurations()):
        command = ["make", "build", "TX_INTERVAL_MS=", "RADIO_FREQ_MHZ=2440", "RUN_ID=1", "CONTROL_PROFILE=baseline",
                   *(f"{k}={v}" for k, v in config.items())]
        record = dict(name=name, command=command, status="planned")
        records.append(record)
        if args.build:
            folder = args.output / name; folder.mkdir()
            with (folder / "build.log").open("xb") as log:
                result = subprocess.run(command, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT)
            record["status"] = "compiled" if result.returncode == 0 else "failed"
            if not result.returncode:
                record["firmware"] = {}
                expected = configure.metadata(configure.configuration({"RADIO_FREQ_MHZ": "2440", "RUN_ID": "1",
                    **{k: str(v) for k, v in config.items()}}))
                for role in ("tx", "rx"):
                    firmware.validated_artifact(ROOT / "build" / f"{role}.uf2", role)
                    binary, _ = firmware.check_elf(ROOT / "build" / f"{role}.elf")
                    firmware.require(firmware.image_configuration(binary) == expected, "Suite configuration mismatch")
                    firmware.require(binary == (ROOT / "build" / f"{role}.bin").read_bytes(), "Suite ELF/BIN mismatch")
                    for suffix in ("elf", "bin", "uf2", "uf2.json", "map"):
                        shutil.copyfile(ROOT / "build" / f"{role}.{suffix}", folder / f"{role}.{suffix}")
                    record["firmware"][role] = json.loads((folder / f"{role}.uf2.json").read_text())
        (args.output / "suite.json").write_text(json.dumps(records, indent=2) + "\n")
        print(f"{name}: {record['status']}", flush=True)
        if record["status"] == "failed": raise SystemExit(f"Build failed; see {folder / 'build.log'}")


if __name__ == "__main__":
    main()
