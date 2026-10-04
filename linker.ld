ENTRY(_start)

KOFF = 0xffffffff80000000;

SECTIONS
{
    . = 1M;

    /* 32 bit boot stub: linked at its physical address */
    .boot :
    {
        KEEP(*(.multiboot))
        *(.boot.text)
        *(.boot.data)
    }

    . = ALIGN(4K) + KOFF;

    .text : AT(ADDR(.text) - KOFF)
    {
        *(.text.start)
        *(.text*)
    }

    . = ALIGN(4K);
    .rodata : AT(ADDR(.rodata) - KOFF)
    {
        *(.rodata*)
    }

    . = ALIGN(4K);
    .data : AT(ADDR(.data) - KOFF)
    {
        *(.data*)
    }

    . = ALIGN(4K);
    .bss : AT(ADDR(.bss) - KOFF)
    {
        *(COMMON)
        *(.bss*)
    }

    /* kernel heap starts at the first 4 MiB boundary past .bss */
    . = ALIGN(4M);
    _heap_start = .;

    boot_pml4_phys = boot_pml4 - KOFF;
    boot_pdpt_a_phys = boot_pdpt_a - KOFF;
    boot_pdpt_k_phys = boot_pdpt_k - KOFF;
    boot_pd_phys = boot_pd - KOFF;
    boot_stack_phys = boot_stack - KOFF;

    /DISCARD/ :
    {
        *(.comment)
        *(.note*)
        *(.eh_frame*)
    }
}
