# Builds the Noge programs for a Cortex-M3 and runs them under QEMU. Needs arm-none-eabi-gcc and
# qemu-system-arm; nogec is built from the top of the repository.
NOGEC   ?= ../../build/nogec
ARMCC   ?= arm-none-eabi-gcc
SIZE    ?= arm-none-eabi-size
QEMU    ?= qemu-system-arm
MACHINE ?= stm32vldiscovery

CPU      = -mcpu=cortex-m3 -mthumb
OPT     ?= -Os
COMMON   = $(CPU) $(OPT) -ffreestanding -ffunction-sections -fdata-sections -Wall -Wextra -Werror
GENFLAGS = $(COMMON) -std=c99 -Wpedantic           # the C that nogec writes must be clean ISO C99
LDFLAGS  = $(CPU) -T link.ld -nostartfiles --specs=nano.specs --specs=nosys.specs -Wl,--gc-sections

NOGEFLAGS ?=                                          # for example --no-builtins
PROGRAMS = hello checks trap systick blinky machine
ELFS     = $(PROGRAMS:%=build/%.elf) build/blinky-hw.elf build/machine-sep.elf

all: $(ELFS)

build:
	mkdir -p build

build/%.c: %.nge $(NOGEC) | build
	$(NOGEC) --target=cortex-m $(NOGEFLAGS) -o $@ $<

build/%.o: build/%.c
	$(ARMCC) $(GENFLAGS) -c -o $@ $<

build/startup.o build/board_qemu.o build/board_hw.o: build/%.o: %.c | build
	$(ARMCC) $(COMMON) -c -o $@ $<

build/blinky-hw.elf: build/blinky.o build/startup.o build/board_hw.o link.ld
	$(ARMCC) $(LDFLAGS) -Wl,-Map=build/blinky-hw.map -o $@ build/blinky.o build/startup.o build/board_hw.o

# the same program as machine.elf, but built from one C file per module (nogec --emit-dir) with the Makefile nogec writes
build/machine-sep.elf: machine.nge $(NOGEC) build/startup.o build/board_qemu.o link.ld | build
	mkdir -p build/machine-sep
	$(NOGEC) --target=cortex-m $(NOGEFLAGS) --emit-dir=build/machine-sep machine.nge
	$(MAKE) --no-print-directory -s -C build/machine-sep CC="$(ARMCC)" CFLAGS="$(GENFLAGS)" \
	    EXTRA="$(CURDIR)/build/startup.o $(CURDIR)/build/board_qemu.o" \
	    LDFLAGS="$(CPU) -T $(CURDIR)/link.ld -nostartfiles --specs=nano.specs --specs=nosys.specs -Wl,--gc-sections"
	cp build/machine-sep/machine $@

build/%.elf: build/%.o build/startup.o build/board_qemu.o link.ld
	$(ARMCC) $(LDFLAGS) -Wl,-Map=build/$*.map -o $@ build/$*.o build/startup.o build/board_qemu.o

# the raw image to write at 0x08000000 (for example with st-flash or OpenOCD)
build/%.bin: build/%.elf
	arm-none-eabi-objcopy -O binary $< $@

size: $(ELFS)
	$(SIZE) $(ELFS)

test: $(ELFS)
	@QEMU="$(QEMU)" MACHINE="$(MACHINE)" sh test.sh

clean:
	rm -rf build

.PHONY: all size test clean
.SECONDARY:
