#!/usr/bin/env python3
"""Receive and independently verify an M3 image over existing USB CDC (stdlib)."""
import argparse
import array
import csv
import fcntl
import json
import os
from pathlib import Path
import select
import struct
import sys
import termios
import time
import tty
import zlib

MAGIC = b"NRFIMG3\n"
HEADER = struct.Struct("<8sBBHIII")
WIDTH = HEIGHT = 320
SIZE, SEGMENT, PACKETS = 102400, 236, 434
READY, START, PIXELS, RESULT, ERROR = range(1, 6)
LENGTHS = {READY: 52, START: 20, PIXELS: SIZE, RESULT: 96, ERROR: 4}
CONFIG_FIELDS = "freq_mhz tx_interval_ms packet_target tx_power_dbm payload_bytes tx_interval_us run_tag".split()
READY_FIELDS = CONFIG_FIELDS + "radio_ready active crc_errors invalid queue_drops usb_errors".split()
STAT_FIELDS = """status tx_generation_us tx_initial_us tx_initial_packets retransmission_rounds
retransmitted_packets tx_radio_us tx_control_rtt_n tx_control_rtt_min_us
tx_control_rtt_mean_us tx_control_rtt_max_us control_retries tx_turn_max_us
initial_missing_packets rx_radio_us rx_verify_us duplicates invalid crc_errors
queue_drops rx_turn_max_us usb_enqueue_us received_packets crc32""".split()
EXTRA_STAT_FIELDS = """tx_crc_us tx_spacing_n tx_spacing_min_us tx_spacing_mean_us tx_spacing_max_us
tx_missed_deadlines tx_airtime_mean_us tx_airtime_max_us tx_handoff_mean_us tx_handoff_max_us
tx_prepare_max_us tx_retransmission_us tx_feedback_us tx_schedule_late_max_us
tx_to_rx_max_us tx_rx_to_tx_max_us rx_processing_max_us rx_rearm_max_us usb_header_crc_us
rx_tx_to_rx_max_us rx_to_tx_max_us""".split()

def require(condition, message):
    if not condition:
        raise ValueError(message)

def expected_image():
    pixels = bytearray(SIZE)
    for y in range(HEIGHT):
        for x in range(WIDTH):
            v = (x * 255 // 319 + y * 255 // 319) // 2
            if x < 96 and y < 96:
                v = 224 if (x // 12 + y // 12) % 2 else 32
            if (x - 220) ** 2 + (y - 100) ** 2 < 45 ** 2:
                v = 255
            if 40 <= x < 130 and 210 <= y < 280:
                v = 16
            if x == y or x + y == 319:
                v = 128
            pixels[y * WIDTH + x] = v
    return bytes(pixels)

def encode_record(kind, frame, body):
    prefix = struct.pack("<8sBBHII", MAGIC, 1, kind, 0, frame, len(body))
    return prefix + struct.pack("<I", zlib.crc32(body, zlib.crc32(prefix))) + body

class Parser:
    """Bounded incremental parser: text may precede records, never enter a body."""
    def __init__(self):
        self.pending = bytearray()
        self.diagnostic_bytes = 0
        self.image_started_at = self.image_finished_at = None

    def feed(self, chunk):
        # Serial caller limits reads to 4096. Also handle large test/file chunks
        # without trusting a wire length for allocation.
        records = []
        for offset in range(0, len(chunk), 4096):
            self.pending.extend(chunk[offset:offset + 4096])
            while self.pending:
                if not self.pending.startswith(MAGIC):
                    if MAGIC.startswith(self.pending):
                        break
                    newline = self.pending.find(b"\n")
                    if newline < 0:
                        require(len(self.pending) <= 8192, "Unframed USB input too long")
                        break
                    line = bytes(self.pending[:newline + 1])
                    require(all(b in (9, 10, 13) or 32 <= b < 127 for b in line), "Malformed USB magic/text")
                    self.diagnostic_bytes += len(line)
                    require(self.diagnostic_bytes <= 8192, "Excessive USB diagnostic text")
                    del self.pending[:newline + 1]
                    continue
                if len(self.pending) < HEADER.size:
                    break
                _, version, kind, reserved, frame, length, crc = HEADER.unpack_from(self.pending)
                require(version == 1 and reserved == 0 and kind in LENGTHS, "Invalid USB header")
                require(length == LENGTHS[kind] or
                        (kind == RESULT and length == 4 * (len(STAT_FIELDS) + len(EXTRA_STAT_FIELDS))),
                        "Invalid USB record length")
                if kind == PIXELS and self.image_started_at is None:
                    self.image_started_at = time.monotonic()
                if len(self.pending) < HEADER.size + length:
                    break
                body = bytes(self.pending[HEADER.size:HEADER.size + length])
                actual = zlib.crc32(body, zlib.crc32(self.pending[:20]))
                require(actual == crc, "USB record CRC32 mismatch")
                if kind == PIXELS: self.image_finished_at = time.monotonic()
                records.append((kind, frame, body))
                del self.pending[:HEADER.size + length]
        return records

    def finish(self):
        require(not self.pending, "Truncated USB record")

class Receiver:
    def __init__(self, expected_frame=None, now=time.monotonic):
        self.expected_frame, self.now = expected_frame, now
        self.frame = self.metadata = self.pixels = self.result = None
        self.started = None

    def accept(self, kind, frame, body):
        require(self.expected_frame is None or frame == self.expected_frame, "Frame ID differs from run tag")
        require(self.frame is None or frame == self.frame, "Frame ID changed within capture")
        self.frame = frame
        require(self.result is None, "Unexpected record after completion")
        if kind == ERROR:
            raise ValueError(f"RX reported image failure {struct.unpack('<I', body)[0]}")
        if kind == READY:
            require(self.metadata is None, "RX reset/readiness after frame start")
            row = dict(zip(READY_FIELDS, struct.unpack("<13I", body)))
            require(row["run_tag"] == frame, "Readiness frame mismatch")
            return {"event": "ready", "mode": "image", **row}
        if kind == START:
            require(self.metadata is None, "Repeated frame start; restart capture after reconnect")
            width, height, size, segment, packets, crc, pattern = struct.unpack("<HHIHHII", body)
            require((width, height, size, segment, packets, pattern) ==
                    (WIDTH, HEIGHT, SIZE, SEGMENT, PACKETS, 1), "Invalid image metadata")
            self.metadata = dict(width=width, height=height, bytes=size, segment_bytes=segment,
                                 packets=packets, crc32=crc, pattern_version=pattern)
            self.started = self.now()
            return {"event": "start", "frame_id": frame, **self.metadata}
        require(self.metadata is not None, "Image/result arrived before metadata")
        if kind == PIXELS:
            require(self.pixels is None and len(body) == SIZE, "Duplicate/incomplete image")
            require(zlib.crc32(body) == self.metadata["crc32"], "Full image CRC32 mismatch")
            expected = expected_image()
            require(body == expected, "Pixel mismatch against independent synthetic pattern")
            self.pixels = body
            return {"event": "pixels_verified", "bytes": len(body)}
        require(kind == RESULT and self.pixels is not None, "Completion without verified image")
        require(len(body) in (96, 4 * (len(STAT_FIELDS) + len(EXTRA_STAT_FIELDS))), "Invalid result length")
        stats = dict(zip(STAT_FIELDS + EXTRA_STAT_FIELDS, struct.unpack(f"<{len(body)//4}I", body)))
        require(stats["status"] == 1 and stats["received_packets"] == PACKETS and
                stats["crc32"] == self.metadata["crc32"], "Invalid completion status/count/checksum")
        require(stats["tx_initial_packets"] == PACKETS and stats["retransmission_rounds"] <= 16 and
                stats["retransmitted_packets"] <= 16 * PACKETS and
                stats["initial_missing_packets"] <= PACKETS and stats["tx_radio_us"] > 0 and
                stats["tx_initial_us"] <= stats["tx_radio_us"], "Inconsistent TX/RX telemetry")
        elapsed = self.now() - self.started
        self.result = {"status": "pass", "frame_id": frame, **self.metadata, "measurements": stats,
                       "host_delivery_seconds": elapsed,
                       "host_effective_mbps": SIZE * 8 / elapsed / 1e6 if elapsed > 0 else None,
                       "initial_packet_loss_percent": 100 * stats["initial_missing_packets"] / PACKETS,
                       "reliable_radio_mbps": SIZE * 8 / stats["tx_radio_us"],
                       "host_timing_basis": "START metadata received to RESULT validated; includes host verification, excludes TX generation and boot",
                       "radio_timing_basis": "TX first START request to receipt of matching COMPLETE plus return guard",
                       "usb_timing_basis": "RX header preparation through last image byte accepted by CDC FIFO; host receipt measured separately",
                       "tx_received_complete_ack": True}
        self.result["host_completed_monotonic"] = self.now()
        return {"event": "complete", **self.result}

    def finish(self):
        require(self.result is not None, "Incomplete image transfer (no validated completion)")

def atomic_json(path, value):
    temp = path.with_suffix(path.suffix + ".tmp")
    temp.write_text(json.dumps(value, indent=2) + "\n")
    temp.replace(path)

def save_image(folder, receiver):
    receiver.finish()
    with (folder / "image.pgm").open("xb") as stream:
        stream.write(b"P5\n320 320\n255\n" + receiver.pixels)
    atomic_json(folder / "image.json", receiver.result)
    flat = {k: v for k, v in receiver.result.items() if not isinstance(v, dict)}
    flat.update(receiver.result["measurements"])
    flat["status"] = "pass"
    with (folder / "image.csv").open("x", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(flat))
        writer.writeheader(); writer.writerow(flat)

def discover_port():
    import usb_identity
    _, ports = usb_identity.snapshot()
    matches = [p.path for p in ports if p.vendor == 0xcafe and p.product == 0x4001]
    require(len(matches) == 1, "Specify --port: RX CDC discovery is missing or ambiguous")
    return matches[0]

def capture(port, folder, expected_frame, timeout):
    parser, receiver = Parser(), Receiver(expected_frame)
    fd = os.open(port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    previous = None
    try:
        previous = termios.tcgetattr(fd)
        tty.setraw(fd)
        attrs = termios.tcgetattr(fd)
        attrs[2] |= termios.CLOCAL | termios.CREAD
        attrs[2] &= ~getattr(termios, "CRTSCTS", 0)
        attrs[4] = attrs[5] = termios.B115200
        termios.tcsetattr(fd, termios.TCSANOW, attrs)
        fcntl.ioctl(fd, termios.TIOCMBIS, array.array("i", [termios.TIOCM_DTR | termios.TIOCM_RTS]))
        deadline = time.monotonic() + timeout
        with (folder / "rx.bin").open("xb") as raw, (folder / "image_events.jsonl").open("x") as events:
            while time.monotonic() < deadline:
                readable, _, _ = select.select([fd], [], [], 0.25)
                if not readable:
                    continue
                chunk = os.read(fd, 4096)
                require(chunk, "CDC disconnected before completion")
                raw.write(chunk); raw.flush()
                for record in parser.feed(chunk):
                    event = receiver.accept(*record)
                    events.write(json.dumps(event) + "\n"); events.flush()
                if receiver.result is not None:
                    parser.finish()
                    receiver.result["host_usb_receive_seconds"] = parser.image_finished_at - parser.image_started_at
                    receiver.result["host_usb_timing_basis"] = "Complete PIXELS header parsed to image record CRC verified; includes host read scheduling"
                    save_image(folder, receiver)
                    return receiver.result
        parser.finish(); receiver.finish()
    finally:
        if previous is not None:
            try:
                termios.tcsetattr(fd, termios.TCSANOW, previous)
            except OSError:
                pass
        os.close(fd)

def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--port", help="Verified RX CDC port; otherwise require exactly one CAFE:4001 device")
    p.add_argument("--output", type=Path, required=True)
    p.add_argument("--expected-frame", type=int)
    p.add_argument("--timeout", type=float, default=180)
    p.add_argument("--runner", action="store_true", help=argparse.SUPPRESS)
    args = p.parse_args(argv)
    require(args.timeout > 0, "Timeout must be positive")
    args.output.mkdir(parents=True, exist_ok=args.runner)
    try:
        result = capture(args.port or discover_port(), args.output, args.expected_frame, args.timeout)
        print(f"PASS: {SIZE} bytes, every pixel verified, CRC32={result['crc32']:08x}", flush=True)
        return 0
    except (ValueError, OSError, KeyboardInterrupt) as exc:
        atomic_json(args.output / "image.json", {"status": "fail", "error": str(exc) or "Interrupted"})
        print(f"FAIL: {exc or 'Interrupted'}", file=sys.stderr, flush=True)
        return 1

if __name__ == "__main__":
    sys.exit(main())
