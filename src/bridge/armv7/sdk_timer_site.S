.syntax unified
.arch armv7-a
.fpu neon-vfpv3
.arm
.text
.balign 4

/* One copied island per original ARM `mrrc p15,0,Rt,Rt2,c14` instruction.
 * The loader replaces the service, destination register and continuation
 * literals.  The original MRRC has no memory side effect: the only guest-visible
 * result is the Rt:Rt2 pair, which the host service writes in the saved frame.
 * Everything else matches the reviewed VP_HOST_BRIDGE_SITE frame layout. */
.global vp_sdk_timer_site_template
.type vp_sdk_timer_site_template,%function
vp_sdk_timer_site_template:
    sub sp, sp, #8
    push {r0-r12, lr}
    mrs r0, APSR
    vmrs r1, FPSCR
    push {r0, r1}
    vpush {d16-d31}
    vpush {d0-d15}
    sub sp, sp, #8
    ldr r0, vp_sdk_timer_site_service_value
    str r0, [sp]
    ldr r0, vp_sdk_timer_site_rt_value
    str r0, [sp, #4]
    add r0, sp, #336
    str r0, [sp, #328]
    ldr r0, vp_sdk_timer_site_continuation_value
    str r0, [sp, #332]
.global vp_sdk_timer_site_branch
vp_sdk_timer_site_branch:
    b vp_bridge_saved_entry

.balign 4
.global vp_sdk_timer_site_service_value
vp_sdk_timer_site_service_value:
    .word 0x0000037f
.global vp_sdk_timer_site_rt_value
vp_sdk_timer_site_rt_value:
    .word 0x00000000
.global vp_sdk_timer_site_continuation_value
vp_sdk_timer_site_continuation_value:
    .word 0x00000000
.global vp_sdk_timer_site_end
vp_sdk_timer_site_end:
.size vp_sdk_timer_site_template,.-vp_sdk_timer_site_template
.section .note.GNU-stack,"",%progbits
