# A kernel in watrlang

A small x86-64 kernel: it boots through multiboot, switches to 64-bit
mode, and runs `kmain()` from `kernel.watr`, which

- writes to the VGA text screen (with colors and scrolling) and the
  serial port, through `echo`,
- installs an interrupt table and handles `int3` breakpoints,
- remaps the PIC and counts PIT timer interrupts while the CPU sleeps,
- prints `KERNEL PANIC` if a runtime check fails (try indexing an array
  out of bounds).

![the kernel's screen in QEMU](../../docs/img/kernel.png)

## Files

- `boot.s`: the multiboot header, the switch from 32-bit protected mode to
  long mode (page tables, GDT, SSE), and the interrupt entry stubs.
- `kernel.watr`: everything else, in watrlang. It uses `--freestanding`
  features: inline asm with register operands for port I/O, `volatile`
  pointers for the screen, `#[packed]` and `#[align]` structs for the
  interrupt table, and the `watr_write` / `watr_panic` hooks.
- `linker.ld`: loads the kernel at 1 MiB.

## Running it

```sh
make run-kernel            # builds it and opens QEMU
./tests/kernel_test.sh     # boots it headless and checks the serial output
```

By hand:

```sh
watrc --freestanding -T linker.ld -o kernel.elf kernel.watr boot.s
objcopy -O elf32-i386 kernel.elf kernel32.elf   # QEMU's -kernel wants ELF32
qemu-system-x86_64 -kernel kernel32.elf -serial stdio
```

The kernel ends by writing to QEMU's `isa-debug-exit` port (if QEMU was
started with `-device isa-debug-exit,iobase=0xf4,iosize=0x04`), and
otherwise halts.
