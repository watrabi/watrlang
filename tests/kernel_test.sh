#!/bin/sh
# Builds examples/kernel and boots it in QEMU, checking what it prints on
# the serial port and that it exits through QEMU's debug-exit device.
# Skipped (successfully) when qemu-system-x86_64 isn't installed.
set -u
cd "$(dirname "$0")/.."

if ! command -v qemu-system-x86_64 > /dev/null 2>&1; then
    echo "kernel: skipped (qemu-system-x86_64 not installed)"
    exit 0
fi

WATRC=${WATRC:-./build/watrc}
mkdir -p build/kernel
"$WATRC" --freestanding -T examples/kernel/linker.ld -o build/kernel/kernel.elf \
    examples/kernel/kernel.watr examples/kernel/boot.s || exit 1
objcopy -O elf32-i386 build/kernel/kernel.elf build/kernel/kernel32.elf || exit 1   # QEMU loads ELF32 multiboot kernels

out=$(timeout 60 qemu-system-x86_64 -kernel build/kernel/kernel32.elf -serial stdio -display none -no-reboot \
      -m 64 -device isa-debug-exit,iobase=0xf4,iosize=0x04 < /dev/null 2>&1)
status=$?
if [ "$status" -ne 33 ]; then   # kmain exits with code 0x10: (0x10 << 1) | 1
    printf 'kernel: FAIL (qemu exited with %s)\n%s\n' "$status" "$out"
    exit 1
fi
if ! printf '%s\n' "$out" | diff -u examples/kernel/expected.txt - > build/kernel/diff.txt; then
    echo "kernel: FAIL (serial output differs)"
    cat build/kernel/diff.txt
    exit 1
fi
echo "kernel: booted in QEMU, output matches"
