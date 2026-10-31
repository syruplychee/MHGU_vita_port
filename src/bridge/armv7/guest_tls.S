.syntax unified
.arch armv7-a
.arm
.text
.balign 4
.global vp_guest_context_missing
.type vp_guest_context_missing,%function
vp_guest_context_missing:
    /* Fail closed at a named trap; never invent a TLS pointer. */
    udf #0x71
    b vp_guest_context_missing
.size vp_guest_context_missing,.-vp_guest_context_missing
.section .note.GNU-stack,"",%progbits
