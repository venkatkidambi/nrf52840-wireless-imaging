# Radio image v1.0 freeze candidate — Milestone 4.1

Software validation is complete; physical validation of this revision is pending.
Do not create the recommended tag `radio-image-v1.0` until the hardware gate below
passes. No commit, tag, or physical flash was performed during implementation.

## Frozen configuration and protocol

Two Seeed XIAO nRF52840 boards, TX with the connected but unused HM01B0, RX on
macOS USB CDC. Proprietary 2 Mbps, 2440 MHz, 0 dBm. Installed UF2 bootloader
0.6.2 / S140 7.3.0; application flash `[0x27000,0xEA000)`. The SoftDevice remains
installed but is not used by the application. No camera, compression, heap,
RTOS, BLE API, or pin changes are introduced.

The user-verified 1100 us trials completed in 534552 and 553877 us with initial
missing counts 9/18, recovery rounds 1/2, retransmissions 9/19, no missed TX
deadlines, and fully verified frames. These are individual successes, not a
latency distribution. Prefer 1100 us; keep 1500 us as the conservative fallback.
Existing no-option image commands retain their 1500 us default for compatibility.

The fixed 248-byte DMA packet is unchanged: version:u8, type:u8, frame:u32,
round-token:u16, index:u16, body-length:u16 (little endian), then 236 body bytes
with zero padding. On-air preamble/address/CRC add eight bytes (1024 us total).
434 DATA segments carry 102400 pixels; index 433 carries 212 bytes. Hardware
CRC16 protects packets; IEEE CRC32 and independent host comparison protect the
whole frame. START carries geometry/CRC/pattern metadata. END explicitly hands
over the channel; one 55-byte MISSING bitmap requests selective repair. COMPLETE
carries the verified CRC; COMMIT proves TX receipt before RX exports the image.
See the [pre-change audit](control-audit.md) and [full protocol](image-transfer.md).

## Control profiles

| Profile | RX request-END guard | TX reply-END guard | TX terminal ACK guard |
| --- | ---: | ---: | ---: |
| baseline (default) | 3000 us | 3000 us | 3000 us |
| balanced (experimental) | 3000 us | 1500 us | none |
| aggressive (experimental) | 1500 us | 1000 us | none |

Select `--control-profile` in the runner or `CONTROL_PROFILE` in Make. Selection
is embedded in both artifacts, checked before copying and in RX readiness, and
saved in experiment metadata. Balanced/aggressive default to 1100 us when no
interval is provided. Explicit legacy interval/max settings remain supported;
do not use faster DATA intervals in this milestone's physical comparison.

Request guards protect TX entering RX after END; reply guards protect RX
returning to listen after its response. The latter is a shorter path without
another CRC or bitmap scan. 1000 us is the enforced minimum candidate, roughly
18× the observed 55 us local transition, leaving about 945 us for remote software,
IRQ delay and timer uncertainty. This is an engineering allowance, **not a proven
safe bound**: driver fault timeouts alone can exceed it. Hardware loss/retry tails
must validate each candidate. Values outside 1000–3000 us fail compilation; 500 us
is excluded. Supported profiles, rather than raw macro overrides, are the
reproducible configuration interface.

Successful COMPLETE already sends as soon as verification/preparation and the
request guard finish; CRC typically consumes that guard. Reduced reply guards
shorten return to repair DATA and COMMIT. Experimental profiles skip the terminal
COMMIT_ACK guard because TX has no subsequent transmission; this does not alter
the reliable completion event or the host integrity gate. No new control messages
or unsolicited feedback windows were added.

END retains 120 ms × 8 attempts; START/COMMIT retain 20 ms × 48 attempts. Each
keeps the original 960 ms timeout allowance, plus packet/turn processing. RX
suppresses replies within the last 10 ms of the peer's window. Sixteen repair
rounds, five-second idle and 30-second frame limits remain. Lost last DATA,
END, feedback, repair DATA or COMPLETE recover through the existing bounded
requests. Duplicate END/COMPLETE reuse verified state. Stale frame/token/type
responses cannot advance TX. Exhaustion reports failure; no incomplete frame
qualifies as success. If all COMMITs are lost, RX times out; if all ACKs are lost,
RX can still export the verified image because it received proof of COMPLETE.

## Added measurements

Every row below appends `_n`, `_min_us`, `_mean_us`, `_max_us` to its field name.
Mean uses a 64-bit sum and integer division. Counts saturate at UINT32_MAX; zero
count means no event, not zero latency. Existing 45 telemetry words retain their
order and meaning; 52 new words bring RESULT to 97 words / 388 bytes. All new
work runs in foreground, using the existing TIMER3 clock and captured events.

| Field stem | Board and exact boundaries |
| --- | --- |
| tx_data_end_to_rx_ready | TX last DATA hardware END → return from RX enable; includes explicit END packet, drain/processing and local turn; one sample per burst |
| tx_feedback_wait | TX END-request hardware END → accepted MISSING hardware END; successful attempts only, no return guard |
| tx_feedback_to_data | TX accepted MISSING hardware END → first repair DATA START; includes guard, bitmap copy, serialization and turn |
| tx_retransmit_burst | TX first repair DATA START → last repair DATA END; one sample per nonempty repair burst |
| tx_complete_wait | TX END-request hardware END → matching COMPLETE hardware END; includes remote verification, no return guard |
| tx_reply_guard | TX guard-call entry → exit, after validation/measurement; actual remaining busy wait, START/END responses only |
| rx_end_detect | RX explicit END hardware END → foreground packet dispatch; queue/service latency, independent of whether final DATA arrived |
| rx_feedback_prepare | RX bitmap initialization/scan → MISSING serialization complete; excludes packet validation and guard |
| rx_feedback_send | RX MISSING captured START → END; excludes preparation, guard, and direction ramp |
| rx_frame_verify | RX full-frame CRC call entry → return; separate from legacy rx_verify_us covering whole request processing |
| rx_complete_prepare | RX COMPLETE body preparation → serialization complete; excludes CRC and guard; includes repeated COMPLETEs |
| rx_complete_send | RX COMPLETE captured START → END |
| rx_confirmation | RX latest COMPLETE END → first COMMIT_ACK END; includes any lost-COMMIT retries, peer guard/preparation, COMMIT airtime and RX ACK guard/airtime |

RX cannot observe the true end of a missing final DATA packet. Explicit END is
the observable handoff boundary, so `rx_end_detect` deliberately does not estimate
the unobserved DATA timestamp. TX records the complete DATA-END-to-listen span.
`rx_confirmation` does not prove ACK reception at TX. No cross-board timestamps
are subtracted. Intervals overlap: RX CRC is inside TX COMPLETE wait, and TX guard
is inside feedback-to-DATA. Do not add them as independent phases.

COMMIT now has a 212-byte body: the original 29 words plus 24 TX phase words.
RX still accepts legacy 52/116-byte COMMITs; host accepts 96/180/388-byte RESULTs
and 52/56-byte READY records. New READY appends a profile ID (0/1/2). Old firmware
does not understand the extended COMMIT; always build matching TX/RX via the
existing runner. The new embedded image identity is CONFIG4; benchmark CONFIG2
and old image CONFIG3 parsing remain supported. Flasher safety/copy logic is
unchanged.

TX snapshots exported telemetry before COMMIT, as before. Its final ACK receipt
and terminal wait cannot be returned through that already-sent snapshot. For
debugger characterization, `image_radio_confirmation` and
`image_radio_terminal_guard` retain count/min/max/sum (mean = sum/count) for
successful COMMIT attempts, including terminal guard. Experimental terminal
guard samples contain only timer/read bookkeeping. The CSV uses the independently
measured RX confirmation span; it never invents a TX final RTT. Lost ACK retries
after the snapshot remain outside host `control_retries`, which retains its
original pre-COMPLETE meaning. No extra reporting handshake is added solely to
measure itself.

## Validation and memory

Build-only matrix (new output directory required; never flashes):

```sh
python3 scripts/performance_suite.py --control-profiles --build --output build/m4.1-freeze
make test
make test HOST_CFLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer -g'
```

All native suites and 95 Python tests pass, including all three protocol/adapter
profiles under ASan/UBSan. Coverage includes final initial DATA loss, repeated
missing requests, lost feedback/COMPLETE/COMMIT/ACK, repeated COMPLETE without
another CRC, stale token/type, permanent loss/retry exhaustion and bounded idle
failure, checksum mismatch, wrap/saturation, guard bounds and terminal guard,
new/legacy USB lengths, unchanged benchmark CLI, board identity and UF2 safety.
Native mocks test ordering and bounds, not physical radio timing or IRQ tails.

Both roles built for baseline/balanced/aggressive at 1100 us, baseline at 1500 us,
and benchmark default/240-byte boundary/247-byte max. ELF/UF2 checks enforce the
flash reservation and 8 KiB stack; symbol audits check aligned SRAM DMA buffers,
fixed framebuffer size, and absence of heap/camera symbols in all seven pairs.
Stack reservation is not a measured stack high-water mark.

| Image role/profile | Flash bytes | Static SRAM bytes | Reserved stack |
| --- | ---: | ---: | ---: |
| TX baseline (1100/1500) | 9252 | 106384 | 8192 |
| TX balanced | 9264 | 106384 | 8192 |
| TX aggressive | 9260 | 106384 | 8192 |
| RX all profiles | 18800 | 112672 | 8192 |

Each board retains one 102400-byte framebuffer, the same aligned DMA pairs and
eight-slot queue (seven usable). RX retains the bitmap, foreground packets and
4096-byte CDC FIFO. New counters and the expanded USB metadata buffer are static.
No ownership or ISR handoff changes were made. Artifacts and manifests in the
build matrix contain exact sizes and hashes; existing experiment files are intact.

## Physical validation gate

Keep frequency, 0 dBm power, board separation/orientation, USB port/cable/hub and
host reader constant. Record geometry and interference conditions. Reset both
boards between trials; use the existing interactive board identification and
RX-first flashing workflow. These commands are **operator actions**, not run by
the build/test suite:

```sh
./scripts/run_experiment.sh --mode image --frequency 2440 --interval-us 1100 --control-profile baseline
./scripts/run_experiment.sh --mode image --frequency 2440 --interval-us 1100 --control-profile balanced
# Only after balanced maintains complete verification:
./scripts/run_experiment.sh --mode image --frequency 2440 --interval-us 1100 --control-profile aggressive
# Conservative fallback:
./scripts/run_experiment.sh --mode image --frequency 2440 --interval-us 1500 --control-profile baseline
```

1. Run baseline at least five times, retaining failures and all output artifacts.
2. Run balanced at least five times after baseline passes. Check every pixel,
   full CRC, TX COMPLETE receipt, missing/retry counts, missed deadlines, queue
   drops and new timing distributions. Recheck baseline if RF conditions change.
3. Attempt aggressive only if balanced succeeds; repeat each successful setting
   at least five times. Include loss-heavy trials, slow host/reconnect checks,
   and observe bounded recovery. Five trials are a screening gate, not proof of
   rare-event reliability; collect more runs when tails or failures differ.
4. Compare success median and worst case, separately from failure rate:

```sh
python3 scripts/compare_images.py experiments --output build/control-trials.csv --summary build/control-summary.json
```

The comparison failure rate is an experiment qualification rate, not an RF packet
loss rate: an unconfirmed UF2 copy can disqualify an otherwise verified frame.
Groups use profile, DATA interval and frequency. Failed/incomplete/unconfirmed
runs stay in the denominator and never enter successful latency means. JSON
includes sample count, mean, median and maximum for each available successful
run metric; phase aggregates are per-run summaries, not pooled event samples.
Unsupported legacy metrics remain absent. Do not mix differing physical setups
in the same comparison; pass selected directories if needed.

Accept a faster profile only with full-frame verification, maintained completion
rate and no materially worse retry/tail behavior. If gains are small or reliability
worsens, retain baseline guards at 1100 us, or the 1500 us fallback. Sub-500 ms
remains aspirational; this revision makes no physical performance claim. Record
the final selected configuration and repeated-trial evidence before explicitly
creating `radio-image-v1.0`. HM01B0 acquisition is the next milestone.
