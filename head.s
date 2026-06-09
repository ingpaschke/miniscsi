| head.s — boot/DRVR shim for the miniscsi C driver (Retro68).
|
| Lays out the bytes C can't: the +0 boot preamble and the DRVR header at +0x1E.
| Captures D5 (SCSI id), marshals A0/A1 into C stack args, routes returns (jIODone
| vs RTS), and supplies the lone libc symbol GCC needs (memset) so the blob links
| with no library at all.

        .section .boothdr,"ax",@progbits
        .global _start
        .global drvr            | so body.c's `extern char drvr` resolves
_start:
        bra.w   install         | +0x00  ROM JSRs here at boot, D5.W = SCSI target id
        .long   1               | +0x04  version
        bra.w   install         | +0x08  4.3 entry (Q800/LC475 use +0)
        .org    0x1E
drvr:
        .word   0x6F00          | drvrFlags: dRead|dWrit|dCtl|dStat|dNeedTime|dNeedLock
        .word   0               | drvrDelay
        .word   0               | drvrEMask
        .word   0               | drvrMenu
        .word   d_open  - drvr  | five 16-bit self-relative routine offsets
        .word   d_prime - drvr
        .word   d_ctl   - drvr
        .word   d_status- drvr
        .word   d_close - drvr
        .byte   5
        .ascii  ".NHsd"
        .even

        .text
| install: ROM JSRs here once at boot, D5.W = SCSI target id.
install:
        move.w  %d5,-(%sp)      | save target id before any C call clobbers d5
        move.w  (%sp)+,%d0
        and.w   #7,%d0
        move.w  %d0,-(%sp)
        bsr     cInstall        | PC-relative; returns status in D0
        addq.l  #2,%sp
        rts

| Prime / Status: queued requests complete via jIODone (D0=result, A1=DCE).
d_prime:
        movem.l %a2,-(%sp)
        movea.l %a1,%a2         | a2 = DCE (preserved across the C call)
        move.l  %a1,-(%sp)
        move.l  %a0,-(%sp)
        bsr     cPrime          | D0 = ioResult
        addq.l  #8,%sp
        bra.s   iodone
d_status:
        movem.l %a2,-(%sp)
        movea.l %a1,%a2
        move.l  %a1,-(%sp)
        move.l  %a0,-(%sp)
        bsr     cStatus
        addq.l  #8,%sp
iodone:                         | tail-jump jIODone with D0=result, A1=DCE; restore a2 first
        movea.l %a2,%a1
        movem.l (%sp)+,%a2
        move.l  0x08FC,%a0      | jIODone
        jmp     (%a0)

| Control: accRun (csCode 65) returns via RTS; every other csCode via jIODone.
d_ctl:
        move.w  26(%a0),%d1     | csCode
        cmp.w   #65,%d1         | accRun?
        bne.s   ctl_queued
        move.l  %a1,-(%sp)
        move.l  %a0,-(%sp)
        bsr     cControl        | one-shot self-mount
        addq.l  #8,%sp
        rts                     | accRun is a SystemTask call -> RTS, not IODone
ctl_queued:
        movem.l %a2,-(%sp)
        movea.l %a1,%a2
        move.l  %a1,-(%sp)
        move.l  %a0,-(%sp)
        bsr     cControl
        addq.l  #8,%sp
        bra.s   iodone

d_open:
d_close:
        moveq   #0,%d0          | noErr
        rts

| memset: the only libc symbol GCC synthesizes (struct-zeroing loops in body.c).
| ABI: 4(sp)=dest, 8(sp)=int c, 12(sp)=size_t n; returns dest in d0.  In asm so the
| compiler can't turn a C memset's own loop into a recursive call to memset.
        .global memset
memset:
        movea.l 4(%sp),%a0      | dest
        move.b  11(%sp),%d1     | fill byte = low byte of the int arg (big-endian)
        move.l  12(%sp),%d0     | n (32-bit -> can't use dbra)
        movea.l %a0,%a1         | keep dest for the return value
ms_loop:
        tst.l   %d0
        beq.s   ms_done
        move.b  %d1,(%a0)+
        subq.l  #1,%d0
        bra.s   ms_loop
ms_done:
        move.l  %a1,%d0         | return dest
        rts
