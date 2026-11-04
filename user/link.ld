ENTRY(_start)

SECTIONS
{
    /* The loader maps this window user-accessible and nothing else, so a program
       linked anywhere outside it cannot run: it would have to live in a region
       the kernel keeps supervisor-only. */
    . = 0x0000008000000000;

    .text : {
        *(.text*)
    }

    /* Each output section starts on its own 4 KiB page. The loader maps pages,
       not sections, so two segments sharing one page would need two different
       protection settings for the same frame. */
    . = ALIGN(4096);
    .rodata : {
        *(.rodata*)
    }

    . = ALIGN(4096);
    .data : {
        *(.data*)
    }

    . = ALIGN(4096);
    .bss : {
        *(.bss*)
        *(COMMON)
    }

    /DISCARD/ : {
        *(.comment)
        *(.eh_frame)
        *(.note*)
    }
}
