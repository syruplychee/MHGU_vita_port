/* Fixed A32 site: preserve guest LR; continuation is a link-time symbol.
 * Guest provides no stack descriptor. Frame is 336 bytes, 8-byte aligned.
 */
.macro VP_HOST_BRIDGE_SITE name, service, continuation
    .balign 4
    .global \name
    .type \name,%function
\name:
    sub sp, sp, #8
    push {r0-r12, lr}
    mrs r0, APSR
    vmrs r1, FPSCR
    push {r0,r1}
    vpush {d16-d31}
    vpush {d0-d15}
    sub sp, sp, #8
    movw r0, #\service
    str r0, [sp]
    mov r0, #0
    str r0, [sp, #4]
    add r0, sp, #336
    str r0, [sp, #328]
    movw r0, #:lower16:\continuation
    movt r0, #:upper16:\continuation
    str r0, [sp, #332]
    b vp_bridge_saved_entry
    .size \name,.-\name
.endm

/* AAPCS function boundary: return to the caller's saved LR instead of a
 * link-time continuation. The bridge still preserves the complete guest
 * frame; the handler changes only declared return registers. */
.macro VP_HOST_BRIDGE_FUNCTION name, service
    .balign 4
    .global \name
    .type \name,%function
\name:
    sub sp, sp, #8
    push {r0-r12, lr}
    mrs r0, APSR
    vmrs r1, FPSCR
    push {r0,r1}
    vpush {d16-d31}
    vpush {d0-d15}
    sub sp, sp, #8
    movw r0, #\service
    str r0, [sp]
    mov r0, #0
    str r0, [sp, #4]
    add r0, sp, #336
    str r0, [sp, #328]
    ldr r0, [sp, #324]
    str r0, [sp, #332]
    b vp_bridge_saved_entry
    .size \name,.-\name
.endm
