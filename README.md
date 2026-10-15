# nRF52840 Wireless Imaging — Milestone 1.2

Bare-metal C11 firmware for two Seeed XIAO nRF52840 boards. TX sends the existing
deterministic 32-byte packet over proprietary Nordic 2 Mbps RADIO. RX validates
CRC, payload, and sequence progression, and reports once per second over native
USB CDC. Frequency, TX interval, and packet target are now build settings.
Default: **2402 MHz, 10 ms between attempt starts, 10,000 attempts, 0 dBm**.

The user reports successful reception with the preceding firmware, zero payload
failures, and about 1.24% cumulative sequence gaps at one meter. This revision
adds measurement to investigate those gaps; it does not establish their cause.
**No hardware was flashed or measured during this milestone implementation.**

See [the experiment procedure and timing definitions](docs/experiments.md) for
all six required experiments, safe flashing commands, CSV collection, and how
to interpret the measurements. [Validation results](docs/validation.md) record
the builds, host tests, memory use, and remaining hardware checks.

No Arduino runtime, RTOS, BLE, SoftDevice calls, dynamic allocation, camera
initialization, or camera-pin access is included. The installed SoftDevice and
bootloader remain in flash. TX does not provide USB serial in application mode.

## One-command dual-board experiment (macOS)

Connect both boards and double-press each reset button to enter UF2 mode. Close
other RX monitors, then run from an interactive terminal:

```sh
cd ~/Documents/Code/nrf52840-wireless-imaging
./scripts/run_experiment.sh 2440 10 10000
```

Omitted trailing arguments default to **2402 MHz, 10 ms, 10000 attempts**.
The runner builds and validates both images, physically identifies the boards,
and requires you to type `FLASH` before writing firmware. On first use it asks
you to select each current mount and unplug/reconnect that physical board to
verify the selection. Unique bootloader USB serials are saved in
`experiments/boards.json`; volume names are never saved as reusable identities.
Unverifiable mappings require physical identification again. Keep cables and
USB ports fixed after confirmation.

RX is flashed first. Its CDC port is discovered from USB ancestry and physical
location, then the existing monitor/CSV logger starts. TX is flashed only after
a fresh complete RX report confirms matching settings, readiness, and zero
prior packet activity. Both copied images carry matching embedded configuration
metadata; SHA-256 hashes identify the exact firmware in the results.

Results are saved under `experiments/<timestamp>_2440MHz_10ms_10000/` as
`config.json`, `rx.log`, `rx.csv`, and `summary.json`, with `runner.log` and copies
of both validated UF2s/manifests. Ctrl-C stops/reaps child processes and saves an
interrupted summary. Nothing erases the chip or changes the verified application
region `[0x27000, 0xEA000)`.

Exit 0 means the final target sequence was observed followed by stable idle
reports. Exit 2 means incomplete tail evidence or a timeout; a successful UF2
copy never proves packet transmission. See [runner behavior and completion
criteria](docs/experiments.md#automated-runner-milestone-12) for identity handling,
timeouts, recovery, and measurement limitations. Hardware flashing and a full
two-board run remain to be validated by the operator.

## Build on macOS

Requirements: Make, a host C compiler for tests, Python **3.12+**, and an ARM GCC
toolchain with binutils and newlib. The full Arm toolchain is available via the
[Homebrew gcc-arm-embedded cask](https://formulae.brew.sh/cask/gcc-arm-embedded):

```sh
xcode-select --install                 # If command-line tools are missing
brew install python
brew install --cask gcc-arm-embedded
make build
make test
```

`make` prefers `arm-none-eabi-gcc` on PATH. On this machine it also detects the
existing Teensy ARM GCC installation under `~/Library/Arduino15/packages/teensy/`.
Only the compiler, binutils, libc memory/string routines, and compiler support
library are used from that installation; no Teensy or Arduino firmware is linked.
To select a toolchain explicitly:

```sh
make build CROSS_COMPILE=/absolute/path/to/bin/arm-none-eabi-
```

Run `make clean` when changing compiler or compiler flags. CPU flags are
`-mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard`, with C11, `-Os`,
section garbage collection, debug symbols, and warnings treated as errors.
The vector initializer uses GCC's designated range extension to C11.

The first build downloads SHA-256-verified release archives into `.cache/` and
extracts them into `deps/`. Subsequent builds work offline. No pip packages or
git submodules are required. `make deps` can fetch dependencies separately.

| Dependency | Pin | Used for |
| --- | --- | --- |
| [TinyUSB](https://github.com/hathach/tinyusb/tree/0.17.0) | 0.17.0 | CDC ACM, USB device core, nRF5x peripheral driver |
| [Nordic nrfx/MDK](https://github.com/NordicSemiconductor/nrfx/tree/v2.11.0) | v2.11.0 | Register definitions, system errata, inline USB clock/errata helpers |
| [ARM CMSIS](https://github.com/ARM-software/CMSIS_5/tree/5.9.0) | 5.9.0 | Cortex-M4 core definitions and intrinsics |

Archive hashes and URLs are in `scripts/deps.py`. Upstream licenses remain in
the extracted dependencies: TinyUSB MIT, Nordic BSD-3-Clause, CMSIS Apache-2.0
and component notices. The bundled newlib/libgcc licenses come with the selected
toolchain. No nrfx peripheral driver implementation or board support package is
linked. `platform/system.c` uses Nordic's revision-gated SystemInit with optional
UICR-writing and GPIO/trace options forbidden.

`scripts/prepare_usb.py` produces a local copy of the pinned TinyUSB nRF5x driver
in `build/`: it leaves HFXO running on USB detach because RADIO owns the clock,
and bounds the port's two hardware startup waits. The dependency tree is not
edited. USB VBUS detection, regulator readiness, pull-up sequencing, NVIC setup,
device descriptors, FICR-based serial number, and USBD interrupt handler are all
integrated explicitly. USB uses TinyUSB's example VID/PID `CAFE:4001` for local
development; allocate an appropriate identity before distributing a product.

Available targets:

```sh
make build        # Both roles
make tx
make rx
make test         # Protocol/timing + configuration/CSV/image/flashing tests
make clean        # Deletes build/ only; retains dependencies and archive cache
```

Experiment settings apply to **both** images. Both boards must be rebuilt and
flashed with the same settings; RX cannot read TX settings over the unchanged
packet format. Keep settings on flash commands too, or export them as shown in
the experiment procedure:

```sh
make clean
make build RADIO_FREQ_MHZ=2402 TX_INTERVAL_MS=10 TX_PACKET_COUNT=10000
make build RADIO_FREQ_MHZ=2440 TX_INTERVAL_MS=20 TX_PACKET_COUNT=10000
make build RADIO_FREQ_MHZ=2476 TX_INTERVAL_MS=50 TX_PACKET_COUNT=10000
```

Changes regenerate `build/experiment_config.h`, invalidate generated role objects/images,
and rebuild both roles even without
`make clean`. Decimal integers only: frequency 2360–2500 MHz, interval 1–60000 ms,
packet count 0–4294967295. `TX_PACKET_COUNT=0` selects the previous continuous
operation. Unsupported values fail before compilation. Keep separate builds
sequential; concurrent builds of different configurations share `build/`.

Outputs: `build/{tx,rx}.{elf,bin,uf2,map}` and `build/{tx,rx}.uf2.json` manifests.
The manifest binds each UF2's hash to its role, embedded experiment configuration,
and safe memory layout. Keep it
beside the UF2 when using `flash.py`. Every generated image is checked against
its ELF load segments, vector table, flash boundaries, and SRAM/stack limits.

## Flash layout and boot

The application occupies **`[0x27000, 0xEA000)`**. This was verified from each
connected board's read-only `CURRENT.UF2`, including the installed SoftDevice's
size/version structure, and cross-checked against the upstream bootloader.
It is not inferred solely from a generic S140 version table.

See [the flash-layout evidence and memory map](docs/layout.md) for source links,
the observed metadata, preserved regions, and why the first 32 KiB of SRAM is
reserved. The linker leaves 8 KiB at the top of SRAM for the stack. Startup copies
`.data`, clears `.bss`, resets interrupt state, enables the FPU through Nordic
SystemInit, and installs the application's vector table directly in `SCB->VTOR`.
No SoftDevice API is called or enabled by the application.

The flasher rechecks the selected board's layout on every run. A changed
bootloader, SoftDevice, or exposed flash boundary disables flashing until the
new configuration is audited. Do not bypass this by manually copying a different
image or changing only the linker origin.

## Identify and flash one board at a time

Double-press reset to enter the existing UF2 bootloader. Discover mounts:

```sh
python3 scripts/flash.py --list
```

Identify the physical camera board as TX and the bare board as RX. If there is
any uncertainty, disconnect one USB cable, list again, reconnect, and list again
to establish the current mapping. **Reconnection can change either mount name.**
Both boards share the same `Board-ID`; that field is a model check, not a unique
identity. This project deliberately does not remember a role from a volume name.

Use the mount you have just identified for each role. These commands deliberately
require explicit mount variables and preserve exported experiment settings:

```sh
RX_VOLUME='/Volumes/REPLACE_WITH_CURRENT_RX_MOUNT'
make flash-rx VOLUME="$RX_VOLUME" FLASH_FLAGS=--dry-run
make flash-rx VOLUME="$RX_VOLUME"

# List again and identify the camera board; mount names can change.
python3 scripts/flash.py --list
TX_VOLUME='/Volumes/REPLACE_WITH_CURRENT_TX_MOUNT'
make flash-tx VOLUME="$TX_VOLUME" FLASH_FLAGS=--dry-run
make flash-tx VOLUME="$TX_VOLUME"
```

Replace `VOLUME` whenever discovery shows a different mapping. Omitting it is
an error, even if only one board is connected. The script prints the selected
mount, board metadata, role, image, and allowed flash range before writing a
single UF2 file to that mount. `--dry-run` performs no writes. No tool in this
project erases the chip or writes UICR/SoftDevice/bootloader payloads. The existing
bootloader itself performs its normal application-update bookkeeping.

The volume should disappear after a successful bootloader update. TX then runs
without a USB interface. RX enumerates as **nRF52840 Imaging RX**, with a serial
number derived from its immutable FICR DEVICEID. A completed file copy alone
does not prove firmware execution; follow the test below. If a copy errors during
automatic ejection, inspect the board before retrying; the script does not retry
or choose another board automatically.

## Monitor RX

After RX application enumeration:

```sh
python3 scripts/monitor.py --list
make monitor-rx PORT=/dev/cu.usbmodemYOUR_RX_PORT
```

Use the exact path printed by `--list`. With multiple serial devices present,
identify RX by its USB product/serial in macOS System Information or by observing
which port appears when RX is connected. The script never guesses. It sets raw
mode, 115200 baud, and DTR/RTS using the Python standard library; baud is only CDC
line coding, not the USB transfer speed. Stop with Ctrl-C. After disconnection,
list ports and reopen it. This firmware does not implement a 1200-baud reset;
double-press the physical reset button to return to UF2 mode.

The banner appears when a terminal opens with DTR and includes `freq_mhz`,
`tx_interval_ms`, `packet_target`, and `tx_power_dbm`. The interval and target
are RX's compiled expectations, not measurements of TX. Each `RX` line retains
the original counters and adds timing, duration, and observed gap statistics.

To create a CSV while also displaying the diagnostics:

```sh
make monitor-rx PORT=/dev/cu.usbmodemYOUR_RX_PORT \
  MONITOR_FLAGS='--csv experiment-2402-10-r1.csv --run-id 2402-10-r1'
```

The CSV must be a new filename. The logger uses only Python's standard library,
records UTC host timestamps and an explicit host run label, and buffers fragmented
serial lines. Partial or malformed reports are excluded. It does not infer an
on-air run identifier. Reset RX before each run, start the logger, then reset/start
TX. Follow [the full controlled procedure](docs/experiments.md), including waiting
for final stable counters. Opening a monitor does not reset firmware counters.

`valid` includes duplicates and backwards packets. `crc_fail` and `payload_fail`
do not advance sequence tracking. `gaps` counts missing sequence values between
validated packets; do not add CRC failures to it. The first valid packet establishes
the baseline, so preceding and trailing losses are not counted. `backwards`
retains the original behavior of rebasing after backwards movement; such runs
have `gap_rate_valid=0` and should be repeated. Sequence arithmetic handles uint32
wrap; the original counters still wrap modulo 2^32. New observed-span accounting
uses uint64. See the experiment document for the exact rate denominator.

`radio_errors` retains the combined error/rearm-failure counter. `rearm_failures`
is its overlapping subset, not another quantity to add. `radio_ready=1` means
software has armed RX; a completed packet may already be waiting for polling.
`usb_errors` still counts hardware/startup faults. `reports_skipped` counts lines
not accepted by the USB FIFO. Firmware never waits for the terminal or prints
individual packets. The TX FIFO is now 2048 bytes to fit one extended report;
USB architecture and nonblocking write behavior are unchanged.

## Driver and protocol details

The driver uses a 4-byte-aligned static SRAM EasyDMA buffer. `PCNF0` contains no
S0/LENGTH/S1 fields and uses the proprietary 8-bit preamble. `PCNF1` fixes both
STATLEN and MAXLEN at 32, BALEN at 4 (five-byte logical address), little-endian
bit ordering, and no whitening. Address zero uses `BASE0=0x89ABCDEF`,
`PREFIX0=0xC3`, `TXADDRESS=0`, and `RXADDRESSES=1`. CRC is two bytes, polynomial
`0x11021`, initial value `0xFFFF`, including the address. TX power is 0 dBm.
These fields follow the [Nordic RADIO specification](https://docs.nordicsemi.com/r/bundle/ps_nrf52840/page/radio.html).

Packet bytes 0–3 are a little-endian uint32 sequence. For every byte index `i`
from 4 to 31:

```text
payload[i] = low8(0xA5 XOR (29*i) XOR (sequence >> (8*(i mod 4))))
```

TX increments sequence once per attempt, without acknowledgements or retries.
`tx_attempts`, `tx_failures`, `tx_recovery_failures`, and `tx_complete` are volatile globals
available through SWD. No camera, image, or frame buffer exists in this milestone.

TIMER0 is a free-running 32-bit 1 MHz timebase (capture 0 in foreground,
capture 1 in the USBD interrupt to prevent timestamp overwrite). Unsigned
subtraction handles its roughly 71-minute wrap for all short deadlines. HFXO
startup is verified with CLOCK task/event/status registers and a 100 ms timeout.
RADIO ramp, completion, and disable waits have 2 ms bounds. A silent receiver
remains armed indefinitely, servicing USB in the polling loop; a started but
unfinished packet times out. Errors cause recovery attempts, not unbounded
peripheral waits. Clock failure before USB startup records fault code 2 and
halts for SWD diagnosis; no USB output is promised when its clock is unavailable.

`READY_START` and `END_DISABLE` shortcuts control each packet. RX copies the
finished payload after DMA stops and rearms before payload checks and USB work.
There is a receive gap during turnaround. TIMER2/PPI now captures END, RXEN
issuance, and READY to measure that gap, while a software capture marks CPU
handling. No RADIO ISR, double buffering, DMA chaining, or retransmission was
added. USB interrupts and foreground work can still delay polling; their elapsed
times and END overlap counters are reported. Timing definitions and resource
ownership are in [the experiment document](docs/experiments.md).

Shared drivers live in `common/`, reset/memory/USB descriptors in `platform/`,
and role loops in `tx/` and `rx/`. Future camera capture, larger packets,
buffer ownership, scheduling, selective retransmission, and frame integrity can
extend those interfaces. They are intentionally outside Milestone 1.
