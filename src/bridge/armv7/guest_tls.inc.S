/* Fixed A32 leaf templates. Stack regions must be registered and quiescently
 * sealed before execution, and have 12 bytes of writable headroom.
 * No calls, FP operations, hardware TLS access or software address translation.
 * The failure target is a macro argument so the branch-entered island form
 * (guest_tls_island.S) can keep its trap inside the island instead of branching
 * back into host code.
 */
.macro VP_GUEST_CONTEXT ctx, index, delta, fail=vp_guest_context_missing
    add \index, sp, #\delta
    lsr \ctx, \index, #24
    movw \index, #:lower16:vp_guest_stack_directory
    movt \index, #:upper16:vp_guest_stack_directory
    ldr \ctx, [\index, \ctx, lsl #2]
    cmp \ctx, #0
    beq \fail
    add \index, sp, #\delta
    ubfx \index, \index, #16, #8
    ldr \ctx, [\ctx, \index, lsl #2]
    cmp \ctx, #0
    beq \fail
.endm

/* Equivalent destination/effects to MRC p15,0,r1,c13,c0,3. */
.macro VP_TLS_READ_RO_R1 fail=vp_guest_context_missing
    push {r0, r2, r3}
    mrs r2, APSR
    VP_GUEST_CONTEXT r0, r3, 12, \fail
    ldr r1, [r0]
    msr APSR_nzcvqg, r2
    pop {r0, r2, r3}
.endm

/* Equivalent source/effects to MCR p15,0,r4,c13,c0,2. */
.macro VP_TLS_WRITE_RW_R4 fail=vp_guest_context_missing
    push {r0, r2, r3}
    mrs r2, APSR
    VP_GUEST_CONTEXT r0, r3, 12, \fail
    str r4, [r0, #4]
    msr APSR_nzcvqg, r2
    pop {r0, r2, r3}
.endm

.macro VP_TLS_READ_RW_R1 fail=vp_guest_context_missing
    push {r0, r2, r3}
    mrs r2, APSR
    VP_GUEST_CONTEXT r0, r3, 12, \fail
    ldr r1, [r0, #4]
    msr APSR_nzcvqg, r2
    pop {r0, r2, r3}
.endm

/* Destination-register variants. Scratch registers exclude the destination, so
 * the leaf keeps every other GPR and the pushed set is restored unchanged.
 */
.macro VP_TLS_READ_RO_R0 fail=vp_guest_context_missing
    push {r1, r2, r3}
    mrs r2, APSR
    VP_GUEST_CONTEXT r1, r3, 12, \fail
    ldr r0, [r1]
    msr APSR_nzcvqg, r2
    pop {r1, r2, r3}
.endm

.macro VP_TLS_READ_RO_R2 fail=vp_guest_context_missing
    push {r0, r1, r3}
    mrs r1, APSR
    VP_GUEST_CONTEXT r0, r3, 12, \fail
    ldr r2, [r0]
    msr APSR_nzcvqg, r1
    pop {r0, r1, r3}
.endm

.macro VP_TLS_WRITE_RW_R0 fail=vp_guest_context_missing
    push {r1, r2, r3}
    mrs r2, APSR
    VP_GUEST_CONTEXT r1, r3, 12, \fail
    str r0, [r1, #4]
    msr APSR_nzcvqg, r2
    pop {r1, r2, r3}
.endm
