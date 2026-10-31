.syntax unified
.arch armv7-a
.fpu neon-vfpv3
.arm
.text
.balign 4

/* One copied island per original ARM SVC instruction.  The two literal words
 * are replaced by the loader: service (VP_GUEST_SVC_BASE + SVC id) and the
 * guest continuation (site + 4).  Everything else matches the reviewed
 * VP_HOST_BRIDGE_SITE frame layout. */
.global vp_sdk_svc_site_template
.type vp_sdk_svc_site_template,%function
vp_sdk_svc_site_template:
    sub sp, sp, #8
    push {r0-r12, lr}
    mrs r0, APSR
    vmrs r1, FPSCR
    push {r0, r1}
    vpush {d16-d31}
    vpush {d0-d15}
    sub sp, sp, #8
    ldr r0, vp_sdk_svc_site_service_value
    str r0, [sp]
    mov r0, #0
    str r0, [sp, #4]
    add r0, sp, #336
    str r0, [sp, #328]
    ldr r0, vp_sdk_svc_site_continuation_value
    str r0, [sp, #332]
    /* Link-time relative branch, rewritten by the loader because this
     * template is copied into the guest module arena. */
.global vp_sdk_svc_site_branch
vp_sdk_svc_site_branch:
    b vp_bridge_saved_entry

.balign 4
.global vp_sdk_svc_site_service_value
vp_sdk_svc_site_service_value:
    .word 0x00000300
.global vp_sdk_svc_site_continuation_value
vp_sdk_svc_site_continuation_value:
    .word 0x00000000
.global vp_sdk_svc_site_end
vp_sdk_svc_site_end:
.size vp_sdk_svc_site_template,.-vp_sdk_svc_site_template
.section .note.GNU-stack,"",%progbits
