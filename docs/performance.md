# Milestone 4 performance audit

Historical milestone results below. For current M4.1 control timing, software
validation and the pending physical freeze gate, see [radio-freeze.md](radio-freeze.md)
and [control-audit.md](control-audit.md).

The preserved hardware baseline is commit `1d38ce1`, experiment
`2026-10-16_010657_705686-0400_2440MHz_240B_1500us_434`. Its files are untouched.
`baselines/milestone3/` contains a source archive, rebuilt rollback ELF/UF2/map
files, disassembly, copied results, and SHA-256 manifest. Rebuilt artifacts use
run tag 1; the original experiment retains the actually flashed artifacts.

## Audit before timing changes

| Component | Evidence / latency | Owner | Opportunity and risk |
| --- | --- | --- | --- |
| Generation | Measured 57,792 us; disassembly has per-pixel UDIV | M4 CPU | Precompute coordinate gradients, preserve every pixel |
| TX frame CRC | Not separately measured; nibble-table two lookups/byte | CPU/flash | Byte table, +960 flash bytes, no table RAM; must match host zlib |
| Initial DATA | Measured 650,606 us, 434 packets at 1500 us | TIMER3/PPI/RADIO | Reduce interval only after loss/recovery measurements |
| Raw airtime | Inferred 1024 us/packet, 444,416 us for 434 packets | 2 Mbps RADIO | Fixed 248 DMA bytes plus 8 RF bytes; payload-only 409,600 us is unattainable |
| TX handoff | Benchmark 1044 requested, 1052 mean actual; 9999 late arms, zero TX failures | END IRQ/CPU/compare | Existing foreground bookkeeping plus 8 us arm lead exceeds 20 us admission margin |
| Initial loss | Measured 18 missing, 9 CRC errors, zero queue drops | RF/RX | Cannot infer packet loss solely from CRC counters |
| Recovery | One round, 18 retransmits; total radio 735,221 us | Half-duplex protocol | Selective bitmap retained; no fixed full feedback-window sleep on success |
| Control exchange | RTT min 8,206 / mean 19,316 / max 41,533 us | CPU/RADIO | 3 ms guards both directions; CRC overlaps one guard |
| RX verification | Measured 36,292 us | CPU/flash | Byte CRC table; do not verify in ISR |
| USB enqueue | Measured 207,406 us INCLUDING PIXELS record CRC computation | CPU/TinyUSB/USBD | Separate header CRC cost; avoid needless short USB packet before body |
| Host receive | Measured 177,929 us | Host USB/scheduler/parser | Independent interval overlaps enqueue; cannot subtract device timestamps |
| Host delivery | Measured 966,563 us from START metadata to RESULT validation | Host monotonic clock | Includes Python pattern verification and USB; excludes TX generation/boot |

At 1044 us the benchmark actually completed 10,000 TX packets but lost 11.08%
at RX (724 CRC errors). Every subsequent arm was rebased to a safe future compare;
9999 misses are one systematically over-tight schedule, not 9999 transmission
failures. Exact instruction/interrupt contributions require hardware trace.
Observed END-to-next-START mean was 28 us, versus the admission allowance of 20.

The existing RADIO uses fast ramp-up only when changing direction. Between packets
it remains TXIDLE/RXIDLE, with END interrupts and manual START, no END_START shortcut.
The alternate RX DMA buffer is started before copying the previous packet into an
eight-slot queue (seven usable); foreground copies again into the framebuffer.
These ownership barriers and hardware CRC16 remain necessary. END is appropriate
for this proprietary mode; no BLE LR / 802.15.4 PHYEND path is in use.
The [Nordic RADIO specification](https://docs.nordicsemi.com/r/bundle/ps_nrf52840/page/radio.html)
documents the states, END and EasyDMA ownership; START must wait for idle.

## Delay and polling inventory

* TIMER3 1 MHz: 8 us minimum compare-arm lead, one-shot PPI group cancels itself.
* RADIO disable/enable: bounded 2000 us event waits; actual combined turns ~53 us.
* TX wait: requested remaining delay + 3000 us fault budget, TIMER1 wakes at 1 ms.
* RX health: ADDRESS-to-END watchdog 2000 us. Never sleep while servicing feedback.
* Control: busy 3000 us guard from received END; 120000 us reply timeout, eight
  attempts, sixteen selective-repair rounds, 5 s idle / 30 s frame limits.
* RX response suppression: do not reply in last 10000 us of peer timeout.
* USB: nonblocking tasks, 512-byte foreground writes, partial writes retained;
  4096-byte FIFO, 64-byte endpoint; READY once per second; startup bound 100 ms.
* Host runner: 100 ms readiness/collection polling, 20 s ready / 90 s image
  limits; host serial read/select timing is independent of RADIO timing.

No physical boards are flashed during implementation. The only hardware-verified
fixed image interval remains 1500 us. 1200, 1100, 1044 and max are candidates, not
verified-safe claims. The guard stays unchanged; bounded retry-cadence changes
are described below and require physical validation.

## Implemented changes and limits

TX prepares the alternate SRAM buffer during transmission. The END interrupt
snapshots the completed packet, hands the staged buffer to DMA in TXIDLE, and
arms the existing one-shot TIMER3/PPI compare. Foreground telemetry is outside
the handoff path. A two-entry completion queue handles a delayed consumer without
losing timestamps; no queued producer means no subsequent START. Comparisons
less than eight microseconds ahead are rebased once, never caught up in a burst.
Maximum mode targets END + 20 us, preserving the existing RX handoff allowance.
The benchmark uses this same mechanism with its existing CLI, packets and metrics.

This removes foreground work from the measured 28 us handoff path; it does not
prove an achieved interval. The 1044 us admission boundary is still an engineering
budget, not a newly verified safe fixed interval. Hardware must measure ISR/PPI
latency and RX loss at every candidate. A delayed IRQ/producer reduces throughput
instead of repeating a buffer. No END_START shortcut or periodic free-running
TX trigger is used.

CRC32 now uses a 256-entry flash table (1024 bytes versus 64, +960 bytes, zero
table RAM). Generation uses a 320-byte const coordinate gradient and layered
pattern fills; independent Python pixel comparison remains mandatory. There is
no per-pixel gradient division. RADIO CRC remains CRC16: its CRC length choices
do not implement the independent IEEE CRC32 frame check. Incremental RX frame
CRC was deliberately avoided because selective recovery is out of order.

DATA encoding clears only unused padding. TX keeps its DMA copy so the protocol
may reuse its serialization buffer immediately; RX keeps queue copies/barriers
so interrupt/foreground ownership remains explicit. Additional RAM funds the
second TX buffer, completion queue and telemetry, rather than saving bytes at
the expense of latency.

Recovery keeps selective full-bitmap batches, 16 repair rounds, 3 ms direction
guards, 120 ms END/verification windows and five/30-second frame limits. START
and COMMIT use 20 ms reply windows with 48 attempts, retaining the original
960 ms timeout allowance. This changes retry cadence, not the nominal successful
control delay. RX suppresses replies in the last 10 ms of the matching window.
An END request reuses TXIDLE after a drained DATA burst, avoiding an unnecessary
disable/ramp cycle. Short windows and faster CPU paths need physical validation;
if those regress recovery, retain the preserved baseline while investigating.

USB coalesces its header and initial body bytes into a 64-byte first write.
The PIXELS record still computes its own CRC, now separately timed as
`usb_header_crc_us`, inside `usb_enqueue_us`. TinyUSB, its 4096-byte FIFO,
64-byte endpoint, 512-byte service bound, partial-write handling and host verifier
remain. No larger FIFO or stack replacement is justified by current evidence.
There is no USB throughput improvement claim without a new run.

The original 24 result words are preserved; 21 appended words record spacing,
airtime, handoff, preparation, missed arms, recovery/feedback, directional turns,
RX processing/rearm and USB header CRC. The host reads both old and new records.
COMMIT adds TX telemetry while preserving DATA/header formats. Rebuild both
roles together; old RX cannot consume the extended COMMIT. See
[precise timing definitions](image-transfer.md#timing-definitions-and-telemetry).

## Reproducible validation and hardware sequence

Build-only suite (output directories must be new):

```sh
python3 scripts/performance_suite.py --output build/m4-plan
python3 scripts/performance_suite.py --build --output build/m4-builds
make build IMAGE_MODE=1 APP_PAYLOAD_SIZE=240 TX_PACKET_COUNT=434 TX_INTERVAL_US=1500
make test
```

The suite compiles all five image profiles plus default, 240-byte boundary, and
247-byte max benchmarks. It freezes ELF/BIN/UF2/maps/manifests and build logs.
It never calls board discovery, serial tools or flashing. Software tests exercise
real driver/state-machine code with register models; they do not emulate silicon
timing, propagation, EasyDMA bus contention or USB electrical behavior.

**Exact first operator hardware command**, after double-resetting both boards:

```sh
./scripts/run_experiment.sh --mode image --frequency 2440 --interval-us 1500
```

This runs the unchanged identity/preflight/FLASH/RX-first/CDC-ready workflow.
Compare the new default against the preserved hardware baseline first. Require
the complete independently verified PGM, matching frame, zero queue drops and
plausible measured airtime/spacing. Then repeat each candidate at least five times:

```sh
./scripts/run_experiment.sh --mode image --frequency 2440 --image-profile 1200
./scripts/run_experiment.sh --mode image --frequency 2440 --image-profile 1100
./scripts/run_experiment.sh --mode image --frequency 2440 --image-profile limit
./scripts/run_experiment.sh --mode image --frequency 2440 --image-profile max
python3 scripts/compare_images.py experiments --baseline baselines/milestone3 --output build/image-trials.csv
```

Reset both boards between operator-run trials. Include interference/loss-heavy
runs, slow host reads and reconnect replay. Keep failed trials in comparisons;
do not choose an interval from its best successful run while ignoring failures.
The CSV preserves status, host verification, loss/retries, CPU/USB timings and
blank unsupported legacy metrics. Only complete, matching, verified runs with
confirmed copies qualify for baseline deltas. No automatic winner is selected.
The <650/<600/~500 ms goals remain unverified targets. Guards and the default
interval must not be reduced based solely on native tests or theoretical airtime.
