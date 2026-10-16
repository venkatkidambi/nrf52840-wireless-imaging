#!/usr/bin/env python3
"""Compare recorded image experiments, including failures; never build or flash."""
import argparse
import csv
import json
import statistics
from pathlib import Path
from receive_image import CONTROL_STAT_FIELDS

MEASUREMENTS = """tx_radio_us tx_initial_us initial_missing_packets retransmission_rounds
retransmitted_packets crc_errors queue_drops rx_verify_us usb_enqueue_us tx_generation_us
tx_crc_us tx_spacing_n tx_spacing_min_us tx_spacing_mean_us tx_spacing_max_us
tx_missed_deadlines tx_airtime_mean_us tx_airtime_max_us tx_handoff_mean_us tx_handoff_max_us
tx_prepare_max_us tx_retransmission_us tx_feedback_us tx_schedule_late_max_us
rx_processing_max_us rx_rearm_max_us usb_header_crc_us control_retries""".split() + CONTROL_STAT_FIELDS
HOST = "host_delivery_seconds host_usb_receive_seconds host_launch_to_delivery_seconds".split()
FIELDS = ["directory", "status", "host_verification", "eligible", "error", "control_profile", "interval_us", "frequency_mhz",
          "radio_delta_from_baseline_us", *MEASUREMENTS, *HOST]


def read_json(path):
    return json.loads(path.read_text()) if path.exists() else {}


def row(folder):
    result = {"directory": str(folder), "eligible": False}
    try:
        config, summary = read_json(folder / "config.json"), read_json(folder / "summary.json")
        image = read_json(folder / "image.json") or summary.get("image") or {}
        if config.get("mode") != "image" and summary.get("mode") != "image" and not image:
            return None
        stats = image.get("measurements", {})
        result.update(status=summary.get("status", "incomplete"), host_verification=image.get("status", "missing"),
                      error=summary.get("error") or image.get("error"), interval_us=config.get("tx_interval_us"),
                      frequency_mhz=config.get("freq_mhz"), control_profile=config.get("control_profile", "baseline"))
        # A fast partial run, stale frame, or unconfirmed copy must not qualify.
        result["eligible"] = bool(result["status"] == "complete_reported" and image.get("status") == "pass"
            and image.get("tx_received_complete_ack") is True and stats.get("status") == 1
            and stats.get("received_packets") == 434 and stats.get("tx_initial_packets") == 434
            and stats.get("crc32") == image.get("crc32") and image.get("bytes") == 102400
            and image.get("frame_id") == config.get("run_tag")
            and isinstance(stats.get("tx_radio_us"), (int, float)) and stats["tx_radio_us"] > 0
            and 0 < stats.get("tx_initial_us", 0) <= stats["tx_radio_us"]
            and "unconfirmed" not in summary.get("copy_status", {}).values())
        result.update({key: stats.get(key) for key in MEASUREMENTS})
        result.update({key: image.get(key) for key in HOST})
    except (OSError, ValueError, TypeError, AttributeError) as exc:
        result.update(status="invalid_record", host_verification="unknown", eligible=False, error=str(exc))
    return result


def directories(paths):
    found = set()
    for path in paths:
        if (path / "config.json").exists() or (path / "image.json").exists():
            found.add(path.resolve())
        else:
            found.update(p.parent.resolve() for p in path.rglob("config.json"))
    return sorted(found)


def compare(paths, output, baseline=None):
    reference = row(baseline) if baseline else None
    if baseline and (reference is None or not reference["eligible"]):
        raise ValueError("Baseline must be a complete, independently verified image experiment")
    rows = [r for path in directories(paths) if (r := row(path)) is not None]
    for r in rows:
        if reference and r["eligible"]:
            r["radio_delta_from_baseline_us"] = r["tx_radio_us"] - reference["tx_radio_us"]
    with output.open("x", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=FIELDS)
        writer.writeheader()
        writer.writerows(rows)
    return rows


def summarize(rows):
    """Group comparable configurations; failed/incomplete trials stay in denominator."""
    groups = {}
    for r in rows:
        key = (r.get("control_profile"), r.get("interval_us"), r.get("frequency_mhz"))
        groups.setdefault(key, []).append(r)
    summaries = []
    for (profile, interval, frequency), trials in groups.items():
        good = [r for r in trials if r["eligible"]]
        result = dict(control_profile=profile, interval_us=interval, frequency_mhz=frequency,
                      trials=len(trials), successes=len(good), failures=len(trials)-len(good),
                      failure_rate=(len(trials)-len(good))/len(trials))
        for field in MEASUREMENTS + HOST:
            samples = [r[field] for r in good if isinstance(r.get(field), (int, float))]
            if samples:
                result[field] = dict(n=len(samples), mean=statistics.mean(samples),
                                     median=statistics.median(samples), worst=max(samples))
        summaries.append(result)
    return summaries


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directories", type=Path, nargs="+")
    parser.add_argument("--output", type=Path, required=True, help="New CSV path; existing files are never overwritten")
    parser.add_argument("--baseline", type=Path)
    parser.add_argument("--summary", type=Path, help="New JSON path for grouped repeated-trial statistics")
    args = parser.parse_args()
    try:
        rows = compare(args.directories, args.output, args.baseline)
        grouped = summarize(rows)
        if args.summary:
            with args.summary.open("x") as stream:
                json.dump(grouped, stream, indent=2)
                stream.write("\n")
    except (OSError, ValueError) as exc:
        parser.error(str(exc))
    print(f"{len(rows)} image runs, {sum(r['eligible'] for r in rows)} eligible passes: {args.output}")
    for group in grouped:
        print(f"{group['control_profile']} / {group['interval_us']} us / {group['frequency_mhz']} MHz: "
              f"{group['successes']}/{group['trials']} passes, failure rate {group['failure_rate']:.1%}, "
              f"radio {group.get('tx_radio_us', {})}")
    print("Compare repeated trials and failure rates; no fastest-safe interval is inferred from a single pass.")


if __name__ == "__main__":
    main()
