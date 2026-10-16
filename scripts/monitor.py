#!/usr/bin/env python3
"""macOS/POSIX CDC monitor using only the Python standard library."""
import argparse
import array
import csv
from contextlib import ExitStack
from datetime import datetime, timezone
import fcntl
import glob
import os
import select
import sys
import termios
import tty

DIAGNOSTICS = """valid radio_ready crc_fail payload_fail gaps duplicates backwards last
radio_errors usb_errors reports_skipped freq_mhz tx_interval_ms tx_power_dbm
packet_target rearm_failures have_sequence first observed_expected observed_gaps
gap_rate_valid gap_rate_ppm uptime_ms observation_ms idle_ms timing_n
rearm_min_us rearm_mean_us rearm_max_us poll_min_us poll_mean_us poll_max_us
software_min_us software_mean_us software_max_us ramp_min_us ramp_mean_us ramp_max_us
timing_rejected usb_work_max_us end_during_usb usb_irq_max_us usb_end_in_irq payload_bytes packet_bytes on_air_bytes tx_interval_us
run_tag observation_us validation_max_us gaps_usb gaps_no_usb valid_usb valid_no_usb tx_stats_mask
tx_attempts tx_starts tx_completions tx_missed tx_delay_min_us tx_delay_mean_us tx_delay_max_us
tx_spacing_n tx_spacing_min_us tx_spacing_mean_us tx_spacing_max_us tx_turn_n
tx_turn_min_us tx_turn_mean_us tx_turn_max_us tx_duration_us tx_failures tx_complete""".split()
CSV_FIELDS = ["host_timestamp", "run_id", "receiver_session", *DIAGNOSTICS,
              "sequence_gap_percent"]

def parse_report(line):
    """Only complete M2 rows; never silently turn absent fields into zeros."""
    if not line.startswith("RX "):
        return None
    values = {}
    try:
        for token in line[3:].split():
            name, value = token.split("=", 1)
            if name in values or not value.isascii() or not value.isdecimal():
                return None
            values[name] = int(value)
    except ValueError:
        return None
    if not set(DIAGNOSTICS).issubset(values):
        return None
    return {key: values[key] for key in DIAGNOSTICS}

class ExperimentLog:
    def __init__(self, stream, run_id):
        self.stream = stream
        self.writer = csv.DictWriter(stream, fieldnames=CSV_FIELDS)
        self.writer.writeheader()
        self.run_id = run_id
        self.pending = b""
        self.discarding = False
        self.previous = None
        self.session = 0
        stream.flush()

    def feed(self, chunk):
        # Serial reads can split at any byte, or contain several complete lines.
        self.pending += chunk
        while b"\n" in self.pending:
            raw, self.pending = self.pending.split(b"\n", 1)
            if self.discarding or len(raw) > 8192:
                self.discarding = False
                continue
            try:
                values = parse_report(raw.decode("ascii").strip("\r"))
            except UnicodeDecodeError:
                continue
            if values is None:
                continue
            if self.previous is not None and (
                    values["uptime_ms"] < self.previous["uptime_ms"] or
                    any(values[k] != self.previous[k] for k in
                        ("freq_mhz", "tx_interval_us", "payload_bytes", "run_tag", "packet_target"))):
                self.session += 1
            self.previous = values.copy()
            values.update(host_timestamp=datetime.now(timezone.utc).isoformat(),
                          run_id=self.run_id, receiver_session=self.session,
                          sequence_gap_percent="")
            if values["gap_rate_valid"] and values["observed_expected"]:
                values["sequence_gap_percent"] = (
                    100 * values["observed_gaps"] / values["observed_expected"])
            self.writer.writerow(values)
            self.stream.flush()
        if len(self.pending) > 8192:
            self.pending = b""
            self.discarding = True

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--list", action="store_true")
    parser.add_argument("--port", help="Explicit /dev/cu.usbmodem... path (never auto-select)")
    parser.add_argument("--csv", help="Create a new CSV file; refuses to overwrite")
    parser.add_argument("--run-id", help="Host experiment label, required with --csv")
    args = parser.parse_args()
    if args.list:
        print("\n".join(sorted(glob.glob("/dev/cu.usbmodem*"))))
        return
    if not args.port:
        parser.error("--port is required; use --list after RX firmware enumerates")
    if args.csv and not args.run_id:
        parser.error("--run-id is required with --csv; reset RX for each experiment")
    if args.run_id and not args.csv:
        parser.error("--run-id requires --csv")
    # Open exclusively before touching the port; do not overwrite prior evidence.
    with ExitStack() as stack:
        logger = None
        if args.csv:
            stream = stack.enter_context(open(args.csv, "x", newline=""))
            logger = ExperimentLog(stream, args.run_id)
        monitor(args.port, logger)

def monitor(port, logger=None):
    fd = os.open(port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    previous = termios.tcgetattr(fd)
    try:
        tty.setraw(fd)
        attrs = termios.tcgetattr(fd)
        attrs[2] |= termios.CLOCAL | termios.CREAD
        attrs[2] &= ~getattr(termios, "CRTSCTS", 0)
        attrs[4] = attrs[5] = termios.B115200
        termios.tcsetattr(fd, termios.TCSANOW, attrs)
        fcntl.ioctl(fd, termios.TIOCMBIS, array.array("i", [termios.TIOCM_DTR | termios.TIOCM_RTS]))
        print(f"Monitoring {port}; Ctrl-C to stop", file=sys.stderr)
        while True:
            readable, _, _ = select.select([fd], [], [], 1)
            if readable:
                chunk = os.read(fd, 4096)
                if not chunk:
                    raise OSError("CDC device disconnected")
                if logger is not None:
                    logger.feed(chunk)
                sys.stdout.buffer.write(chunk)
                sys.stdout.buffer.flush()
    finally:
        try:
            termios.tcsetattr(fd, termios.TCSANOW, previous)
        except OSError:
            pass
        os.close(fd)

if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        pass
    except OSError as exc:
        raise SystemExit(f"Monitor: {exc}")
