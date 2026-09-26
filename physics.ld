/*
 * Linker script for Physics Authority Nucleus (Milestone 2: PHYSICS_BOOT)
 * Target Platform: QEMU virt AArch64 / Sovereign Machine
 * Normative Base: 0x40200000
 */

ENTRY(_start)

SECTIONS
{
    . = 0x40200000;

    .text : {
        *(.text.entry)
        *(.text)
        . = ALIGN(8);
    }

    .rodata : ALIGN(8) {
        *(.rodata .rodata.*)
        . = ALIGN(8);
    }

    . = 0x40201000;
    .vectors : AT(0x40201000) {
        *(.text.vectors)
    }

    /DISCARD/ : {
        *(.comment)
        *(.note.*)
        *(.eh_frame)
    }
}
