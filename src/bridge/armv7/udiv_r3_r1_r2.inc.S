/* Self-authored A32 island body for UDIV r3,r1,r2 (AL).
 * Entry: direct B, writable guest stack with 16 bytes of scratch space.
 * Exit: direct B to a site-specific continuation; LR is never changed.
 * No C calls, FP, TLS, privileged operations or instruction dispatch.
 * Integer division by zero yields zero, matching the reference A32 path.
 */
.macro VP_UDIV_R3_R1_R2 continuation
    push {r0, r4, r5, r12}
    mrs r12, APSR
    mov r3, #0
    cmp r2, #0
    beq .Lvp_udiv_done\@
    cmp r2, #1
    moveq r3, r1
    beq .Lvp_udiv_done\@
    cmp r1, r2
    blo .Lvp_udiv_done\@
    moveq r3, #1
    beq .Lvp_udiv_done\@
    clz r5, r2
    clz r0, r1
    sub r5, r5, r0
    lsl r4, r2, r5
    mov r0, r1
.Lvp_udiv_loop\@:
    cmp r0, r4
    subcs r0, r0, r4
    adc r3, r3, r3
    lsr r4, r4, #1
    subs r5, r5, #1
    bpl .Lvp_udiv_loop\@
.Lvp_udiv_done\@:
    msr APSR_nzcvq, r12
    pop {r0, r4, r5, r12}
    b \continuation
.endm
