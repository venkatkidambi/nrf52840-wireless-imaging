# Milestone 4.1 control audit (before timing changes)

Source inspected: `common/image_protocol.c`, `common/image_radio.c`,
`common/radio.c`, `common/radio_timing.c`, `rx/image.c`, and recorded experiments.

## Complete sequence and waits

1. START/READY negotiates frame metadata. TX waits for READY and then a 3000 us
   guard from its received hardware END. Generation and TX CRC precede START.
2. TX pipelines 434 DATA packets at the selected interval (1100 us preferred,
   1500 us fallback). The last completion is drained before serializing END.
3. TX sends END in TXIDLE, then disables/enables RX and waits for READY locally.
   RX detects the burst boundary by dequeuing this explicit END. It does not
   wait for silence or the last DATA index. Losing the last DATA or END is safe:
   the former appears in the bitmap; the latter causes an END retry.
4. RX validates frame/token, scans 434 bits, and serializes one 55-byte bitmap.
   It waits until request hardware END + 3000 us, sends one fixed 248-byte
   response, and immediately returns to RX. Processing overlaps the guard.
5. TX accepts only matching frame/token/type, then waits until response END +
   3000 us. It copies the bitmap and prepares the next requested DATA packet.
   Each repair burst uses the same packet interval, drains, and sends END again.
6. When all segments exist, END processing computes full-frame CRC32 before
   COMPLETE. The guard has usually expired during CRC, so COMPLETE sends
   immediately after preparation. Repeated END reuses verified immutable state.
7. TX validates COMPLETE CRC, waits its response-END guard, and freezes reliable
   completion telemetry. It sends COMMIT proving receipt of COMPLETE. RX accepts
   COMMIT only after verification, returns COMMIT_ACK, and permits USB export.
   TX waits another guard after ACK; this exchange is outside frozen TX stats.
   Lost COMPLETE repeats END; lost COMMIT/ACK repeats COMMIT idempotently.

All control packets occupy 1024 us at the existing fixed RF packet size. Guards
are anchored to received END, not to synchronized clocks. Local direction
enable measurements (~50–55 us) do not bound remote foreground or IRQ delay.

| Existing wait/bound | Purpose |
| --- | --- |
| 3000 us on RX requests and TX replies | Peer direction/processing allowance |
| 120000 us × 8 END attempts | Verification and lost feedback recovery |
| 20000 us × 48 START/COMMIT attempts | Short control recovery; same 960000 us allowance |
| Last 10000 us of reply window suppressed | Avoid late replies colliding with next request |
| 16 repair rounds | Bound permanent DATA loss |
| 5 s idle / 30 s frame | Bound inactive or stalled receiver |
| RADIO event waits 2000 us | Fault bound for direction events |
| TX completion remaining schedule + 3000 us | Driver fault bound, not normal guard |
| RX ADDRESS watchdog 2000 us | Detect stalled reception |
| TIMER3 compare lead 8 us / max-mode handoff 20 us | Existing DATA scheduler bounds |
| TIMER1 wake 1 ms | Driver waiting; control polling does not sleep |
| USB READY 1 s, startup 100 ms, foreground writes ≤512 bytes | Existing USB service |
| Runner readiness 20 s / image 90 s, polling 100 ms | Host lifecycle, outside RF RTT |

## Why the maximum RTT is about 30.8 ms

Recorded 1100-us trials `2026-10-16_013552_266108-0400_2440MHz_240B_1100us_434`
and `2026-10-16_013935_124281-0400_2440MHz_240B_1100us_434` contain RTT maxima
30788/30790 us and RX verification 25592 us. In the code the successful RTT
starts before sending END and ends after the received COMPLETE guard. Thus its
path includes request airtime, foreground CRC, response turn/airtime and the
3000 us return guard. The approximate residual after 25592 + 2×1024 + 3000 is
148/150 us. This is a derived cross-trial-duration consistency check, not an
independently measured phase or subtraction of cross-board timestamps.

No 30.8 ms timer exists. Failed attempts are excluded from successful RTT
samples; END has a 120 ms timeout. The code path and recorded CRC duration
identify verification as the dominant contributor, not a scheduled feedback
window. Existing aggregates cannot isolate every remaining microsecond; the
new phase measurements are needed to verify that decomposition on hardware.

## Decisions

Preserve explicit END, bitmap, full CRC, retry allowance and verification window.
No safe earlier feedback opportunity exists before END without another protocol
change. Test request/reply guards independently: baseline 3000/3000 us,
balanced 3000/1500 us, aggressive 1500/1000 us. Keep the request side more
conservative because RX may be servicing USB. The reply guard chiefly protects
RX's immediate return to listen after sending feedback. These are experimental
engineering budgets, not established safe RF timings. Reject <1000 us; 500 us
has insufficient evidence for foreground/IRQ tail latency. The unchanged
10 ms late-response exclusion also protects retransmitted control windows.

## Post-audit terminal guard change

Balanced/aggressive also omit the TX guard after matching COMMIT_ACK. TX next
disables RADIO and sleeps; no subsequent transmission can collide with the peer's
return to RX. Baseline keeps that guard. This removes a redundant successful-path
wait without shortening any retry allowance. The final TX timing stays locally
inspectable because COMMIT already carried the exported telemetry snapshot.
