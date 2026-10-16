# nRF52840 Wireless Imaging — Milestone 2

Bare-metal C11 synthetic throughput benchmark for two Seeed XIAO nRF52840 boards.
The shared proprietary 2 Mbps RADIO driver uses EasyDMA, hardware CRC16,
interrupt-driven double-buffered RX, and TIMER/PPI TX scheduling. Native USB CDC
on RX reports cumulative diagnostics once per second. TX sends three final
telemetry pages so RX can report measured TX timing without adding USB to TX.
No application packet or telemetry page is retransmitted; there are no ACKs.

**Default: 2440 MHz, 0 dBm, 32 application bytes, 10,000 packets, 10 ms interval.**
M2 firmware has been compiled and host-tested; its throughput and reduced RX
turnaround have **not been measured on hardware**. No boards were flashed during
implementation. The 450–600 ms image target remains an experiment question.

See [experiment procedure, timing definitions, and state-machine rationale](docs/experiments.md)
and [software validation and remaining hardware checks](docs/validation.md).
No Arduino runtime, RTOS, BLE/SoftDevice API, heap allocation, or camera/pin
access is included. Existing bootloader, SoftDevice, and flash-layout checks remain.

## First high-speed experiment

After inspecting the build results, connect both boards and double-reset both
into UF2 mode. Close other serial monitors and run from an interactive terminal:

```sh
./scripts/run_experiment.sh --frequency 2440 --payload 32 --interval-us 1000 --packets 10000
```

The runner builds and validates matching images, identifies physical RX and TX,
and asks for the existing `FLASH` confirmation. RX is flashed first; TX is
flashed only after CDC enumeration and a fresh, clean report with matching
configuration and run tag. Unique USB identities are used, never a remembered
mount suffix. Ambiguous identification stops the operation. See the flash
procedure below for the retained safety checks.

Then explore a larger packet and maximum scheduling:

```sh
./scripts/run_experiment.sh --frequency 2440 --payload 240 --interval-us 1200 --packets 10000
./scripts/run_experiment.sh --frequency 2440 --payload 240 --max-throughput --packets 10000
```

The legacy positional CLI remains: `./scripts/run_experiment.sh 2440 10 10000`
means milliseconds. **Payload 240 / interval 500 us is impossible and rejected**:
the packet alone occupies 1024 us on air.

Results in `experiments/<timestamp>_<MHz>_<payload>B_<interval>us_<count>/` include
configuration, frozen UF2s/manifests, raw USB output, per-report `rx.csv`, and
`summary.json` / `summary.csv`. A fresh random 32-bit run tag binds both builds
and on-air packets. TX statistics are valid only when `tx_stats_mask=7`; missing
telemetry leaves measured TX goodput and delivery ratio unavailable, never
replaced by configured values. Ctrl-C preserves partial evidence and reaps children.

## Packet sizes and scheduling

`APP_PAYLOAD_SIZE` is useful synthetic data, excluding the eight-byte header.
The fixed format omits S0/LENGTH/S1. `PCNF1.MAXLEN=STATLEN=payload+8`, both limited
to 255; maximum application data is therefore **247 bytes**. CRC, preamble and
address are additional on-air bytes, not DMA bytes.

| Application bytes | DMA bytes | On-air bytes | Airtime us | Minimum fixed interval us | Feasible requested matrix intervals us |
| ---: | ---: | ---: | ---: | ---: | --- |
| 32 | 40 | 48 | 192 | 212 | 250, 500, 1000, max |
| 64 | 72 | 80 | 320 | 340 | 500, 1000, max |
| 128 | 136 | 144 | 576 | 596 | 1000, max |
| 240 | 248 | 256 | 1024 | 1044 | max |
| 247 | 255 | 263 | 1052 | 1072 | max (extra boundary configuration) |

All integer payloads 32–247 and fixed intervals from their minimum through
60,000,000 us are accepted. The 20 us handoff allowance is an engineering admission
budget, **not verified sustained performance**. Actual late schedules are counted
and rebased; no catch-up burst occurs. Zero selects maximum mode. Neither 150 nor
200 us fits a supported payload plus the admission allowance.

```sh
make build APP_PAYLOAD_SIZE=240 TX_INTERVAL_US=1200 RADIO_FREQ_MHZ=2440 TX_PACKET_COUNT=10000
make build APP_PAYLOAD_SIZE=240 TX_INTERVAL_US=0
make build RADIO_FREQ_MHZ=2440 TX_INTERVAL_MS=10  # legacy units
make test
```

Do not set conflicting microsecond and millisecond values. Generated
`tx_interval_ms` is only a compatibility field (integer division); use
`tx_interval_us` for analysis. `RUN_ID=1` is the direct-build default; choose a new
shared value for manually flashed runs. `TX_PACKET_COUNT=0` runs until the reserved
telemetry sequence boundary (0xfffffff0), then stops instead of wrapping. The
runner requires a positive bounded count. M2 packets/metadata are incompatible
with M1 firmware; rebuild and flash both roles together.

## Matrix without hardware writes

```sh
# Save admission decisions and commands only:
python3 scripts/benchmark_matrix.py --output experiments/m2-plan
# Compile all 10 feasible matrix entries, reject the other 14; never flash:
python3 scripts/benchmark_matrix.py --build --output build/m2-matrix-new
# Explicit operator-run matrix; normal FLASH/identity checks for each entry:
python3 scripts/benchmark_matrix.py --run --output experiments/m2-hardware
```

Output directories must be new. `--payloads`, `--intervals-us` (0=max),
`--frequency`, and `--packets` customize the matrix. `matrix.json` and `matrix.csv`
are saved after each entry. Hardware mode asks the operator to double-reset both
boards between entries; this firmware intentionally has no remote bootloader reset.
Use separate output directories for repeated trials. Builds share `build/` and
must be sequential, including direct Make invocations.

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

Configuration changes regenerate `build/experiment_config.h` and invalidate
role objects/images, including on Make versions with coarse mtime resolution.
Outputs remain `build/{tx,rx}.{elf,bin,uf2,map}` and `.uf2.json` manifests.
Each image is checked against ELF/vector/flash/RAM limits; role, SHA-256 and
embedded M2 configuration are verified again before flashing.

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
does not prove firmware execution; follow the test below. The flasher reports
the failing copy operation, acknowledged byte count, and current submitted span.
Writes are limited to 512-byte UF2 blocks. EIO/ENODEV on the final submitted
block, or during final sync/close after acknowledged writes, returns exit 3
(copy unconfirmed). A failed syscall does not prove zero device delivery.
The runner keeps collecting RX evidence, records the warning,
and never turns an unconfirmed copy into a successful-copy claim. Other copy
errors abort. No copy is automatically retried or redirected to another board.

## Monitor and architecture

```sh
python3 scripts/monitor.py --list
make monitor-rx PORT=/dev/cu.usbmodemYOUR_RX_PORT \
  MONITOR_FLAGS='--csv new-run.csv --run-id my-run'
```

Use the identified RX port; baud 115200 is CDC line coding, not throughput.
The firmware never prints individual packets or waits for a terminal. M2's
logger requires the complete M2 schema; old logs remain historical evidence.
Reports contain configured values, RX validation/loss counters, hardware timing,
USB overlap counters, and final TX telemetry. Definitions are in the experiment
procedure, including the difference between packet delivery and observed gaps.

Shared code remains under `common/`, boot and USB descriptors under `platform/`,
and role loops under `tx/` and `rx/`. RX validates the completed DMA buffer in the
RADIO ISR after starting the alternate buffer. TX prepares its next buffer while
the current one is transmitting. RADIO stays in RXIDLE/TXIDLE between packets;
it is disabled on recovery and at TX completion. TXIDLE can emit the configured
DTX signal, so this benchmark trades continuous radio power/occupancy for speed.
This is a USB-powered bench design, not a low-power duty-cycled link.
