@ ARM7 copy stub for booting a program whose ARM7 binary loads into ARM7 RAM
@ (see common/slboot.h). Boot7Poll() copies it to VRAM and runs it there,
@ out of the way of the RAM it fills. Position independent: no literals.
@   r0 = source (VRAM), r1 = destination, r2 = bytes (multiple of 4), r3 = entry

    .arm
    .section .text.boot7_stub, "ax"
    .global boot7_stub, boot7_stub_end

boot7_stub:
1:  subs    r2, r2, #4
    ldrge   r12, [r0], #4
    strge   r12, [r1], #4
    bgt     1b

    mov     r12, #0x04000000
    add     r12, r12, #0x180        @ REG_IPC_SYNC
    mov     r0, #0xA00              @ out = SLBOOT_SYNC_ARM7_READY
    strh    r0, [r12]
2:  ldrh    r0, [r12]
    and     r0, r0, #0xF
    cmp     r0, #0xB                @ wait for SLBOOT_SYNC_ARM9_COPIED
    bne     2b
    mov     r0, #0
    strh    r0, [r12]               @ out = 0
    bx      r3
boot7_stub_end:
