/* Loads the kernel at 1 MiB, with the multiboot header first. */
ENTRY(_start)

PHDRS
{
    text PT_LOAD FLAGS(5);     /* read + execute */
    rodata PT_LOAD FLAGS(4);   /* read */
    data PT_LOAD FLAGS(6);     /* read + write */
}

SECTIONS
{
    . = 1M;
    .text : { *(.multiboot) *(.text .text.*) } :text
    . = ALIGN(4K);
    .rodata : { *(.rodata .rodata.*) } :rodata
    . = ALIGN(4K);
    .data : { *(.data .data.* .data.rel.ro .data.rel.ro.*) } :data
    .bss : { *(COMMON) *(.bss .bss.*) } :data
    /DISCARD/ : { *(.note.GNU-stack) *(.comment) }
}
