# Validation results

## Milestone 1.2 — 2026-10-15

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
