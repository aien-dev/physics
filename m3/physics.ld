/*
 * Linker script for Physics Effects Nucleus (Milestone 3: PHYSICS_EFFECTS)
 * Target Platform: QEMU virt AArch64 / Sovereign Machine
 * Normative Base: 0x40200000
 * Normative Vectors: 0x40204000 (2 KiB aligned)
 */

ENTRY(_start)

SECTIONS
{
    . = 0x40200000;

    .text : {
        *(.text.entry)
        *(.text)
        *(.text.sha256_be)
        . = ALIGN(8);
    }

    .rodata : ALIGN(8) {
        *(.rodata .rodata.*)
        . = ALIGN(8);
    }

    . = 0x40204000;
    .vectors : AT(0x40204000) {
        *(.text.vectors)
    }

    /DISCARD/ : {
        *(.comment)
        *(.note.*)
        *(.eh_frame)
    }
}
