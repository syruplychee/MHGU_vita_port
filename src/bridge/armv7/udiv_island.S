.syntax unified
.arch armv7-a
.arm
.text
.balign 4
#include "udiv_r3_r1_r2.inc.S"
.global vp_udiv_template_begin,vp_udiv_template_end
vp_udiv_template_begin:
    VP_UDIV_R3_R1_R2 vp_udiv_template_end
vp_udiv_template_end:
/* The final word (offset 100) is rebound to the original site's +4. */
.section .note.GNU-stack,"",%progbits
