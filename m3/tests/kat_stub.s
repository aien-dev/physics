// kat_stub.s -- trivial -bios stub for the SHA-256 KAT: runs from the virt
// flash at 0x0 and branches to the KAT image loaded raw at 0x40200000 (above the DTB QEMU places at the RAM base).
        .section .text, "ax", %progbits
        .global _start
_start:
        movz    x0, #0x4020, lsl #16
        br      x0
