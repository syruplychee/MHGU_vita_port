.syntax unified
.arch armv7-a
.fpu neon-vfpv3
.arm
.text
.balign 4
#include "guest_tls.inc.S"

.global vp_bridge_enter
.type vp_bridge_enter,%function
vp_bridge_enter:
    push {r4-r11,lr}
    sub sp, sp, #4
    vpush {d8-d15}
    sub sp, sp, #24
    stm sp, {r0-r3}
    mov r4, sp
    str r4, [r0, #8]
    vmrs r4, FPSCR
    str r4, [r0, #12]
    ldr r4, [r0, #32]
    vmsr FPSCR, r4
    ldr ip, [sp, #8]
    ldr r3, [sp, #12]
    ldr sp, [sp, #4]
    ldm r3, {r0-r3}
    blx ip
.global vp_bridge_return
vp_bridge_return:
    push {r0,r1}
    VP_GUEST_CONTEXT r4,r5,8
    vmrs r1, FPSCR
    str r1, [r4, #32]
    ldr r0, [sp]
    ldr sp, [r4, #8]
    ldr r1, [r4, #12]
    vmsr FPSCR, r1
    add sp, sp, #24
    vpop {d8-d15}
    add sp, sp, #4
    pop {r4-r11,pc}
.size vp_bridge_enter,.-vp_bridge_enter

.global vp_bridge_saved_entry
.type vp_bridge_saved_entry,%function
vp_bridge_saved_entry:
    VP_GUEST_CONTEXT r4,r5,336
    ldr r0, [r4, #20]
    cmp r0, #1
    bne vp_bridge_failed
    mov r0, #2
    str r0, [r4, #20]
    mov r1, sp
    str r1, [r4, #16]
    ldr sp, [r4, #8]
    ldr r0, [r4, #12]
    vmsr FPSCR, r0
    mov r0, r4
    ldr ip, [r4, #24]
    blx ip
    /* Even a private stress handler can clobber all GPRs/FP; only SP is trusted. */
    ldr r4, [sp]
    str r0, [r4, #36]
    cmp r0, #0
    bne vp_bridge_abort
    ldr r5, [r4, #16]
    mov r0, #1
    str r0, [r4, #20]
    mov r0, #0
    str r0, [r4, #16]
    mov sp, r5
    add sp, sp, #8
    vpop {d0-d15}
    vpop {d16-d31}
    pop {r0,r1}
    vmsr FPSCR, r1
    msr APSR_nzcvqg, r0
    pop {r0-r12,lr}
    add sp, sp, #4
    ldr pc, [sp], #4
.size vp_bridge_saved_entry,.-vp_bridge_saved_entry

/* A rejected host service cannot resume the guest. Unwind this invocation to
 * its saved host frame, preserving the host ABI and returning the real error.
 * This is controlled bridge failure, not a C++ or hardware fault unwinder. */
.type vp_bridge_abort,%function
vp_bridge_abort:
    ldr r1, [r4, #16]
    ldr r1, [r1, #268]
    str r1, [r4, #32]
    mov r1, #0
    str r1, [r4, #16]
    ldr sp, [r4, #8]
    ldr r1, [r4, #12]
    vmsr FPSCR, r1
    add sp, sp, #24
    vpop {d8-d15}
    add sp, sp, #4
    pop {r4-r11,pc}
.size vp_bridge_abort,.-vp_bridge_abort

.global vp_bridge_failed
.type vp_bridge_failed,%function
vp_bridge_failed:
    udf #0x72
    b vp_bridge_failed
.size vp_bridge_failed,.-vp_bridge_failed
.section .note.GNU-stack,"",%progbits
