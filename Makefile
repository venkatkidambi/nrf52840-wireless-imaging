.DEFAULT_GOAL := build
PYTHON ?= python3
CROSS_COMPILE ?= $(shell $(PYTHON) scripts/toolchain.py)
CC := $(CROSS_COMPILE)gcc
OBJCOPY := $(CROSS_COMPILE)objcopy
SIZE := $(CROSS_COMPILE)size
HOST_CC ?= cc
RADIO_FREQ_MHZ ?= 2440
TX_PACKET_COUNT ?= 10000
APP_PAYLOAD_SIZE ?= 32
RUN_ID ?= 1
IMAGE_MODE ?= 0
export RADIO_FREQ_MHZ TX_INTERVAL_MS TX_INTERVAL_US TX_PACKET_COUNT APP_PAYLOAD_SIZE RUN_ID IMAGE_MODE
NRFX := deps/nrfx-2.11.0
CMSIS := deps/CMSIS_5-5.9.0
TUSB := deps/tinyusb-0.17.0/src
ARCH := -mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard
CPPFLAGS := -DNRF52840_XXAA -include build/experiment_config.h -Icommon -Iplatform -I$(NRFX)/mdk \
 -I$(NRFX) -I$(NRFX)/hal -I$(NRFX)/drivers/src -I$(CMSIS)/CMSIS/Core/Include -I$(TUSB)
CFLAGS := $(ARCH) -std=c11 -Os -g3 -ffunction-sections -fdata-sections \
 -fno-common -fno-builtin -fno-unwind-tables -fno-asynchronous-unwind-tables \
 -Wall -Wextra -Werror -MMD -MP
LDFLAGS := $(ARCH) -nostartfiles -nostdlib -Tplatform/linker.ld \
 -Wl,--gc-sections,--build-id=none,--fatal-warnings
COMMON := platform/startup.c platform/system.c common/clock.c common/radio.c common/radio_timing.c common/protocol.c common/experiment_identity.c
COMMON += common/image_protocol.c common/image_radio.c
USB := common/usb.c platform/usb_descriptors.c $(TUSB)/tusb.c \
 $(TUSB)/common/tusb_fifo.c $(TUSB)/device/usbd.c $(TUSB)/device/usbd_control.c \
 $(TUSB)/class/cdc/cdc_device.c
TX_OBJS := $(addprefix build/tx/,$(COMMON:.c=.o) tx/main.o tx/image.o)
RX_OBJS := $(addprefix build/rx/,$(COMMON:.c=.o) $(USB:.c=.o) rx/main.o rx/image.o common/image_usb.o) build/rx/dcd_nrf5x.o

.PHONY: build tx rx deps check-tools clean flash-tx flash-rx monitor-rx test FORCE
build: tx rx
deps:
	@$(PYTHON) scripts/deps.py
check-tools:
	@command -v $(CC) >/dev/null || { echo 'Missing $(CC). Install ARM GCC or set CROSS_COMPILE=/path/to/arm-none-eabi-'; exit 1; }
	@command -v $(OBJCOPY) >/dev/null || { echo 'Missing $(OBJCOPY)'; exit 1; }
	@command -v $(SIZE) >/dev/null || { echo 'Missing $(SIZE)'; exit 1; }
tx rx: check-tools deps build/experiment_config.h
	@$(MAKE) build/$@.uf2 build/$@.uf2.json

$(TX_OBJS) $(RX_OBJS): | check-tools deps
$(TX_OBJS) $(RX_OBJS): build/experiment_config.h
build/experiment_config.h: FORCE scripts/configure.py
	@$(PYTHON) scripts/configure.py $@
FORCE:
build/tx/%.o: %.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@
build/rx/%.o: %.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@
build/dcd_nrf5x.c: scripts/prepare_usb.py | deps
	@mkdir -p build
	$(PYTHON) $< $(TUSB)/portable/nordic/nrf5x/dcd_nrf5x.c $@
build/rx/dcd_nrf5x.o: build/dcd_nrf5x.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@
build/tx.elf: $(TX_OBJS) platform/linker.ld
	$(CC) $(LDFLAGS) -Wl,-Map,build/tx.map $(TX_OBJS) -Wl,--start-group -lc -lgcc -Wl,--end-group -o $@
	$(SIZE) $@
build/rx.elf: $(RX_OBJS) platform/linker.ld
	$(CC) $(LDFLAGS) -Wl,-Map,build/rx.map $(RX_OBJS) -Wl,--start-group -lc -lgcc -Wl,--end-group -o $@
	$(SIZE) $@
build/%.bin: build/%.elf
	$(OBJCOPY) -O binary $< $@
build/%.uf2: build/%.elf build/%.bin scripts/firmware.py
	$(PYTHON) scripts/firmware.py --elf build/$*.elf --bin build/$*.bin --uf2 $@ --role $*
build/%.uf2.json: build/%.uf2
	@test -f $@ || $(PYTHON) scripts/firmware.py --elf build/$*.elf --bin build/$*.bin --uf2 build/$*.uf2 --role $*

flash-tx: tx
	@test -n "$(VOLUME)" || { echo 'Set VOLUME to the explicitly identified TX bootloader mount'; exit 1; }
	$(PYTHON) scripts/flash.py --role tx --firmware build/tx.uf2 --volume "$(VOLUME)" $(FLASH_FLAGS)
flash-rx: rx
	@test -n "$(VOLUME)" || { echo 'Set VOLUME to the explicitly identified RX bootloader mount'; exit 1; }
	$(PYTHON) scripts/flash.py --role rx --firmware build/rx.uf2 --volume "$(VOLUME)" $(FLASH_FLAGS)
monitor-rx:
	@test -n "$(PORT)" || { echo 'Set PORT=/dev/cu.usbmodem... (scripts/monitor.py --list)'; exit 1; }
	$(PYTHON) scripts/monitor.py --port "$(PORT)" $(MONITOR_FLAGS)
test:
	@mkdir -p build
	@for payload in 32 64 128 240 247; do \
	  $(HOST_CC) -std=c11 -Wall -Wextra -Werror -Icommon -DAPP_PAYLOAD_SIZE=$$payload common/protocol.c tests/test_protocol.c -o build/test_protocol && ./build/test_protocol || exit 1; \
	done
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -Icommon tests/test_measure.c -o build/test_measure
	./build/test_measure
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -Itests/fakes -Icommon common/clock.c tests/test_clock.c -o build/test_clock
	./build/test_clock
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -Icommon common/image_protocol.c tests/test_image_protocol.c -o build/test_image_protocol
	./build/test_image_protocol
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -Icommon -DIMAGE_MODE=1 -DAPP_PAYLOAD_SIZE=240 -DTX_PACKET_COUNT=434 -DTX_INTERVAL_US=1500 common/image_protocol.c common/image_usb.c tests/test_image_usb.c -o build/test_image_usb
	./build/test_image_usb build/test_image_stream.bin
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -Itests/fakes -Icommon -DIMAGE_MODE=1 -DAPP_PAYLOAD_SIZE=240 -DTX_PACKET_COUNT=434 -DTX_INTERVAL_US=1500 common/image_protocol.c common/image_radio.c tests/test_image_radio.c -o build/test_image_radio
	./build/test_image_radio
	$(PYTHON) -m unittest discover -s tests -v
clean:
	rm -rf build

.SECONDARY:
-include $(TX_OBJS:.o=.d) $(RX_OBJS:.o=.d)
