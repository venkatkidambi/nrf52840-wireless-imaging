# Reliable image transfer (Milestones 3–4.1)

This mode transfers one deterministic 320 × 320, 8-bit grayscale frame per reset.
Both devices allocate a static 102,400-byte framebuffer. The TX producer is
`image_generate(buffer)`; a future camera producer can replace it without changing
transport ownership. No camera peripheral, pin, or HM01B0 register is accessed.
RX never publishes partial pixels as a successful image.

## First hardware experiment

Close serial monitors, connect both boards, and double-reset both into their
existing UF2 bootloaders. From an interactive terminal in the repository:

```sh
./scripts/run_experiment.sh --mode image --frequency 2440
```

Defaults are 248 DMA bytes, 236 image bytes per packet, 434 initial packets,
1500 µs requested START spacing, and 0 dBm. The existing physical-board discovery,
UF2 preflight/copy implementation, serial pairing, RX-first order, and `FLASH`
confirmation are retained. Readiness now checks a checksummed image-mode record
with the exact build configuration, run tag, armed radio, and zero activity/errors.
The host opens CDC before TX is flashed. An unconfirmed UF2 copy still returns an
unconfirmed experiment result even if the image verifies. No copy is retried.

The benchmark command remains unchanged:

```sh
./scripts/run_experiment.sh --frequency 2440 --payload 240 --interval-us 1500 --packets 10000
```

Image mode accepts `--interval-us` from 1044 to 3000, or `--max-throughput`.
`--image-profile baseline|1200|1100|limit|max` selects 1500/1200/1100/1044/0 us.
1100 us now has successful full-image hardware trials; 1500 us is the conservative
fallback. `--control-profile baseline|balanced|aggressive` selects independent
control timing, with baseline as default. Reduced guards remain experimental;
see the [control audit](control-audit.md) and [freeze/validation guide](radio-freeze.md).
Its payload/count are fixed; incompatible overrides are rejected. The 3000 µs
limit keeps bounded recovery compatible with frame timeouts. Benchmark limits,
packet formats, logging, and maximum-throughput mode are unchanged. Direct builds
must specify matching image settings explicitly:

```sh
make build IMAGE_MODE=1 APP_PAYLOAD_SIZE=240 TX_PACKET_COUNT=434 TX_INTERVAL_US=1500 RADIO_FREQ_MHZ=2440
make test
```

These two commands never flash. The runner freezes role-checked UF2s and manifests
as before. Image builds use `IMAGING-CONFIG3` in the retained configuration record;
benchmark builds keep `IMAGING-CONFIG2` and their previous metadata schema.

Results retain `config.json`, `runner.log`, role UF2s/manifests, `rx.log`,
`summary.json`, and `summary.csv`. Image mode adds `rx.bin` (exact binary CDC
capture), `image_events.jsonl`, `image.pgm`, `image.json`, and `image.csv`.
The benchmark-only `rx.csv` schema is not used for binary image data.
Only verified pixels get a PGM. A failed receiver saves a failure `image.json`
and the partial raw capture. Standalone reception is also available:

```sh
python3 scripts/receive_image.py --port /dev/cu.usbmodemYOUR_VERIFIED_RX --output experiments/new-image-capture
```

Omitting `--port` requires exactly one CAFE:4001 device in the existing USB
discovery results; ambiguity is an error. Choose a new output directory. Reopen
CDC to replay a completed immutable frame after a host disconnect; start a new
capture rather than trying to join two interrupted streams. Reset both boards
for another radio experiment. The protocol intentionally supports one frame/run.

## Radio serialization

All integers are unsigned little endian, explicitly encoded; no C struct is
transmitted. Hardware radio settings and CRC16 remain those of the benchmark.
Every DMA packet is 248 bytes, zero padded after its declared body.

| Offset | Bytes | Field |
| ---: | ---: | --- |
| 0 | 1 | Protocol version = 1 |
| 1 | 1 | Type |
| 2 | 4 | Frame ID = shared random build run tag |
| 6 | 2 | Round token, 0 initially, 1–16 for retransmission |
| 8 | 2 | Packet index for DATA; zero for controls |
| 10 | 2 | Actual body length |
| 12 | 0–236 | Body; remaining DMA bytes must be zero |

| Type | Value | Body |
| --- | ---: | --- |
| FRAME_START | 1 | width:u16, height:u16, bytes:u32, segment:u16, count:u16, CRC32:u32, pattern_version:u32 (20 bytes) |
| READY | 2 | Empty, token 0 |
| FRAME_DATA | 3 | Pixels, index 0–433, token identifies original/retransmitted data |
| FRAME_END | 4 | Empty; explicitly requests a feedback window |
| MISSING | 5 | 55-byte bitmap: bit i = 1 requests segment i; unused high bits zero |
| FRAME_COMPLETE | 6 | Verified frame CRC32:u32 |
| COMMIT | 7 | Words 0–12 followed by 24–39 (116 bytes); token is last data round + 1 |
| COMMIT_ACK | 8 | Empty |
| ABORT | 9 | Error code:u32 |

There are 433 full 236-byte segments and one 212-byte final segment. Total
count/geometry are established by START rather than repeated in each DATA header.
RX checks version, type, tag, round, exact lengths, zero padding, geometry, index,
and expected final-segment length before copying. Duplicate indices do not
overwrite the first accepted copy. Ordering is independent of placement. A frame
CRC mismatch aborts rather than guessing which previously accepted segment failed.
CRC32 is reflected IEEE (0xedb88320, initial/final XOR 0xffffffff), matching zlib.
This is corruption/run-isolation checking, not authentication.

The deterministic pattern combines horizontal and vertical gradients, a 12-pixel
checkerboard in the upper left, a circle, a rectangle, and diagonals. Pattern
version 1 is implemented separately in C and Python; the host compares every byte
as well as CRC32. Tests feed the actual C-generated USB stream to the Python parser.

## Half-duplex agreement and recovery

1. TX generates pixels and CRC, then repeats START until READY. RX accepts the
   same START idempotently: it never clears an in-progress bitmap on a retry.
2. TX sends the initial burst, then END with token 0, immediately switching to RX.
3. RX responds only to START/END/COMMIT, at least **3000 µs after that request's
   hardware END** in baseline (1500 us in aggressive). It sends one response and immediately returns to RX. There are
   no unsolicited feedback timers that could collide with a data burst.
4. TX listens for **120 ms after END requests**, or **20 ms for START/COMMIT**,
   accepting only the corresponding type/frame/token. It waits another 3000 µs
   from response END before transmitting in baseline (1500 us balanced, 1000 us aggressive). END has eight attempts; START/COMMIT
   have 48, preserving the original 960 ms total timeout allowance. A timeout repeats the control
   request, never the full data burst. CRC-failed replies are ignored.
5. Each MISSING bitmap creates the next round. TX sends only requested indices;
   at most 16 recovery rounds follow the original burst. Missing END or feedback
   recovers through repeated END, including an all-missing bitmap if a burst was lost.
6. Once all segments are present, RX verifies CRC32 in foreground and sends
   COMPLETE. It retains that state and resends COMPLETE for repeated END, so losing
   the acknowledgment does not trigger full retransmission or duplicate USB output.
7. TX accepts COMPLETE only with the expected CRC32. Its success event is receipt
   of this ACK. It then sends COMMIT with final TX measurements. RX requires COMMIT
   before exporting pixels; this proves TX actually received COMPLETE. COMMIT and
   its ACK are retryable. Experimental profiles omit the terminal post-ACK guard because TX sends nothing further. Loss of every COMMIT_ACK can leave TX uncertain about
   telemetry delivery, but RX has the verified image and proof of COMPLETE receipt.
   Loss of every COMMIT leaves RX without proof and it reports timeout, not success.

RX expires incomplete/uncommitted state after five seconds of inactivity or
30 seconds from accepted START, even if traffic continues. TX also checks the
30-second bound during bursts; driver waits and control retries are bounded.
Requests taking too long to process are not answered after the reply window:
RX suppresses responses when fewer than 10 ms remain. A subsequent poll retries
the response. Retry exhaustion sends ABORT where the radio still works; silence
also becomes an explicit receiver/host failure. Status codes: 1=verified success,
2=CRC mismatch, 3=timeout, 4=RX radio failure, 5=TX recovery exhausted/driver failure.
The host has a separate collection deadline, including the case where START never
arrived. No data count, final index, or nominal duration alone implies success.

### Turnaround and ownership

The [Nordic RADIO state specification](https://docs.nordicsemi.com/r/bundle/ps_nrf52840/page/radio.html)
requires DISABLED before changing direction and READY before START. With fast
ramp-up, software controls interframe spacing; hardware TIFS is not enforced.
The adapter therefore uses the existing bounded DISABLE → DISABLED → TXEN/RXEN →
READY driver paths. The 3 ms guard is a conservative software allowance, not a
claim of measured switching latency. Both roles record the maximum local time
spent in direction-enable calls, including software and interrupt latency; that
also includes initial enable. Measure it on hardware before reducing the guard.

The RX driver starts the alternate aligned DMA slot before invoking the
image callback. The callback copies CRC-valid packets into an eight-slot SPSC
queue (seven usable), publishes with a memory barrier, and returns. Foreground
holds a queue slot until its copy finishes, then releases it. Queue overflow drops
the new packet and increments a counter; selective recovery handles missing DATA.
DMA never targets the framebuffer. TX copies serialized packets to an aligned
pair of static DMA buffers and stages the alternate buffer while the current
packet transmits. The END ISR arms the next TIMER/PPI START only after TXIDLE.
A two-entry completion queue preserves results if foreground is delayed; each
DMA buffer stays immutable until its corresponding wait returns. An absent
producer leaves RADIO idle. The burst drains before END feedback or direction changes.
The framebuffer becomes immutable on verification. USB uses a separate header,
metadata buffer, CDC FIFO, and the immutable framebuffer source.

The camera producer and full-frame CRC do not run in an ISR. DATA processing is
foreground work; the image loop services USB nonblockingly on each visit. Both
roles retain TIMER3/PPI scheduling and the independent TIMER1 wakeup. Fixed DATA
intervals rebase if late, as in the benchmark. Measured ADDRESS spacing and END-to-next-START handoff now accompany the actual
`tx_initial_us` software span. A late compare is counted once per packet and
rebased; max mode retains at least 20 us after END for receiver handoff.

## USB framing and backpressure

The same TinyUSB CDC interface, descriptors, serial identity, and USB startup
remain. Image mode emits only binary records; benchmark text is compiled out.
Each record starts with this 24-byte header:

| Offset | Field |
| ---: | --- |
| 0 | 8 bytes `NRFIMG3` followed by LF |
| 8 | version:u8 = 1 |
| 9 | type:u8 |
| 10 | reserved:u16 = 0 |
| 12 | frame:u32 |
| 16 | body_length:u32 |
| 20 | CRC32 of header bytes 0–19 followed by the body |

Types and exact lengths: READY=1/56 bytes (legacy 52 accepted), START=2/20 bytes, PIXELS=3/102400
bytes, RESULT=4/388 bytes (legacy 96/180 accepted), ERROR=5/4 bytes. READY contains seven configuration words
in retained-firmware order, then radio_ready, active, crc_errors, invalid,
queue_drops, usb_errors, then control-profile ID (0/1/2). START repeats the radio metadata. ERROR contains status.
RESULT contains 97 telemetry words; the original 45 retain their order.
New RX accepts 52/116-byte legacy COMMITs and the new 212-byte COMMIT. Old RX cannot accept
new COMMIT, so always rebuild and flash both roles through the existing runner. The host accepts bounded ASCII
diagnostic lines only between records; body bytes are never parsed as text/magic.
It rejects unknown versions/types, oversized or incorrect lengths, bad CRCs,
truncation, duplicate/mixed frames, pixels without metadata, and incomplete results.

Each USB write advances by its actual accepted count, including zero. A slow or
absent host does not discard image bytes. A disconnect clears queued output and
resets the record writer; reconnect replays metadata and the retained verified
frame. There is no host ACK or promise of host persistence from a CDC FIFO write.
Only the host's validated result establishes successful Mac delivery. USB error
or host timeout can therefore fail collection after radio success.

## Timing definitions and telemetry

All device times are unsigned local microseconds, wrap-safe within bounded runs.
No timestamp from one board is subtracted from a timestamp on another board.
The TX words (0–12, 24–39 and 45–68) are frozen into COMMIT **before its own retries**; control RTT
and retry aggregates therefore cover START and END exchanges, excluding COMMIT.

| Word | Host field | Definition |
| ---: | --- | --- |
| 0 | status | Success/error code |
| 1 | tx_generation_us | TX call entry/return around synthetic pixel generation, excludes CRC |
| 2 | tx_initial_us | TX initial burst loop entry through last successful DATA wait, includes packet preparation |
| 3 | tx_initial_packets | Initial DATA transmissions confirmed by ADDRESS and END |
| 4 | retransmission_rounds | Number of recovery data rounds |
| 5 | retransmitted_packets | Successfully completed DATA retransmissions |
| 6 | tx_radio_us | TX first START exchange call through matching COMPLETE return, includes retries and final guard; excludes generation/CRC and COMMIT |
| 7–10 | tx_control_rtt_n/min/mean/max_us | Successful control call entry/return spans, including request, turns and guards; integer mean |
| 11 | control_retries | Failed control attempts before COMPLETE; includes the final failed attempt if exhausted |
| 12 | tx_turn_max_us | Largest local direction-enable call span through COMPLETE |
| 13 | initial_missing_packets | Missing segments at first received END, including leading/trailing loss |
| 14 | rx_radio_us | RX first accepted START END through END of the request that triggers successful verification; excludes verification itself |
| 15 | rx_verify_us | Foreground processing time of the successful verification request, includes CRC |
| 16–19 | duplicates / invalid / crc_errors / queue_drops | RX transport diagnostics; CRC errors are not added to missing count |
| 20 | rx_turn_max_us | Largest RX-side direction-enable call span, including initial enable |
| 21 | usb_enqueue_us | RX PIXELS header preparation to final image byte accepted into CDC FIFO; includes backpressure, not a USB bus completion timestamp |
| 22 | received_packets | Unique accepted segments, must be 434 |
| 23 | crc32 | CRC32 actually calculated from RX framebuffer |
| 24 | tx_crc_us | TX full-frame CRC before START, excludes generation |
| 25–28 | tx_spacing_n/min/mean/max_us | ADDRESS spacing only between consecutive DATA in the same burst |
| 29 | tx_missed_deadlines | One count per completed fixed-interval DATA whose compare needed rebasing; max mode excluded |
| 30–31 | tx_airtime_mean/max_us | DATA END minus ADDRESS plus 24 us preamble/address |
| 32–33 | tx_handoff_mean/max_us | Prior DATA END to next DATA START, same burst |
| 34 | tx_prepare_max_us | DATA serialization call span, excludes adapter DMA copy and waiting |
| 35 | tx_retransmission_us | Sum of retransmission DATA burst spans, including preparation/drain |
| 36 | tx_feedback_us | Sum of END exchange spans, including retries, CRC wait and return guard |
| 37 | tx_schedule_late_max_us | Largest positive actual START minus requested fixed deadline |
| 38–39 | tx_to_rx_max_us / tx_rx_to_tx_max_us | TX board direction-enable call maxima, including initial enable |
| 40 | rx_processing_max_us | Foreground DATA decode/copy/bitmap processing maximum |
| 41 | rx_rearm_max_us | Hardware-captured END to alternate RX START maximum |
| 42 | usb_header_crc_us | PIXELS record header/CRC preparation; subset of usb_enqueue_us |
| 43–44 | rx_tx_to_rx_max_us / rx_to_tx_max_us | RX board direction-enable maxima, including initial enable |

Initial DATA, retransmission DATA, and END feedback spans are sequential TX
components; tx_radio_us also includes START negotiation and loop overhead.
RX verification overlaps TX feedback; USB header CRC is inside enqueue;
host receive overlaps enqueue. Do not add these overlapping measurements.
The appended phase fields and RX confirmation span are defined in the
[freeze guide](radio-freeze.md#added-measurements). The COMMIT round trip is deliberately excluded from frozen TX telemetry;
receipt of COMMIT proves COMPLETE reached TX, even when COMMIT_ACK is lost.

The host records START metadata reception through validated RESULT as
`host_delivery_seconds`, including its byte comparison. `host_usb_receive_seconds`
spans parsing the PIXELS header through verification of that record CRC and
includes host scheduling. The runner also records `host_launch_to_delivery_seconds`
using host monotonic timestamps around TX flash invocation and host completion;
this includes UF2 copy, boot, generation, radio, USB, and host verification. It is
an experiment launch latency, not a pure RF figure. No synchronized
generation-to-host timestamp is claimed. Standalone capture has no launch span.

`reliable_radio_mbps = 819200 / tx_radio_us`. Host effective Mbps uses the same
102400 useful bytes divided by `host_delivery_seconds`; overhead is never counted
as useful image data. These rates are emitted only after complete verification.
The payload-only theoretical lower bound is 409.6 ms; even a lossless fixed
1500 µs / 434-packet initial burst is roughly 650 ms before control overhead.
The 450–600 ms aspiration requires faster experiments and is **not achieved or
measured by these software tests**. Bidirectional loss must be measured anew.

## Memory and validation scope

Each role has one 102400-byte framebuffer. The driver has two 248-byte DMA slots,
the adapter two 248-byte TX DMA buffers and eight 252-byte packet queue slots.
RX additionally owns the 55-byte bitmap, protocol state, foreground packet/reply
copies, 64-byte USB header/prefix, 388-byte metadata area, and existing 4096-byte CDC FIFO.
Current linked totals are recorded in the [freeze guide](radio-freeze.md#validation-and-memory).
Low SRAM through 0x20008000 and the top 8192-byte stack reservation remain intact.
The unchanged linker and ELF/UF2 validator enforce `[0x27000,0xEA000)` flash and
data/BSS below 0x2003e000. Symbol tests check DMA alignment/strides, framebuffer
size, stack boundaries, and absence of heap/camera symbols. Stack reservation is
not a measured high-water mark. There is no allocation at runtime.

`make test` exercises the actual portable TX/RX state machines and image radio
adapter with fake peripherals, CRC golden vectors, reverse ordering, duplicate
segments, final boundaries, selective multi-round loss, lost requests/feedback/
COMPLETE/COMMIT/ACKs, permanent loss, checksum failure, timer wrap, queue ownership,
USB short/zero writes, disconnect replay, malformed streams, C-to-Python pattern
agreement, CLI compatibility and mocked RX-first flashing. None simulates RF
propagation, oscillator/USB electrical behavior, PPI latency, or physical reboot.
No board was flashed during implementation.

For the first hardware run, retain all artifacts and check verified PGM pixels,
both radio/host completion evidence, packet and retry counters, local timing,
and any queue drops. Then repeat with a slow reader, a reader started after radio
completion, and USB detach/reconnect. Repeat loss-heavy RF trials and loss of the
final ACK before tightening guards or claiming a sustained reliable rate.
