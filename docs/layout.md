# Verified application layout

Evidence gathered read-only on **2026-10-15**. Both attached devices reported:

```text
UF2 Bootloader 0.6.2-12-g459adc9-dirty lib/nrfx (v2.0.0) lib/tinyusb (0.10.1-293-gaf8e5a90) lib/uf2 (remotes/origin/configupdate-9-gadbb8c7)
Model: Seeed XIAO nRF52840
Board-ID: Seeed_XIAO_nRF52840_Sense
Date: Nov 30 2021
SoftDevice: S140 7.3.0
```

The temporary mounts were `/Volumes/XIAO-SENSE` and `/Volumes/XIAO-SENSE 1`.
Those names are observations only, not stable device or role identifiers.

## Installed-device evidence

Each `CURRENT.UF2` was 1,908,736 bytes, representing 3,728 (`0xE90`) blocks
with 256-byte flash payloads. The first block targets `0x1000`; the last targets
`0xE9F00`. The exposed region therefore ends at **`0xEA000`**, exclusive.
The current UF2 family field is Seeed's board-specific `0x28860045`.
Generated application UF2 files instead use the generic nRF52840 application
family `0xADA52840`, which the upstream bootloader accepts.

Reading the block at flash `0x3000` gave these words on **both** devices:

| Address | Value | Meaning |
| --- | --- | --- |
| `0x3000` | `0xFFFFFF2C` | Information structure size byte is `0x2C` |
| `0x3004` | `0x51B1E5DB` | SoftDevice magic |
| `0x3008` | `0x00027000` | Installed SoftDevice size / application origin |
| `0x300C` | `0xFFFF0123` | Firmware ID in low 16 bits |
| `0x3010` | `0x0000008C` | SoftDevice ID 140 |
| `0x3014` | `0x006ADB78` | Version 7,003,000 = 7.3.0 |

The bootloader's `SD_SIZE_GET(MBR_SIZE)` reads exactly the size word above.
Thus `0x27000` is verified from installed flash, not assumed from the label.
The `dirty` build string means the binary is not claimed to be bit-identical to
an upstream release. Live metadata and exported region boundaries are checked in
addition to the source cross-check. No full dump is saved and no board writes
were performed during verification.

## Authoritative source cross-check

- [Nordic SoftDevice information definitions, bundled by upstream bootloader 0.6.2](https://github.com/adafruit/Adafruit_nRF52_Bootloader/blob/0.6.2/lib/softdevice/s140_nrf52_6.1.1/s140_nrf52_6.1.1_API/include/nrf_sdm.h):
  the structure starts at `MBR_SIZE + 0x2000`, and size is at offset `+8`.
  This header is evidence for the metadata format, **not** a claim that 6.1.1
  is installed; the live version and size words identify the installed image.
- [Bootloader application launch](https://github.com/adafruit/Adafruit_nRF52_Bootloader/blob/0.6.2/lib/sdk11/components/libraries/bootloader_dfu/bootloader.c):
  `bootloader_app_start()` gets the application origin from installed SD metadata.
- [UF2 configuration](https://github.com/adafruit/Adafruit_nRF52_Bootloader/blob/0.6.2/src/usb/uf2/uf2cfg.h)
  and [CURRENT.UF2 generation](https://github.com/adafruit/Adafruit_nRF52_Bootloader/blob/0.6.2/src/usb/uf2/ghostfat.c):
  exported flash spans `MBR_SIZE` through `BOOTLOADER_REGION_START - DFU_APP_DATA_RESERVED`.
- [nRF52840 bootloader linker](https://github.com/adafruit/Adafruit_nRF52_Bootloader/blob/0.6.2/linker/nrf52840.ld):
  bootloader starts at `0xF4000`; configuration, MBR parameters, settings, and
  double-reset RAM locations are separately reserved.
- [Bootloader build configuration](https://github.com/adafruit/Adafruit_nRF52_Bootloader/blob/0.6.2/Makefile):
  nRF52840 reserves `10 * 4096` bytes of application data below `0xF4000`.
  `0xF4000 - 0xA000 = 0xEA000`, matching both devices' exported end address.

The linked upstream source was downloaded and inspected locally. The live
evidence is also rechecked by `scripts/flash.py` during every dry run and write.
Any unknown layout is rejected; changing a constant alone is not verification.

## Flash map (end addresses exclusive)

| Region | Start | End | Application policy |
| --- | --- | --- | --- |
| MBR | `0x00000` | `0x01000` | Preserve |
| Installed S140 | `0x01000` | `0x27000` | Preserve, never enable |
| Application | `0x27000` | `0xEA000` | Only generated UF2 target range; 780 KiB |
| Reserved application data | `0xEA000` | `0xF4000` | Preserve; 40 KiB |
| Bootloader executable | `0xF4000` | `0xFD800` | Preserve; 38 KiB |
| Bootloader configuration | `0xFD800` | `0xFE000` | Preserve; 2 KiB |
| MBR parameter page | `0xFE000` | `0xFF000` | No application writes |
| Bootloader settings | `0xFF000` | `0x100000` | No application writes; bootloader owns update bookkeeping |
| UICR | `0x10001000` | outside main flash | Never emitted or written |

All firmware load bytes must lie in the application region. The flasher rejects
bootloader-family UF2s, unknown flags, out-of-range or noncontiguous blocks,
inconsistent sizes/counts, bad vectors, and role/hash mismatches. Padding the
last block to 256 bytes is also checked to stay below `0xEA000`.

## SRAM and reset

| Region | Start | End | Use |
| --- | --- | --- | --- |
| Preserved low RAM | `0x20000000` | `0x20008000` | MBR forwarding/bookkeeping and bootloader retained state |
| Application data/BSS/free RAM | `0x20008000` | `0x2003E000` | Static allocations, no heap |
| Reserved stack | `0x2003E000` | `0x20040000` | 8 KiB, initial MSP `0x20040000` |

The low 32 KiB includes the bootloader double-reset word at `0x20007F7C` and
retained bond-sharing area `[0x20007F80, 0x20008000)`. It is deliberately excluded
from `.data`/`.bss`, although the application does not enable the SoftDevice.
This leaves 224 KiB total application SRAM, including its stack.

The existing bootloader loads MSP and branches to the reset vector. The
application's 256-byte, 256-byte-aligned vector table is at `0x27000`; every
interrupt slot has a handler and USBD IRQ 39 has its actual handler in RX.
Reset clears stale NVIC/SysTick state and sets VTOR directly, bypassing
SoftDevice interrupt forwarding. Early fault codes are available through
`platform_fault_code` in SRAM: 1 = main returned, 2 = HFXO startup, 3 = TX radio
initialization, 4 = Nordic helper assertion, 5 = USB stack initialization;
`0x100 + IPSR` identifies an unexpected exception. These faults halt safely
without touching camera GPIOs. No firmware-side flash programming is used.

Remaining verification: run the actual UF2 handoff, CDC enumeration, and RF
test on the physical boards. Source and image validation establish placement,
not successful execution, measured stack margin, or packet-delivery reliability.
