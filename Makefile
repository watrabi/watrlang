CC      ?= cc
CFLAGS  ?= -O2 -g
# Always applied, even when CFLAGS is set on the command line.
WATR_CFLAGS := -std=c99 -Wall -Wextra -Wpedantic -Wshadow -D_POSIX_C_SOURCE=200809L -Ibuild
LDFLAGS ?=

# make SAN=1 builds with AddressSanitizer + UBSan
ifeq ($(SAN),1)
WATR_CFLAGS += -fsanitize=address,undefined -fno-omit-frame-pointer
LDFLAGS += -fsanitize=address,undefined
endif

BUILD := build
SRCS  := $(wildcard src/*.c)
OBJS  := $(SRCS:src/%.c=$(BUILD)/%.o)
DEPS  := $(OBJS:.o=.d)

.PHONY: all test clean run-kernel

all: $(BUILD)/watrc

$(BUILD)/watrc: $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $^

$(BUILD)/%.o: src/%.c | $(BUILD)
	$(CC) $(WATR_CFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

# The runtime prelude (written in watrlang) is embedded as a byte array.
$(BUILD)/prelude.o: $(BUILD)/prelude.inc
$(BUILD)/prelude.inc: runtime/prelude.watr | $(BUILD)
	od -An -v -tx1 $< | sed -e 's/\([0-9a-f][0-9a-f]\)/0x\1,/g' > $@

$(BUILD):
	mkdir -p $@

test: $(BUILD)/watrc
	./tests/run_tests.sh
	WATR_GC_STRESS=1 ./tests/run_tests.sh
	python3 tests/expr_fuzz.py
	WATR_GC_STRESS=1 python3 tests/expr_fuzz.py
	python3 tests/prog_fuzz.py
	python3 tests/abi_fuzz.py --programs 6
	python3 tests/array_fuzz.py
	WATR_GC_STRESS=1 python3 tests/array_fuzz.py --programs 4
	./tests/kernel_test.sh

# Boots examples/kernel in a QEMU window (Ctrl-A X quits with -nographic).
run-kernel: $(BUILD)/watrc
	mkdir -p $(BUILD)/kernel
	$(BUILD)/watrc --freestanding -T examples/kernel/linker.ld -o $(BUILD)/kernel/kernel.elf \
		examples/kernel/kernel.watr examples/kernel/boot.s
	objcopy -O elf32-i386 $(BUILD)/kernel/kernel.elf $(BUILD)/kernel/kernel32.elf
	qemu-system-x86_64 -kernel $(BUILD)/kernel/kernel32.elf -serial stdio -m 64

clean:
	rm -rf $(BUILD)

-include $(DEPS)
