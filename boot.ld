/* boot.ld — flat link for the miniscsi driver.
 *
 * The ROM loads this blob from an Apple_Driver43 partition and JSRs offset 0, so
 * head.s's .boothdr (boot preamble + DRVR header) must sit at the very front.
 * Owning the link keeps the blob to exactly [.boothdr][.text/.rodata][.data] —
 * no entry trampoline, no relocation table, no runtime.  The driver is fully
 * position-independent (-mpcrel, 0 relocs), so VMA 0 is only a link-time anchor;
 * the ROM may load the image anywhere and it runs as-is.
 */
ENTRY(_start)

SECTIONS
{
    /* Drop GCC's per-function MacsBug name strings (.text.<fn>.macsbug).  Must
     * precede the .text glob below, which would otherwise claim them. */
    /DISCARD/ : {
        *(.text.*.macsbug)
        *(.eh_frame*) *(.comment) *(.note*) *(.gnu.attributes) *(.gnu.warning)
    }

    . = 0;

    .text : {
        KEEP(*(.boothdr))               /* +0 preamble + DRVR header @ +0x1E */
        *(.text .text.*)                /* install, dispatch thunks, memset, the C body */
        *(.rodata .rodata.* .rodata1)   /* string literals (PC-relative, in-image) */
    }

    .data : { *(.data .data.*) }        /* gDriveIcon + the zero-init resident globals */

    /* No startup pass allocates .bss, so it must be empty (-fno-zero-initialized-in-bss). */
    .bss (NOLOAD) : { *(.bss .bss.* COMMON) }
    ASSERT(SIZEOF(.bss) == 0, "BSS must be empty")
}
