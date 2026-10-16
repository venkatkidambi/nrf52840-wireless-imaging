"""Measurement definitions shared by summaries and matrix aggregation."""

def telemetry_consistent(config, row):
    if row.get("tx_stats_mask") != 7 or any(row.get(k) != v for k, v in config.items()):
        return False
    required = ("tx_completions", "tx_starts", "tx_attempts", "tx_complete", "tx_failures", "tx_duration_us")
    if any(k not in row for k in required): return False
    if not (0 <= row["tx_completions"] <= row["tx_starts"] <= row["tx_attempts"] <= config["packet_target"]):
        return False
    if row.get("valid", 0) - row.get("duplicates", 0) > row["tx_starts"]: return False
    return not row["tx_complete"] or (row["tx_completions"] == config["packet_target"]
                                      and not row["tx_failures"] and row["tx_duration_us"] > 0)

def metrics(config, row):
    row = row or {}
    tx_known = telemetry_consistent(config, row)
    sequence_ok = bool(row.get("have_sequence")) and not row.get("backwards", 0)
    unique = row.get("valid", 0) - row.get("duplicates", 0)
    # A no-data run with complete TX telemetry has zero goodput, not no evidence.
    usable = (not row.get("backwards", 0) and (sequence_ok or tx_known)
              and all(row.get(k) == v for k, v in config.items()))
    complete = tx_known and bool(row.get("tx_complete")) and not row.get("tx_failures")
    duration = row.get("tx_duration_us") if tx_known else None
    starts = row.get("tx_starts") if tx_known else None
    goodput = unique * config["payload_bytes"] * 1e6 / duration if usable and complete and duration else None
    observation = row.get("observation_us", 0)
    rx_rate = ((unique - 1) * config["payload_bytes"] * 1e6 / observation
               if sequence_ok and unique > 1 and observation else None)
    return {
        **config,
        "radio_packet_bytes": config["payload_bytes"] + 8,
        "on_air_bytes": config["payload_bytes"] + 16,
        "tx_telemetry_complete": tx_known,
        "tx_attempts_measured": row.get("tx_attempts") if tx_known else None,
        "tx_starts_measured": starts,
        "tx_completions_measured": row.get("tx_completions") if tx_known else None,
        "valid_rx_packets": row.get("valid"),
        "unique_valid_rx_packets": unique if usable else None,
        "crc_failures": row.get("crc_fail"),
        "sequence_gaps": row.get("observed_gaps"),
        "payload_failures": row.get("payload_fail"),
        "actual_tx_interval_mean_us": row.get("tx_spacing_mean_us") if tx_known and row.get("tx_spacing_n") else None,
        "tx_turnaround_mean_us": row.get("tx_turn_mean_us") if tx_known and row.get("tx_turn_n") else None,
        "rx_turnaround_mean_us": row.get("rearm_mean_us") if row.get("timing_n") else None,
        "transmission_duration_us": duration,
        "receiver_observation_us": observation if sequence_ok else None,
        "application_bytes_per_second": goodput,
        "application_mbps": goodput * 8 / 1e6 if goodput is not None else None,
        "packet_delivery_ratio": unique / starts if usable and starts else None,
        "rx_observation_bytes_per_second": rx_rate,
        "projected_102400_byte_seconds": 102400 / goodput if goodput else None,
        "throughput_basis": "unique valid RX application bytes / TX first-preamble-to-final-END duration; excludes final telemetry",
        "rx_rate_basis": "(unique valid RX packets - 1) * payload / first-to-last valid RX END span",
        "usb_overlap_gap_fraction": (row.get("gaps_usb", 0) / (row.get("gaps_usb", 0) + row.get("valid_usb", 0))
            if row.get("gaps_usb", 0) + row.get("valid_usb", 0) else None),
        "non_usb_overlap_gap_fraction": (row.get("gaps_no_usb", 0) / (row.get("gaps_no_usb", 0) + row.get("valid_no_usb", 0))
            if row.get("gaps_no_usb", 0) + row.get("valid_no_usb", 0) else None),
        "image_projection_basis": "rate projection only; not measured frame delivery or end-to-end latency",
    }
