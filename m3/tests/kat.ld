/* kat.ld -- SHA-256 KAT image, loaded raw at 0x40200000 (QEMU puts the DTB at the RAM base 0x40000000 with -bios). */
OUTPUT_FORMAT("elf64-littleaarch64")
OUTPUT_ARCH(aarch64)
ENTRY(_start)
KAT_BASE  = 0x40200000;
KAT_LIMIT = 0x40270000 - 0x4000;   /* image must stay below the 16 KiB stack */
SECTIONS
{
    . = KAT_BASE;
    .text : {
        KEEP(*(.text.boot))
        *(.text .text.*)
    }
    .rodata : ALIGN(8) { *(.rodata .rodata.*) }
    .data   : ALIGN(8) { *(.data .data.*) }
    .bss    : ALIGN(8) { *(.bss .bss.* COMMON) }
    __kat_end = .;
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
ASSERT(SIZEOF(.data) == 0, "KAT: unexpected .data")
ASSERT(SIZEOF(.bss) == 0, "KAT: unexpected .bss (raw image would not zero it)")
ASSERT(__kat_end <= KAT_LIMIT, "KAT: image overlaps stack")
ASSERT(SHA256_MAX_LEN <= 0x10000, "KAT: SHA256_MAX_LEN exceeds the 64 KiB PAT buffer")
