ENTRY(loader)
OUTPUT_FORMAT(elf64-x86-64)

SECTIONS
{
    /* The kernel will be loaded at 1MB by the bootloader */
    . = 1M;
    
    _kernel_start = .;

    /* Multiboot headers MUST be at the very beginning of the file */
    .boot : {
        KEEP(*(.multiboot2_header))
        KEEP(*(.multiboot_header))
    }

    .text : ALIGN(4K) {
        *(.text)
    }

    .rodata : ALIGN(4K) {
        *(.rodata)
    }

    .ksymtab : ALIGN(8) {
        __start_ksymtab = .;
        KEEP(*(.ksymtab))
        __stop_ksymtab = .;
    }

    .data : ALIGN(4K) {
        *(.data)
    }

    .bss : ALIGN(4K) {
        *(COMMON)
        *(.bss)
    }

    . = ALIGN(4K);
    _kernel_end = .;
}