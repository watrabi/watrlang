# boot.s: Multiboot 1 header, switch from 32-bit protected mode to 64-bit
# long mode, and interrupt service routine stubs for watrlang kernel.

.section .multiboot, "a"
.align 4
    .long 0x1BADB002                # magic
    .long 0x00000003                # flags: align modules + mem info
    .long -(0x1BADB002 + 0x00000003)# checksum (0xE4524FFE)

.section .text
.globl _start
.code32
_start:
    cli

    # Set up 32-bit stack
    movl $stack_top, %esp

    # Clear page tables
    movl $pml4, %edi
    xorl %eax, %eax
    movl $(4096 * 3 / 4), %ecx
    rep stosl

    # PML4[0] -> PDPT
    movl $pdpt, %eax
    orl $0x03, %eax                 # Present | Writable
    movl %eax, (pml4)

    # PDPT[0] -> PD
    movl $pd, %eax
    orl $0x03, %eax                 # Present | Writable
    movl %eax, (pdpt)

    # Identity map first 1GB with 512 2MB huge pages (flags: 0x83 = Present | Writable | Huge Page)
    movl $pd, %edi
    movl $0x83, %ebx
    movl $512, %ecx
1:
    movl %ebx, (%edi)
    movl $0, 4(%edi)
    addl $0x200000, %ebx
    addl $8, %edi
    loop 1b

    # Load CR3 with PML4 address
    movl $pml4, %eax
    movl %eax, %cr3

    # Enable PAE in CR4 (bit 5)
    movl %cr4, %eax
    orl $(1 << 5), %eax
    movl %eax, %cr4

    # Enable Long Mode in EFER MSR (0xC0000080, bit 8: LME)
    movl $0xC0000080, %ecx
    rdmsr
    orl $(1 << 8), %eax
    wrmsr

    # Enable Paging (bit 31) and Protected Mode (bit 0) in CR0
    movl %cr0, %eax
    orl $((1 << 31) | (1 << 0)), %eax
    movl %eax, %cr0

    # Load 64-bit GDT
    lgdt (gdt64_ptr)

    # Far jump to 64-bit mode (code selector 0x08)
    ljmp $0x08, $long_mode_start

.code64
long_mode_start:
    # Set segment registers to 64-bit data selector (0x10)
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    movw $0x00, %ax
    movw %ax, %fs
    movw %ax, %gs

    # Set up 64-bit stack
    movq $stack_top, %rsp
    xorq %rbp, %rbp

    # Enable SSE (required for floating point in watrlang):
    # Clear CR0.EM (bit 2), set CR0.MP (bit 1)
    movq %cr0, %rax
    andb $0xFB, %al
    orb $0x02, %al
    movq %rax, %cr0

    # Set CR4.OSFXSR (bit 9) and CR4.OSXMMEXCPT (bit 10)
    movq %cr4, %rax
    orl $((1 << 9) | (1 << 10)), %eax
    movq %rax, %cr4

    # Call kernel main
    call kmain

    # Halt if kmain returns
halt_loop:
    cli
    hlt
    jmp halt_loop

# ---- Interrupt stubs --------------------------------------------------------

.globl isr_breakpoint
.type isr_breakpoint, @function
isr_breakpoint:
    cld
    pushq %rax
    pushq %rcx
    pushq %rdx
    pushq %rbx
    pushq %rbp
    pushq %rsi
    pushq %rdi
    pushq %r8
    pushq %r9
    pushq %r10
    pushq %r11
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15

    call on_breakpoint

    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %r11
    popq %r10
    popq %r9
    popq %r8
    popq %rdi
    popq %rsi
    popq %rbp
    popq %rbx
    popq %rdx
    popq %rcx
    popq %rax
    iretq

.globl isr_timer
.type isr_timer, @function
isr_timer:
    cld
    pushq %rax
    pushq %rcx
    pushq %rdx
    pushq %rbx
    pushq %rbp
    pushq %rsi
    pushq %rdi
    pushq %r8
    pushq %r9
    pushq %r10
    pushq %r11
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15

    call on_timer

    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %r11
    popq %r10
    popq %r9
    popq %r8
    popq %rdi
    popq %rsi
    popq %rbp
    popq %rbx
    popq %rdx
    popq %rcx
    popq %rax
    iretq

# ---- GDT --------------------------------------------------------------------

.section .rodata
.align 8
gdt64:
    .quad 0                         # Null descriptor
gdt64_code:
    .quad 0x00209A0000000000        # 64-bit code: ring 0, executable, readable, long mode (0x08)
gdt64_data:
    .quad 0x0000920000000000        # 64-bit data: ring 0, writable (0x10)
gdt64_end:

gdt64_ptr:
    .word gdt64_end - gdt64 - 1
    .long gdt64

# ---- BSS: Page tables and stack ---------------------------------------------

.section .bss
.align 4096
pml4:
    .skip 4096
pdpt:
    .skip 4096
pd:
    .skip 4096

.align 16
stack_bottom:
    .skip 16384
stack_top:
