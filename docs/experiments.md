# Experiment procedure — Milestones 1.1 and 1.2

This experiment measures the existing link before any image transfer, camera
acquisition, acknowledgments, or retries. Keep the camera connected and unused.
This document describes tests to perform; it contains no new hardware results.

## Automated runner (Milestone 1.2)

The first automated 2440 MHz experiment is:

```sh
cd ~/Documents/Code/nrf52840-wireless-imaging
./scripts/run_experiment.sh 2440 10 10000
```

Run in an interactive macOS terminal using the build prerequisites in README.
No additional Python packages are needed. Zero, one, two, or three positional
arguments are accepted; omitted trailing values default to 2402/10/10000.
Frequency and interval ranges are unchanged. This runner requires a packet
target of 1–4294967295; continuous mode is available only in the manual workflow.
`--help` describes arguments without building, discovering, or flashing.

### Physical identification and consent

1. Close other serial monitors. Put **both** boards in UF2 bootloader mode with
   double-reset; TX must remain quiet until the runner flashes it. Building does
   not reset a board or enter its bootloader automatically.
2. The runner lists actual XIAO bootloader mounts. On first use, enter the full
   current path for RX (no camera), unplug that physical board when prompted,
   and reconnect/double-reset it. The selected USB attachment must be exactly
   the one that disappeared, with exactly one replacement on reconnect.
   Repeat for TX (camera attached). Incorrect or ambiguous transitions abort.
3. Unique bootloader VID/PID/serial identities can be reused from
   `experiments/boards.json`. Neither mount names nor suffixes such as ` 1`
   establish identity. Missing or duplicate saved serials trigger identification
   again. A board without a unique serial is verified only for this attachment
   and requires identification on the next run. Remove `experiments/boards.json`
   to deliberately redo labeling.
4. Both images and both boards pass the existing flasher's dry-run validation.
   Review printed roles, mounts, and settings, then type **FLASH**. Every run
   requires this confirmation; there is no unattended `--yes` switch. Until
   confirmation, the runner only builds, reads board metadata, and saves local
   files. Keep USB cables and hub ports fixed afterward.

Discovery uses the macOS IOService registry's USB device ancestors and
`diskutil info -plist` to associate mounted disks with actual USB attachments.
Before each copy the safe flasher rechecks the current USB registry entry ID,
so a changed attachment at the same mount path is refused. Unavailable USB
ancestry or RX topology is an error, never an invitation to guess a port.
No identity is inferred from model-wide Board-ID or CURRENT.UF2 contents.

After RX flashing, its old bootloader attachment must disappear and a unique
`CAFE:4001` CDC port must appear at the same physical USB location. RX's FICR
serial is saved after this association and checked on later runs. Bootloader
and application serial strings are **not** assumed to have the same encoding.
Other CDC devices at other locations are ignored; ambiguous identities abort.
Live RX enumeration/serial ancestry was inspected read-only during development;
bootloader-to-application transitions still require the first physical run.

### Execution and evidence

The runner builds matching TX/RX images with `make build`, including existing
ELF/BIN/vector/UF2/flash-boundary checks. A retained 32-byte data record embeds
frequency, interval, target, and RF power in each image without runtime work.
Both embedded records and manifests must match the request. Frozen UF2 copies
and SHA-256 manifests are placed in the experiment directory before flashing;
later builds cannot substitute a different image for the ongoing experiment.
Do not run a separate `make` concurrently while the runner is building. A local
lock prevents simultaneous runner invocations.

RX is flashed first via `scripts/flash.py`. Enumeration has a 30-second deadline;
readiness has a 20-second deadline. The existing `scripts/monitor.py` runs before
TX, writing raw diagnostics to `rx.log` and complete reports to `rx.csv` using
its existing CSV implementation. The runner checks a complete status report for
`radio_ready=1`, exact frequency/interval/target/power, zero received packet
activity, and no startup radio/rearm/USB errors. It then verifies TX is still
the same attachment, checks fresh RX readiness again, and flashes TX without
another operator pause. A copy has a 45-second command limit and is never
automatically retried after an error. TX reboot is inferred only from subsequent
RX sequence evidence; file copy success alone is not a transmission result.

The flasher uses unbuffered 512-byte UF2 block writes and reports the failed
operation (`open`, `write`, `fsync`, or `close`), acknowledged byte count, and
current submitted span. EIO/ENODEV on the **final submitted block**, or during
final sync/close after acknowledged writes, is a distinct exit-3,
copy-unconfirmed outcome. A syscall may fail after the device receives data:
the operator's 23:54 run returned an error on a single 4096-byte write, yet the
same RX session later reported sequence 9999. Breaking writes into UF2 blocks
distinguishes an early transfer error from an ambiguous final-block handoff.
In this narrow case the runner continues bounded RX capture
without retrying; RX flashing still requires verified enumeration and clean
readiness before TX may be flashed. Errors before submitting the final block,
explicit zero-progress writes, and unrelated errors still abort.
`config.json` and `summary.json` preserve per-role `copy_status`; the summary
also records `collection_status` independently from copy confirmation. No
`tx_copy_completed` timestamp is written for an unconfirmed TX copy.

Each new `experiments/<local timestamp with offset>_<frequency>MHz_<interval>ms_<target>/`
directory contains:

| File | Evidence |
| --- | --- |
| `config.json` | Requested settings, UTC timestamps, board identities, image hashes/manifests, TX copy timestamps |
| `rx.log` | Original monitor/USB diagnostics, including banner and errors |
| `rx.csv` | Existing complete diagnostic rows with host timestamps and session detection |
| `summary.json` | Result status, reason, final complete diagnostic row, observed gap percentage |
| `runner.log` | Build, image validation, flasher output and copy errors |
| `{tx,rx}.uf2` and `.uf2.json` | Exact validated images used for the run |

Failures before monitoring still save configuration and a failure summary;
RX logs exist only if capture started. Full final diagnostics include rearming,
poll/software/ramp latency, USB timing/overlap, error counts, first/last sequence,
idle time, and duration. A host run ID does not add an on-air identifier.

### Completion, exit codes, and recovery

Let `nominal = packet_target * tx_interval_ms / 1000` seconds and
`idle = max(3 seconds, 3 * tx_interval_ms / 1000)`. Completion requires at least
three reports with stable packet/error/timing counters and RX idle time at least
`idle`. The runner never waits for `valid == packet_target`.

| Status | Exit | Meaning |
| --- | ---: | --- |
| `complete_observed` | 0 | Last target sequence (`packet_target - 1`) observed, followed by stable idle reports |
| `copy_unconfirmed` | 2 | Final sequence and stable idle observed, but at least one UF2 copy had an unconfirmed final write/sync/close |
| `incomplete_tail` | 2 | At least one valid packet, stable idle reports, and nominal duration + 10 seconds elapsed, but final sequence unobserved |
| `timed_out` | 2 | Hard deadline expired without sufficient completion/idle evidence; includes runs receiving no valid packets |
| `failed` | 1 | Build, identity, flash, serial, readiness, configuration, or run-integrity error |
| `interrupted` | 130 | Ctrl-C, termination, or hangup; children reaped and partial evidence saved |

The hard deadline is `nominal * 1.25 + idle + 30` seconds after TX copy returns.
For 10000 packets at 10 ms, nominal duration is 100 seconds and the hard limit
is 158 seconds after copy completion. A missing tail may end collection after
110 seconds if stable/idle evidence is available, but is explicitly **incomplete**.
These are host collection limits; transmitter stalls/failures can exceed the
estimate. Observing the final sequence is useful bounded-run evidence, not
independent SWD verification of `tx_complete` or proof of all offered packets.

Unexpected RX resets, changed settings, out-of-range sequences, duplicates, or
backwards movement abort rather than combining uncertain runs. Existing session
and uptime reset detection remains heuristic. There is no on-air run identifier;
keep other transmitters quiet. Observed sequence gaps exclude leading/trailing
losses and never constitute an exact RF packet-error rate.

On interruption or failure, preserve the directory, inspect `summary.json`,
`runner.log`, and `rx.log`, resolve the reported cause, then double-reset both
boards before retrying with a new directory. A failed copy may have partially
or fully started firmware; do not assume the old firmware remains installed.
Ctrl-C stops host processes, not a TX already transmitting its bounded run.
The standalone manual commands below remain available.

## Configuration and scheduling

| Setting | Default | Accepted values |
| --- | ---: | --- |
| `RADIO_FREQ_MHZ` | 2402 | Decimal integer 2360–2500 MHz |
| `TX_INTERVAL_MS` | 10 | Decimal integer 1–60000 ms; test 10, 20, optionally 50 |
| `TX_PACKET_COUNT` | 10000 | 1–4294967295 attempts; 0 means continuous |
| RF power | 0 dBm | Unchanged fixed `RADIO_TXPOWER_TXPOWER_0dBm` |

The [Nordic RADIO specification](https://docs.nordicsemi.com/r/bundle/ps_nrf52840/page/radio.html)
defines `FREQUENCY.FREQUENCY` as an offset 0–100, with `MAP=Default` based at
2400 MHz and `MAP=Low` based at 2360 MHz. The implementation uses Low below
2400 MHz and Default otherwise. Thus the three experiments use register values
2, 40, and 76 (MAP=0). Both images compile the same setting; RX repeats it in
its startup banner and every report. No runtime retuning is performed.

The existing free-running TIMER0 schedules attempts. An interval runs from the
foreground scheduling timestamp immediately before encoding/transmission to the
next such timestamp, **not from completion to completion**. Actual on-air start
includes encoding, peripheral preparation, and ramp-up. TX has no USB workload.
A late attempt starts when execution resumes and rebases the schedule; there are
no catch-up bursts. Failed attempts still consume a sequence number and count
against the target. Once the target is reached TX sleeps until reset; SWD
`tx_complete=1`, `tx_attempts`, `tx_failures`, and `tx_recovery_failures` allow
inspection without introducing TX USB. RX cannot read these variables.

Do not infer exact offered on-air packet count from the configured target.
The RX-reported interval and target are compiled expectations, not settings
negotiated with TX. Pass the same settings to build and flash commands. An
unqualified later `make flash-tx` uses the default settings unless they remain
exported in that shell. Changed settings invalidate generated role objects/images and rebuild both roles
without cleaning. This avoids stale images with macOS Make's coarse timestamps.

## What the timing measurements mean

The working RADIO state machine is preserved:

1. `READY_START` begins reception when ramp-up reaches READY.
2. RADIO receives the packet, calculates hardware CRC, and raises END;
   `END_DISABLE` begins disabling the radio. The CRC result is available for the
   completed packet. No RADIO interrupt is enabled.
3. Foreground `radio_poll()` notices END and captures a CPU-handling timestamp.
   It waits for DISABLED, reads CRCSTATUS, and copies 32 bytes out of EasyDMA.
4. `radio_receive()` prepares the existing buffer and event flags, then issues
   RXEN. It waits for READY. Validation, statistics, and USB work follow rearming.

The receiver cannot acquire another packet while disabled or ramping. READY is
the practical endpoint because the existing hardware shortcut immediately issues
START. A packet whose preamble/address falls in this blind interval can be lost
without a CRC failure. A sample includes successfully completed packets with bad
CRC or bad deterministic payload as well as valid packets.

RX owns the following instrumentation resources; none are shared with TinyUSB:

| Resource | Connection/use |
| --- | --- |
| TIMER2 | Free-running 32-bit, prescaler 4, 1 MHz; no interrupt |
| PPI channel 0 | RADIO END → TIMER2 capture 0 |
| PPI channel 1 | RADIO READY → TIMER2 capture 1 |
| TIMER2 capture 2 | CPU task write immediately after polling observes END |
| EGU0 event/task 0, PPI channel 2 | EGU event → RADIO RXEN, with fork → TIMER2 capture 3 |
| TIMER0 captures 0 / 1 | Existing system time in foreground / USB ISR |

The [PPI fork](https://docs.nordicsemi.com/r/bundle/ps_nrf52840/page/ppi.html)
issues RXEN and its capture together. A CPU interrupt before the EGU trigger
therefore belongs to software latency, not ramp-up. The
[EGU task/event mechanism](https://docs.nordicsemi.com/r/bundle/ps_nrf52840/page/egu.html)
adds no interrupt handler. Hardware captures still occur when USB preempts the
CPU. [TIMER configuration](https://docs.nordicsemi.com/r/bundle/ps_nrf52840/page/timer.html)
provides microsecond resolution; these are not cycle-accurate CPU measurements.
Allow about one timer tick of quantization per span and the PPI synchronization
delay (up to one 16 MHz clock period); no calibration correction is subtracted.

| Report prefix | Span | Interpretation |
| --- | --- | --- |
| `rearm_*_us` | END → next READY | Total turnaround blind interval |
| `poll_*_us` | END → software capture | Delay until foreground begins handling completion, including preemption |
| `software_*_us` | Software capture → hardware RXEN issuance | DISABLED wait, copy, prepare, and any interrupt delays |
| `ramp_*_us` | RXEN issuance → READY | RADIO ramp-up, independent of when the CPU notices READY |

Each prefix has minimum, integer mean (rounded down), and maximum.
`timing_n` is the common number of accepted samples. Initial arming is excluded.
Failed rearming cancels any pending sample and increments `timing_rejected`;
out-of-order captures or spans at least one second also reject a sample. A
failure before an END sample exists contributes only to error counters. Samples
are accumulated only after RX is listening again. Per-sample spans sum to total;
rounded means can differ by a few microseconds when summed.

Unsigned timestamp subtraction permits TIMER2 wrap inside a sample. The timer
wraps every 4294.967296 seconds, but accepted spans are less than one second;
normal bounded driver/USB operations are much shorter than a wrap. Do not debug
halt or suspend execution during a measurement: a halt for an entire timer wrap
is ambiguous and not detectable from these captures. Reset and repeat after any
debug halt. TIMER0 duration is extended into uint64 in the running foreground
loop, so normal long runs survive timer wrap. This also requires the loop to run
at least once per wrap. Observation timestamps are software timestamps after
validation, not packet-edge captures.

Timing sums are uint64 and sample counts saturate at UINT32_MAX. Even the
product UINT32_MAX × UINT32_MAX fits uint64; in practice each accepted span is
under one million microseconds. Saturation retains the prior aggregate rather
than wrapping a count/divisor. No division or formatting occurs between END and
RXEN. Instrumentation adds one software capture, register reads/stores, an EGU
trigger, and function-call overhead there; the PPI captures run autonomously.
Actual overhead and latency still require board measurements. Generated code
inspection and arithmetic tests are recorded in the validation document.

## USB scheduling and correlation

USB has both a priority-6 USBD interrupt (`tud_int_handler`) and foreground
`usb_task()` (`tud_task_ext(0, false)`), called from the main polling loop. USB
foreground service is not called inside `radio_receive()` or its waits, but USB
interrupts remain enabled there. Foreground checks for a newly completed packet
again before USB work. A packet can still finish during USB service or report
formatting, and then waits for the next radio poll. Nonblocking USB task service
can drain several queued events; it is not a fixed-time single-event operation.

`usb_work_max_us` covers the entire foreground USB section, including banner,
periodic formatting, writing, and interrupt preemption. `end_during_usb` counts
END changing from clear to set in that section. `usb_irq_max_us` covers the ISR
body, including its timestamp overhead but excluding exception entry/exit;
`usb_end_in_irq` counts END transitions during that ISR body. Foreground and ISR
overlap counters can describe the same packet; do not add them. They establish
overlap, not proof of a loss. Their reported values can lag the current report
by one loop iteration. Maxima are cumulative, not per-second maxima.

The longer report is still one line per second with no floating point, heap,
or per-packet printing. A 1536-byte line buffer and 2048-byte CDC TX FIFO permit
an entire report to be accepted or skipped without blocking. Slow terminals can
increase `reports_skipped`. Interrupt profiling is just two timer reads, END
checks, and a maximum update, with no output or division in the ISR.

## Counters, denominator, and run limits

Existing counters keep their meanings:

- `valid`: CRC and deterministic payload passed, including duplicates/backwards.
- `crc_fail`: completed packet had invalid hardware CRC. No sequence is trusted.
- `payload_fail`: CRC passed but deterministic payload validation failed.
- `gaps`: missing sequence values between validated packets, modulo-32-bit counter.
- `duplicates`: same sequence as the current baseline; baseline unchanged.
- `backwards`: modular backwards/out-of-order movement, including TX restarts;
  the existing implementation rebases to that sequence. It is not a reorder buffer.
- `radio_errors`: polling/state/timeout or unsuccessful rearm/recovery episodes.
- `rearm_failures`: calls to `radio_receive()` that failed, including initial arm;
  overlaps `radio_errors`, rather than adding an independent loss count.
- `usb_errors`: existing USB hardware/startup-fault count.

`first`, `last`, and `have_sequence` describe actual validated observations.
For a run with no backwards movement, `observed_expected` starts at one and adds
each forward modular sequence delta; `observed_gaps` adds delta minus one.
Duplicates add to neither. Both new accumulators are uint64. This supports
ordinary uint32 sequence wrap; deltas of at least 2^31 are ambiguous and treated
as backwards just as before. Missing packets before the first valid packet and
after the last valid packet are unknown.

```text
observed sequence-gap rate = observed_gaps / observed_expected
                          = gaps / (last - first + 1)   # simple no-wrap run
```

`gap_rate_ppm` is the fraction in parts per million, rounded down, with overflow-
safe scaling. `gap_rate_valid=0` means no valid baseline or backwards movement;
the numeric zero is then a placeholder. CSV `sequence_gap_percent` is blank for
invalid rates, and otherwise uses the exact integer counts rather than rounded
firmware ppm. `valid + gaps` is not the denominator when duplicates exist.
Do **not** add CRC failures to gaps: a lost sequence may already correspond to
a CRC failure, TX failure, missed address, or receiver downtime. A final CRC
failure might not be reflected in gaps because no later valid packet follows.

`uptime_ms` measures the RX loop's total observation time since initialization.
`observation_ms` spans the first to most recent validated packet (zero with fewer
than two), and `idle_ms` is time since the latest valid observation. RX remains
listening and reporting after TX stops, so uptime keeps advancing. Stable counts
and increasing idle time suggest silence; they do not prove TX completed.

There is no run ID or start/end marker on air. Use a fresh host `--run-id`, a new
CSV, and reset both boards for each trial. RX cannot distinguish a TX reset,
another transmitter with the same address, or some out-of-order traffic. The
CSV logger marks a new `receiver_session` if reported uptime decreases or build
settings change, but this is only a heuristic: a reset entirely between sparse
reports can go undetected. Do not combine sessions or reuse a run after a reset.
The target describes attempts, not proven transmissions. Exact packet-error
rate requires independent confirmation of TX completion and TX failures, plus
full observation of the run. Without that evidence report the **observed sequence-
gap rate**, CRC count, and timing data, not an exact RF packet-error rate.

## Required matrix and controlled procedure

Fix board separation at approximately one meter. Keep orientation, USB cables,
power supply/ports, host, camera connection, and surrounding equipment constant.
Record these details and nearby traffic/activity with the host run label.
Repeat trials (preferably at least three per setting); vary channel order on
repeats to reduce time-of-day/traffic confounding. Do not move boards between runs.

| Frequency MHz | Interval ms | Attempts | Approximate TX run duration |
| ---: | ---: | ---: | ---: |
| 2402 | 10 | 10000 | 100 s |
| 2440 | 10 | 10000 | 100 s |
| 2476 | 10 | 10000 | 100 s |
| 2402 | 20 | 10000 | 200 s |
| 2440 | 20 | 10000 | 200 s |
| 2476 | 20 | 10000 | 200 s |

Optionally repeat all three at 50 ms (~500 s). Durations exclude boot and can
increase after scheduling delays/errors; they are not stop signals.

1. Keep TX in its bootloader or unpowered. Select a matrix row, export the three
   settings, build both images, and run host tests. Identify physical RX and its
   current mount; flash RX using the safe procedure below. This resets RX stats.
2. Open the RX monitor with a fresh CSV and run label before TX starts. Verify the
   banner matches the selected row, `valid=0`, `radio_ready=1`, and no startup
   errors. If packets already arrive, isolate the unintended transmitter and
   restart the trial. Logger opening alone does not reset counters.
3. Identify and flash TX with the same settings (or reset the already matching
   TX firmware). Flashing immediately starts the bounded run. Log the host start
   time. Do not reset either board during a trial.
4. Wait at least the nominal duration plus five seconds, and retain at least
   three final reports with stable valid/last/gap/timing counts and increasing
   idle time. A missing tail remains unknown; `last=9999` is useful evidence but
   is not guaranteed. If no packets arrive, check configuration/board identity
   rather than interpreting the row as measured 100% loss.
5. Stop the logger with Ctrl-C, save the final row and note first/last sequences,
   errors, skipped reports, and any operator changes. Keep TX quiet while resetting
   RX for the next run. Reset TX only after the new logger is collecting.
6. Compare final counts and per-report counter deltas between rows/repeats.
   Increases in CRC/gaps that change by channel while rearm distributions remain
   stable support an RF/channel contribution. Large polling/software maxima,
   END-in-USB overlaps, or rearm failures accompanying gaps support scheduling
   involvement. Improvement at 20/50 ms may be consistent with timing pressure,
   but also changes offered load and collision probability. Channel changes
   alone do not conclusively identify interference. These aggregates cannot
   assign a particular missing sequence to a particular latency sample.

## Exact command workflow

All flash commands here are manual user actions. Never infer board identity
from a mount suffix. Replace mount/port placeholders only after identifying the
physical board using disconnect/reconnect and the existing discovery commands.
Keep shell settings exported in **each terminal** used for make/flash commands.

For the first experiment, in the RX/build terminal:

```sh
cd ~/Documents/Code/nrf52840-wireless-imaging
export RADIO_FREQ_MHZ=2402 TX_INTERVAL_MS=10 TX_PACKET_COUNT=10000
make clean
make build
make test
python3 scripts/flash.py --list
RX_VOLUME='/Volumes/REPLACE_WITH_IDENTIFIED_RX_MOUNT'
make flash-rx VOLUME="$RX_VOLUME" FLASH_FLAGS=--dry-run
make flash-rx VOLUME="$RX_VOLUME"
python3 scripts/monitor.py --list
RX_PORT='/dev/cu.usbmodemREPLACE_WITH_IDENTIFIED_RX_PORT'
make monitor-rx PORT="$RX_PORT" \
  MONITOR_FLAGS='--csv experiment-2402-10-r1.csv --run-id 2402-10-r1'
```

While the monitor runs, in a second terminal, identify and start TX:

```sh
cd ~/Documents/Code/nrf52840-wireless-imaging
export RADIO_FREQ_MHZ=2402 TX_INTERVAL_MS=10 TX_PACKET_COUNT=10000
python3 scripts/flash.py --list
TX_VOLUME='/Volumes/REPLACE_WITH_IDENTIFIED_TX_MOUNT'
make flash-tx VOLUME="$TX_VOLUME" FLASH_FLAGS=--dry-run
make flash-tx VOLUME="$TX_VOLUME"
```

For each remaining channel/interval row, reset/quiet TX first. Use the same
identify, dry-run, flash-RX, monitor, flash-TX steps above, replacing **both**
terminals' exported settings with exactly one of these lines:

```sh
export RADIO_FREQ_MHZ=2440 TX_INTERVAL_MS=10 TX_PACKET_COUNT=10000
export RADIO_FREQ_MHZ=2476 TX_INTERVAL_MS=10 TX_PACKET_COUNT=10000
export RADIO_FREQ_MHZ=2402 TX_INTERVAL_MS=20 TX_PACKET_COUNT=10000
export RADIO_FREQ_MHZ=2440 TX_INTERVAL_MS=20 TX_PACKET_COUNT=10000
export RADIO_FREQ_MHZ=2476 TX_INTERVAL_MS=20 TX_PACKET_COUNT=10000
```

Build and collect with the selected row (run separately for each experiment,
not as an automated flashing loop):

```sh
make build RADIO_FREQ_MHZ="$RADIO_FREQ_MHZ" TX_INTERVAL_MS="$TX_INTERVAL_MS" TX_PACKET_COUNT=10000
# Identify and flash RX, then start this logger before starting TX:
RUN_ID="${RADIO_FREQ_MHZ}-${TX_INTERVAL_MS}-r1"
make monitor-rx PORT="$RX_PORT" \
  MONITOR_FLAGS="--csv experiment-${RUN_ID}.csv --run-id ${RUN_ID}"
# In the second terminal with matching exports, identify and flash TX as above.
```

Change `r1` to `r2`, etc., on repeats; existing CSV paths are deliberately refused.
For monitoring without CSV, the original command remains:
`make monitor-rx PORT="$RX_PORT"`. No serial dependency needs installing.
