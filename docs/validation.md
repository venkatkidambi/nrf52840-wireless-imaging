# Validation results

## Milestone 4 — 2026-10-16

The M3 hardware measurement is preserved separately: reliable radio 735,221 us,
initial DATA 650,606 us, one recovery round/18 retransmits, complete image PASS.
It is not an M4 measurement. No physical boards were flashed, no live serial
port opened, and no new RF or USB performance measured during this work.

ARM GCC 15.2.1 (Arm GNU Toolchain 15.2.Rel1), C11 `-Os`, warnings as errors:
all eight paired configurations compile and pass intended configuration,
ELF/BIN/UF2, vector, role/hash, flash-region and static-RAM checks.

| Configuration | Interval us | TX flash bytes | RX flash bytes | TX static RAM | RX static RAM |
| --- | ---: | ---: | ---: | ---: | ---: |
| Image baseline | 1500 | 8844 | 17296 | 105960 | 111908 |
| Image candidate | 1200 | 8844 | 17296 | 105960 | 111908 |
| Image candidate | 1100 | 8844 | 17296 | 105960 | 111908 |
| Image admission boundary | 1044 | 8844 | 17296 | 105960 | 111908 |
| Image maximum | 0 | 8840 | 17288 | 105960 | 111908 |
| Benchmark default (32 bytes) | 10000 | 4812 | 17804 | 492 | 9740 |
| Benchmark 240-byte boundary | 1044 | 4816 | 17868 | 1324 | 10156 |
| Benchmark 247-byte maximum | 0 | 4804 | 17868 | 1356 | 10172 |

Both roles still reserve an additional 8192-byte stack. Baseline image flash ends
at TX `0x2928c`, RX `0x2b390`, below `0xea000`. Static RAM includes alignment;
RX leaves 109276 bytes of application SRAM beyond static data and reserved stack.
No heap or camera symbols are linked. Stack space is a reservation, not a measured
high-water mark. Final `build/` artifacts are image/2440 MHz/1500 us/run tag 1.

`make test` passes 89 Python tests, five benchmark-payload protocol builds, and
seven native timing/clock/image/driver executables. The production RADIO driver
is now tested with a behavioral register model, including staged-buffer ownership,
ordered double completions, late producers, deadline accounting, wrap, admission
limits, RX rearm-before-callback, cancellation and bounded peripheral failure.
The actual TIMER/PPI register setup is tested for one-shot START/capture/disarm
routing and the 7/8 us safe-arm boundary. No model claims cycle-accurate timing.
Existing selective-repair and lost COMPLETE/COMMIT/ACK tests remain; short-window
attempts preserve the original total timeout allowance. CRC tests compare byte
lookup against independent bitwise CRC over unaligned/random inputs, empty
buffers and incremental splits. The C-produced frame still matches independent
Python pixels and CRC `653244199`. USB tests include partial/zero writes, blocking,
reconnect replay and legacy/extended result parsing. Comparison tests retain
failures and reject partial, mismatched and copy-unconfirmed runs as candidates.

AddressSanitizer and UndefinedBehaviorSanitizer pass for native image protocol,
USB writer, image adapter, RADIO driver and TIMER/PPI tests. Baseline SHA-256
checks pass, and copied measurement JSON still matches the untouched experiment.
Build logs/artifacts are under `build/m4-verified/`; the full test log is
`build/m4-test.log`, and sanitizer output is `build/m4-sanitizers/checks.log`.

These checks do not validate RF loss, silicon state-transition latency, USB bus
throughput, oscillator accuracy or timing under physical load. All M4 scheduler,
CRC/generation, USB coalescing and retry-cadence performance require operator
hardware runs. The 1500 us default and 3 ms guards remain; see the
[performance audit and exact hardware procedure](performance.md).

## Milestone 3 — 2026-10-16 (software validation only)

Implemented a separate image-mode protocol, selective half-duplex recovery,
102400-byte framebuffers on both roles, CRC32 verification, framed CDC output,
standard-library host verifier/PGM writer, and an image-mode extension to the
existing runner. No physical board was flashed, no serial port was opened,
and no M3 RF or USB hardware timing was measured during implementation.

The complete existing suite and new native/host tests pass: **85 Python tests**,
benchmark protocol tests at five payload sizes, timing and clock tests, and three
new native image executables. Native tests execute the production TX/RX protocol,
radio adapter with a fake driver, and USB writer with short/zero writes. They cover
serialization golden bytes, CRC golden vector, exact final partial segment,
reverse ordering, duplicates, frame/index/length bounds, conflicting START,
selective recovery across multiple rounds, lost START/END requests and feedback,
lost COMPLETE and COMMIT acknowledgments, permanent loss and retry exhaustion,
checksum mismatch, timer wrap, SPSC queue ownership/overflow, and reconnect replay.
Python tests decode the C-generated stream, independently compare every pixel,
exercise fragmented/malformed/truncated USB records and completion failures,
check mode/configuration isolation, and verify the same mocked RX-first workflow.

Paired ARM GCC builds (warnings as errors) and linked symbol audits passed:

| Mode | Payload setting | Interval µs | TX flash | RX flash | TX static RAM | RX static RAM |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Benchmark | 32 | 1000 | 4316 | 17420 | 428 | 9668 |
| Benchmark | 240 | 1500 | 4320 | 17420 | 1260 | 10084 |
| Benchmark | 247 | 0 (max) | 4160 | 17420 | 1292 | 10100 |
| Image | 240 (236 pixels) | 0 (max) | 6176 | 15244 | 105452 | 111272 |
| Image | 240 (236 pixels) | 1500 | 6204 | 15276 | 105456 | 111276 |

Final build artifacts are image mode / 2440 MHz / 1500 µs / run tag 1. All artifacts
pass ELF/BIN/UF2 vector, load-region, role, hash, and stack-reservation checks.
Image TX ends at flash 0x2883c and RX at 0x2abac, below 0xea000. Each still reserves
an additional 8192-byte stack. RX has 109908 bytes of application SRAM remaining
after static data and stack reservation. Exact build manifests and logs are saved
locally in `build/m3-build-audit.json` and `build/m3-build-*.log`.

ELF tests verify both 102400-byte framebuffers, aligned DMA buffers and two-slot
strides, SRAM placement below 0x2003e000, initial stack at 0x20040000, and absence
of heap/camera symbols. Benchmark images contain no image framebuffer or transport
state machine. These are placement checks, not measured stack high-water marks.
The actual radio driver and TIMER/PPI implementation are unchanged.

The native protocol/state-machine suite also passes AddressSanitizer and
UndefinedBehaviorSanitizer. Python syntax compilation, shell syntax, and
`git diff --check` pass. These checks do not open hardware devices.

A source comparison against the pre-change revision confirmed byte-for-byte
preservation of `flash.py`, `usb_identity.py`, the shell runner, radio/timing driver,
and linker script. AST comparisons confirmed unchanged `identify`, `select_boards`,
`wait_for_rx`, and `Runner.flash`. Original CLI/safety tests remain enabled, including
uncertain UF2 copies and no retry. No mounts or hardware discovery were queried.

Hardware work remains the first operator-run command below, repeated RF-loss
trials, measured turnaround/guard margin, USB backpressure/disconnect tests, and
actual reliable throughput. The default initial burst already exceeds the
450–600 ms aspiration before recovery; no performance achievement is claimed.

```sh
./scripts/run_experiment.sh --mode image --frequency 2440
```

See [protocol, measurements, RAM ownership and first-run instructions](image-transfer.md).

## RX enumeration failure and wakeup correction — 2026-10-16

Operator run `experiments/2026-10-16_003021_324381-0400_2440MHz_32B_1000us_10000/`
confirmed the complete RX UF2 copy, then timed out waiting for RX CDC enumeration.
TX was never flashed. A subsequent read-only USB registry snapshot found only the
identified TX bootloader attachment; RX was absent. Original run evidence is
preserved.

The initial M2 firmware could execute `WFI` before its first USB poll, relying on
SysTick to wake the foreground. On nRF52, SysTick stops while the CPU sleeps
([Nordic documentation](https://docs.nordicsemi.com/r/bundle/nrf5_sdk_v13.1.0/page/group_nrf_systick_hal.html)).
With TX still in bootloader mode and USB startup not yet polled, RX could remain
asleep. The same wakeup choice also undermined the TX radio-wait timeout.

Both roles now start a peripheral TIMER1 1-ms compare/clear interrupt at priority
7 in `time_init()`, before sleeping. Its vector is explicitly installed; TX stops
this wakeup source after completion. RADIO remains at priority 1 and USB at 6.
TIMER0 measurement and TIMER3/PPI radio scheduling retain their existing roles.

The corrected pair built for 2440 MHz / 32 application bytes / 1000 us / 10000
packets / run tag 2604119099, passing ELF and UF2 safety checks. TX uses **4320
flash bytes / 428 static RAM bytes**; RX uses **17420 flash bytes / 9668 static RAM
bytes**. Each reserves an additional 8192-byte stack. These artifacts supersede
the initial M2 artifact sizes below; the matrix was not rebuilt for this fix.

`make test` passed **69 Python tests**, native protocol checks at all five payload
sizes, native timing checks, and a new test compiling the actual clock driver
against a register model. That model checks periodic peripheral wakeup without
CPU/SysTick, USB or RADIO activity, interrupt acknowledgement, and shutdown. New
ELF checks verify the linked TIMER1 and RADIO vectors in both images. This is
software validation, not a physical sleep/clock measurement. `git diff --check`
also passed. No firmware was flashed and no live serial port was opened during
this investigation. Enumeration and RF behavior after this correction still
require an operator retry.

## Milestone 2 — 2026-10-16 (software validation only)

Implemented the shared interrupt-driven RADIO path, two aligned DMA slots per
role, fixed application payload configuration, TIMER3/PPI scheduling, run tags,
post-run TX telemetry, extended USB reports and matrix/summary tooling. No board
was flashed, no live serial device was opened, and no RF performance was measured.
Historical hardware observations below belong to earlier firmware.

`make test` passed **68 Python tests**, the native protocol test at **32, 64,
128, 240 and 247 application bytes**, and the native timing test. Coverage includes
all single-bit data corruptions, independent golden bytes, DMA canaries, run tags,
reserved sequences, gaps/duplicates/restarts, telemetry pages and conflicting
pages, timer wrap/deadline guard arithmetic, all 24 matrix admission decisions in
both Python and C, missing telemetry, header-excluded goodput, lost-tail completion,
CSV schema/capacity, firmware pairing, existing flash safety and interruption cleanup.
The original safety cases remain enabled; no physical-flash test was substituted.

Both TX and RX built for all **10 admitted matrix combinations**; the other **14
were rejected**. These are admission/build outcomes, not successful RF experiments.
The 32-byte / 200-us request, for example, fails the chosen handoff budget even
though raw airtime is only 192 us. Build logs and machine-readable admission,
configuration, hash and size records are in `build/m2-matrix-final/` locally.

| Application bytes | Interval us (0=max) | TX flash bytes | RX flash bytes | TX static RAM bytes | RX static RAM bytes |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 32 | 1000 | 4196 | 17356 | 428 | 9668 |
| 32 | 500 | 4196 | 17356 | 428 | 9668 |
| 32 | 250 | 4196 | 17356 | 428 | 9668 |
| 32 | 0 | 4048 | 17356 | 428 | 9668 |
| 64 | 1000 | 4204 | 17356 | 556 | 9732 |
| 64 | 500 | 4204 | 17356 | 556 | 9732 |
| 64 | 0 | 4056 | 17356 | 556 | 9732 |
| 128 | 1000 | 4200 | 17356 | 812 | 9860 |
| 128 | 0 | 4052 | 17356 | 812 | 9860 |
| 240 | 0 | 4052 | 17356 | 1260 | 10084 |

Additional paired builds passed at 247 bytes/max (TX 4040 flash / 1292 static RAM;
RX 17356 flash / 10100 static RAM), 240 bytes/1200 us, and the legacy
`TX_PACKET_COUNT=0` branch. The final local artifacts are **2440 MHz / 32 bytes /
1000 us / 10000 packets / run tag 1**: TX **4196 flash / 428 static RAM**, RX
**17356 flash / 9668 static RAM**. Both reserve a separate 8192-byte stack.
`build/m2-audit.json` records these extra builds, hashes, DMA addresses and sizes.

Every built pair passed the existing ELF load/vector/flash/RAM checks and
UF2/hash/role checks. Flash remains `[0x27000,0xEA000)`, SRAM starts at 0x20008000,
and the top 8 KiB remains reserved for stack. The linker and flash safety scripts
were not changed. Symbol inspection of the extra builds found no unresolved or
heap-allocation entry points and no `nrf_nvmc_write_word`. The DMA symbol audit
confirmed four-byte alignment, static SRAM placement below the stack, and correct
array strides: 80 bytes for two 40-byte slots, 496 bytes for two 248-byte slots,
and 512 bytes for two padded 255-byte slots. No HM01B0/camera access was introduced.
Stack capacity is a link-time reservation, not a measured runtime high-water mark.

Python syntax checks, shell syntax checks and `git diff --check` passed.
No hardware tool, mount discovery or serial access is needed for these checks:

```sh
make test
python3 scripts/benchmark_matrix.py --build --output build/m2-matrix-recheck
make build APP_PAYLOAD_SIZE=247 TX_INTERVAL_US=0
make build APP_PAYLOAD_SIZE=32 TX_INTERVAL_US=1000 TX_PACKET_COUNT=10000
```

The register design was checked against Nordic's RADIO packet/state-machine
specification, linked in [the experiment rationale](experiments.md#state-machine-and-ownership-rationale).
Host compilation cannot validate PPI propagation, END-to-START radio readiness,
CRC handoff timing, oscillator accuracy, RF loss, or USB load effects. Required
hardware work is: run the first 32-byte/1000-us experiment, verify complete final
TX telemetry and clean diagnostics, measure turnaround/spacing at decreasing
intervals, then run/repeat the payload matrix including maximum mode. Exercise
USB backpressure/disconnect and receiver recovery, inspect USB overlap buckets,
and independently confirm event-to-air timing before treating the numbers as RF
latency. The 20-us admission budget and 8-us compare-programming guard are
unverified engineering allowances; missed deadlines remain visible.

No measured throughput improvement, sustained maximum, image-transfer latency,
or 450–600 ms target achievement is claimed. See the measurement definitions for
why a receiver-only time span or a payload-only airtime calculation is insufficient.

## Operator run at 23:54 on 2026-10-15 — recovered RX evidence

Run directory: `experiments/2026-10-15_235426_344758-0400_2440MHz_10ms_10000/`.
RX identity, enumeration, and clean readiness passed. The TX host write then
returned EIO: zero acknowledged bytes from a single submitted 4096-byte UF2.
The original runner stopped its monitor at RX uptime 2000 ms.

A subsequent **five-second read-only CDC inspection** found the same RX USB
attachment (registry entry `4295071287`, serial `AC261B8166A787DE`) with these
stable cumulative counters:

| Measurement | Recovered value |
| --- | ---: |
| Frequency / interval / target | 2440 MHz / 10 ms / 10000 |
| First / last valid sequence | 0 / 9999 |
| Valid packets | 9857 |
| Observed sequence gaps / expected span | 143 / 10000 |
| Observed gap rate | 1.43% |
| CRC / payload failures | 91 / 0 |
| Duplicates / backwards | 0 / 0 |
| Radio / rearm / USB errors | 0 / 0 / 0 |
| Rearm min / mean / max | 132 / 137 / 143 µs |
| Timing samples | 9948 |
| Observation duration | 99990 ms |

The final four reports (uptime 176001–179001 ms) are saved as
`recovered_rx.log`; `recovered_summary.json` records the identity, final
diagnostics, and limitations. Original config, CSV, raw log, and failed summary
remain unchanged. This establishes final-sequence reception in the same RX
attachment; it does not reconstruct the missing per-second trace or independently
verify TX's SWD counters or the precise USB failure mechanism. The host's failed
write was not evidence that zero bytes reached the device.

The upstream 0.6.2 [MSC completion callback](https://github.com/adafruit/Adafruit_nRF52_Bootloader/blob/0.6.2/src/usb/msc_uf2.c)
finishes application DFU once all UF2 blocks are counted. This is consistent
with a reboot/host-write acknowledgement race, but the installed `dirty` build
and actual host transfer were not traced. No bootloader changes were made.

The flasher now writes individual 512-byte UF2 blocks and tracks acknowledged
bytes separately from the submitted final span. EIO/ENODEV on that final span
permits continued RX collection with copy status **unconfirmed**, just as a
late sync/close error does. Earlier write failures still abort; no write is
retried. Terminal failure output now includes the command's diagnostic log tail.
`make test` passed **62 Python tests and both native C programs**, including the
original single-call EIO scenario, final-block uncertainty, early-block failure,
short-write boundaries, and no retry. No boards were flashed for this fix;
the revised write sequence still needs operator hardware validation.

## Milestone 1.2 — 2026-10-15

Follow-up after the operator's `2026-10-15_234822_345957-0400` run: logs show
successful RX enumeration/readiness and TX preflight checks, followed by errno 5
during the TX copy step. The original flasher did not record the operation that
failed, so a final-sync disconnect remains a hypothesis. Capture ended before
another complete RX report; zero packets in that summary do not establish that
TX failed to start. The original run artifacts have been preserved unchanged.

The flasher now records operation/byte progress and distinguishes full host
writes followed by EIO/ENODEV at sync/close (exit 3, unconfirmed) from write
failures (exit 1). The runner preserves capture for the former, records separate
copy and collection statuses, and returns exit 2 even if final-sequence evidence
arrives. It never retries the copy. Follow-up `make test` passed **58 Python
tests and both native C programs**, including short writes, partial write errors,
late sync/close errors, unrelated errors, preserved uncertainty, timeout without
RF evidence, and continued capture. No additional hardware flashing or live
serial monitoring was performed for this fix.

Implemented `scripts/run_experiment.sh`, its standard-library Python runner,
and macOS USB ancestry discovery. The existing safe UF2 flasher and CDC/CSV
monitor are reused. The flasher accepts an optional verified USB attachment ID
and rechecks it immediately before copying. The only firmware addition is a
retained 32-byte configuration record; radio/protocol/camera behavior is unchanged.

Software validation on the macOS development host:

- `make build RADIO_FREQ_MHZ=2440 TX_INTERVAL_MS=10 TX_PACKET_COUNT=10000`
  passed with ARM GCC 15.2.1 and warnings as errors. Both embedded configurations
  and manifests report 2440 MHz / 10 ms / 10000 attempts / 0 dBm.
- All existing ELF/BIN/vector/UF2/layout validations passed. TX occupies
  `[0x27000, 0x277F8)` (2040 flash bytes, 168 static RAM bytes); RX occupies
  `[0x27000, 0x2ADBC)` (15804 flash bytes, 4852 static RAM bytes). Both retain
  the 8192-byte reserved stack and application end limit `0xEA000`.
- `make test` passed both native C programs and **49 Python tests**, including
  all previous configuration, CSV, protocol, timing, and flashing safety tests.
- New tests exercise default/missing arguments, invalid settings, missing UF2
  mounts, duplicate identities, swapped mount names, physical disconnect/reconnect
  verification, changed attachment IDs, USB device/interface ancestry, ambiguous
  CDC ports, enumeration/readiness timeouts, each RX setting mismatch, prior
  traffic, image mismatches, correct flash/monitor ordering, explicit confirmation,
  TX flashing failure, loss-tolerant completion, missing final sequence, no traffic,
  hard timeout, RX reset detection, and paths containing spaces.
- A real subprocess interruption test sends SIGINT to mocked orchestration and
  verifies exit 130, a saved interrupted summary, and no surviving monitor child.
  Additional tests verify command failure logs, timeout reaping, and fragmented
  CSV consumption through the existing logger.
- `bash -n scripts/run_experiment.sh`, Python syntax compilation, and the shell
  entrypoint's `--help`/invalid-argument paths passed without hardware writes.
- A read-only macOS IOService USB snapshot confirmed the connected RX device's
  FICR serial, USB location, and `/dev/cu.usbmodem101` ancestry. The parser was
  checked against that snapshot; this observed path is not hardcoded in discovery.

**No physical board was flashed, no live serial port was opened, and no RF run
was performed during implementation.** Physical first-run labeling, bootloader
disk ancestry, mode transitions, copy/ejection behavior, and the complete RX-first
hardware workflow still require operator validation. Final local build artifacts
are 2440 MHz / 10 ms / 10000 attempts. The older measurements below describe
Milestone 1.1 artifacts, before the configuration record was added.

## Milestone 1.1 historical validation

Validation performed on 2026-10-15 on the macOS development host, using
ARM GCC 15.2.1 (Arm GNU Toolchain 15.2.Rel1), C11, `-Os`, and warnings as errors.
Dependencies were already available locally. **No board was flashed, no live
serial port was opened, and no RF/timing hardware measurements were performed.**

## Compilation and artifacts

A clean parallel TX/RX build passed, followed by all nine configurations below.
Configuration changes were exercised without cleaning between rows, including
returning to an earlier configuration. Both images had distinct UF2 hashes for
each matrix row. RX binary banner strings matched the requested settings. Each
ELF passed vector/load-region/RAM validation; each BIN matched its ELF; each UF2
passed role/hash validation and its decoded payload matched the BIN.

| Frequency MHz | Interval ms | Target | TX flash bytes | RX flash bytes | Result |
| ---: | ---: | ---: | ---: | ---: | --- |
| 2402 | 10 | 10000 | 2004 | 15772 | Pass |
| 2402 | 20 | 10000 | 2008 | 15772 | Pass |
| 2402 | 50 | 10000 | 2008 | 15772 | Pass |
| 2440 | 10 | 10000 | 2008 | 15772 | Pass |
| 2440 | 20 | 10000 | 2012 | 15772 | Pass |
| 2440 | 50 | 10000 | 2012 | 15772 | Pass |
| 2476 | 10 | 10000 | 2008 | 15772 | Pass |
| 2476 | 20 | 10000 | 2012 | 15772 | Pass |
| 2476 | 50 | 10000 | 2012 | 15772 | Pass |

Additional TX/RX builds passed at 2360, 2399, and 2500 MHz to exercise Low MAP,
its upper selected value, and the upper Default MAP boundary. The continuous
`TX_PACKET_COUNT=0` branch also built successfully. Final artifacts were restored
to **2402 MHz / 10 ms / 10000 attempts**. An unchanged configuration produced a
no-op rebuild (ELF modification times unchanged).

The sweep exposed a coarse-mtime problem with the host's Make: rapid configuration
changes could leave old objects or UF2s despite changed settings. The configuration
step now validates inputs first and explicitly invalidates generated role objects,
dependency files, and images on a setting change. Unchanged settings retain outputs.
A regression test exercises invalidation independently of timestamp resolution.

Final default image memory, including alignment:

| Image | Flash bytes | Flash interval (end exclusive) | Static RAM | Reserved stack |
| --- | ---: | --- | ---: | ---: |
| TX | 2004 | `[0x27000, 0x277D4)` | 168 | 8192 |
| RX | 15772 | `[0x27000, 0x2AD9C)` | 4852 | 8192 |

Both fit well inside the verified application region `[0x27000, 0xEA000)` and
application SRAM. The RX line buffer is 1536 bytes; CDC TX FIFO is 2048 bytes.
A test checks that the complete schema fits even with 20-digit values in every
field. Stack reservation is a link-time limit, **not measured runtime stack use**.
Symbol inspection found no unresolved symbols, heap allocation entry points,
or `nrf_nvmc_write_word` in either ELF. Startup, linker, flash scripts, and the
UF2 validation/generation implementation remain unchanged.

## Tests

`make test` passes two native C executables and **18 Python unittest cases**:

- Protocol: independent golden bytes, all single-bit payload corruptions,
  sequence wrap, gaps, duplicates, backwards/restart behavior, leading-packet
  baseline, no CRC/gap double counting, and overflow-safe ppm scaling.
- Timing arithmetic: empty statistics, min/mean/max, sums above 32 bits,
  UINT32_MAX sample-count saturation, maximum possible sum, modular timer wrap,
  zero-tick subspans, stale/overlong spans, and reversed capture rejection.
- Configuration: defaults, integer/range rejection, 10/20/50 ms compilation,
  frequency/MAP encoding and boundaries, direct C preprocessor guards, Make
  rejection, unchanged header behavior, and changed-product invalidation.
- CSV: byte-by-byte fragmentation, multiple reports, UTC timestamps, run labels,
  session-reset heuristic, exact observed gap ratio, invalid-rate blanks,
  malformed/truncated/oversized input, schema agreement, and overwrite refusal.
- Existing safety suite: UF2 encoding, unsafe addresses/flags/families/vectors,
  reservation limits, manifest role/hash, changed board/SoftDevice/layout,
  mandatory explicit target, and built artifacts. The built-image test now also
  compares decoded UF2 payload to the current ELF to detect stale UF2s.

Python syntax compilation also passed for the changed scripts/tests. Serial
parsing tests use synthetic diagnostic text; they are not USB hardware tests.

## Timing path inspection and limitations

ARM disassembly of the default RX image confirms:

- Enabled `radio_timing_end()` has 15 instructions through return (excluding
  alignment/literal words): one capture write, two capture reads, and state
  stores. There is no loop, division, printing, or interrupt masking there.
- The enabled EGU/RXEN launch branch has nine instructions through return, plus
  its caller's branch-and-link. PPI captures END/READY/RXEN independently of CPU
  interrupt servicing. This replaces the direct RXEN write only on RX.
- `radio_receive()` waits for READY before calling the aggregation function.
  Aggregation performs four bounded min/max/sum/count updates; report-time means
  and decimal formatting are outside the turnaround-critical path.
- The USBD wrapper adds two timer captures, END checks, subtraction, and a maximum
  update. It has no new loop, formatting, division, or interrupt source.

These are instruction-path checks, not a measured cycle count: bus stalls,
flash timing, IRQ entry/exit, and USB load require the actual boards. Hardware
must confirm plausible ramp values, microsecond capture accuracy, no excessive
observer overhead, stable USB enumeration/backpressure, and actual loss behavior.
The existing normal ramp-up mode and packet format remain unchanged.

Unsigned 32-bit timer differences are tested across wrap. Timing sums are uint64;
even UINT32_MAX samples of UINT32_MAX ticks cannot overflow them. Counts saturate.
The rate formatter avoids overflowing a 64-bit numerator during ppm conversion.
Original protocol counters retain modulo-32-bit semantics; new span counters and
elapsed duration use uint64. Debugger halts and CPU stalls for a full timer wrap
are explicitly excluded from valid experiments, as explained in the procedure.

## Reproduce the software checks

```sh
make clean
for freq in 2402 2440 2476; do
  for interval in 10 20 50; do
    make -j4 build RADIO_FREQ_MHZ="$freq" TX_INTERVAL_MS="$interval" TX_PACKET_COUNT=10000 || exit 1
  done
done
make build RADIO_FREQ_MHZ=2402 TX_INTERVAL_MS=10 TX_PACKET_COUNT=10000
make test
```

These commands build only. Use the separately documented
[controlled experiment and manual flash workflow](experiments.md) for hardware.
