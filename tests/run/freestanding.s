# Entry point for tests/run/freestanding.watr (run_tests.sh links it in).
# A freestanding program has no runtime to call it, so this starts it:
# align the stack, call kmain, and exit with its result.
    .intel_syntax noprefix
    .text
    .globl _start
_start:
    xor ebp, ebp
    and rsp, -16
    call kmain
    mov edi, eax
    mov eax, 231            # exit_group
    syscall
