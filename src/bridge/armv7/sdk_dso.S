.syntax unified
.arch armv7-a
.fpu neon-vfpv3
.arm
.text
#include "host_bridge.inc.S"
#include "guest_tls.inc.S"

/* AAPCS function entries need only integer argument preservation for the
 * integer-only fast helper. Misses restore the original call before entering
 * the complete bridge. Fatal kernel errors get their own non-retrying bridge. */
.macro VP_SDK_LOCK_FUNCTION name, service, boolean=0
    .balign 4
    .global \name
    .type \name,%function
\name:
    push {r0-r3,ip,lr}
    VP_GUEST_CONTEXT r1,r2,24
    ldr r1, [r1, #28] /* VPHostBridge.user == VPExecutor * */
    movw r2, #\service
    bl vp_sdk_lock_fast
    cmp r0, #0
    blt 2f
    beq 1f
    .if \boolean
        cmp r0, #1
        moveq r0, #1
        movne r0, #0
        str r0, [sp]
    .endif
    pop {r0-r3,ip,lr}
    bx lr
1:
    pop {r0-r3,ip,lr}
    b \name\()_slow
2:
    str r0, [sp]
    pop {r0-r3,ip,lr}
    b vp_sdk_lock_fast_error
    .size \name,.-\name
    VP_HOST_BRIDGE_FUNCTION \name\()_slow, \service
.endm

VP_HOST_BRIDGE_FUNCTION vp_sdk_alloc, 0x100
VP_HOST_BRIDGE_FUNCTION vp_sdk_free, 0x101
VP_HOST_BRIDGE_FUNCTION vp_sdk_ctor_gate, 0x230
VP_HOST_BRIDGE_FUNCTION vp_sdk_dso_enter, 0x231
VP_SDK_LOCK_FUNCTION vp_sdk_lock, 0x232
VP_SDK_LOCK_FUNCTION vp_sdk_unlock, 0x233
VP_HOST_BRIDGE_FUNCTION vp_sdk_exception_info, 0x234
VP_HOST_BRIDGE_FUNCTION vp_sdk_malloc, 0x235
VP_HOST_BRIDGE_FUNCTION vp_sdk_heap_free, 0x236
VP_HOST_BRIDGE_FUNCTION vp_sdk_tls_normal, 0x240
VP_HOST_BRIDGE_FUNCTION vp_sdk_tls_private, 0x241
VP_HOST_BRIDGE_FUNCTION vp_sdk_tls_free_bridge, 0x242
VP_HOST_BRIDGE_FUNCTION vp_sdk_tls_get_bridge, 0x243
VP_HOST_BRIDGE_FUNCTION vp_sdk_tls_set_bridge, 0x244
VP_SDK_LOCK_FUNCTION vp_sdk_try_lock, 0x245, 1
VP_SDK_LOCK_FUNCTION vp_sdk_is_locked, 0x246, 1
VP_HOST_BRIDGE_FUNCTION vp_sdk_lock_fast_error, 0x24b
VP_HOST_BRIDGE_FUNCTION vp_sdk_os_unavailable, 0x249
/* Mid-function site: the handler selects the SDK loop or original epilogue. */
VP_HOST_BRIDGE_FUNCTION vp_sdk_video_readback, 0x24a
/* Marker-gated subsdk0 mid-function probes; handlers replay the displaced
 * integer word and supply the original continuation, rather than guest LR. */
VP_HOST_BRIDGE_FUNCTION vp_movie_color_rgb_enter, 0x24c
VP_HOST_BRIDGE_FUNCTION vp_movie_color_rgb_leave, 0x24d
VP_HOST_BRIDGE_FUNCTION vp_movie_color_nv12_enter, 0x24e
VP_HOST_BRIDGE_FUNCTION vp_movie_color_nv12_leave, 0x24f
/* Locked main resource-submit sites; handlers replay the displaced words. */
VP_HOST_BRIDGE_FUNCTION vp_engine_submit_enter, 0x250
VP_HOST_BRIDGE_FUNCTION vp_engine_submit_leave, 0x251
VP_HOST_BRIDGE_FUNCTION vp_engine_full_failed, 0x252
/* A/B profile boundary around one complete root call. The enter handler keeps
 * r0=context and returns the selected target (native C or original trampoline)
 * in ip; the leave handler accounts the completed call before restoring the
 * caller's callee-saved registers. */
VP_HOST_BRIDGE_FUNCTION vp_engine_full_ab_enter, 0x253
VP_HOST_BRIDGE_FUNCTION vp_engine_full_ab_leave, 0x254

/* Full queue replacement: C owns its local state; original caller ABI stays
 * on the guest stack. Failures enter the normal host bridge and stop. The
 * profile flag is read once at the root entry so the single-path build keeps
 * the original fast path. */
.balign 4
.global vp_engine_full_entry
.type vp_engine_full_entry,%function
vp_engine_full_entry:
    ldr ip, 1f
    ldr ip, [ip]
    cmp ip, #0
    bne vp_engine_full_ab_entry
    ldr ip, 2f
    ldr ip, [ip]
    cmp ip, #0
    bne vp_engine_full_fast_entry
    push {r4,lr}
    bl vp_engine_full_run
    pop {r4,lr}
    cmp r0,#0
    bne vp_engine_full_failed
    bx lr
1:  .word vp_engine_full_profile_enabled
2:  .word vp_engine_full_fast_enabled
.size vp_engine_full_entry,.-vp_engine_full_entry

.balign 4
.type vp_engine_full_fast_entry,%function
vp_engine_full_fast_entry:
    push {r4,lr}
    bl vp_engine_full_run_fast
    pop {r4,lr}
    cmp r0,#0
    bne vp_engine_full_failed
    bx lr
.size vp_engine_full_fast_entry,.-vp_engine_full_fast_entry

/* Alternating A/B wrapper. push {r4,lr} keeps SP 8-byte aligned and preserves
 * the caller's r4 and return address. The bridge enter leaves r0=context and
 * writes the path target to ip; blx ip runs either the native C body or the
 * original-entry trampoline. The leave bridge checks the C return value only
 * in native mode, so an arbitrary original-root r0 is never treated as an
 * error. On success the two bridges restore the complete register/FPSCR/VFP
 * frame and this wrapper returns the untouched original caller ABI. */
.balign 4
.global vp_engine_full_ab_entry
.type vp_engine_full_ab_entry,%function
vp_engine_full_ab_entry:
    push {r4,lr}
    bl vp_engine_full_ab_enter
    blx ip
    bl vp_engine_full_ab_leave
    pop {r4,lr}
    bx lr
.size vp_engine_full_ab_entry,.-vp_engine_full_ab_entry

/* Invoke the exact locked NVN slot with core, stack and hard-float arguments.
 * Call offsets: r[]=8, stack[]=24, s[]=60. Keep 8-byte SP alignment. */
.balign 4
.global vp_engine_nvn_call
.type vp_engine_nvn_call,%function
vp_engine_nvn_call:
    push {r4-r11,ip,lr}
    sub sp,sp,#32
    mov r4,r0
    mov r5,r1
    mov r6,r2
    add ip,r5,#24
    ldm ip,{r0-r3,r8-r11}
    stm sp,{r0-r3,r8-r11}
    vldr s0,[r5,#60]
    vldr s1,[r5,#64]
    vldr s2,[r5,#68]
    vldr s3,[r5,#72]
    add ip,r5,#8
    ldm ip,{r0-r3}
    blx r4
    cmp r6,#0
    strne r0,[r6]
    strne r1,[r6,#4]
    movne r2,#1
    strne r2,[r6,#8]
    add sp,sp,#32
    pop {r4-r11,ip,pc}
.size vp_engine_nvn_call,.-vp_engine_nvn_call

/* Only the two fixed-count layout encoders are replaced. The C helper uses
 * guest-TLS-safe memory operations and original SDK calls on
 * misses. It preserves callee-saved registers and the original guest stack. */
.balign 4
.global vp_engine_layout_entry
.type vp_engine_layout_entry,%function
vp_engine_layout_entry:
    push {r0-r3,ip,lr}
    mov r0,r5
    mov r1,r4
    bl vp_engine_layout_prepare
    cmp r0,#0
    beq 2f
    pop {r0-r3,ip,lr}
    ldr ip,1f
    ldr pc,[ip]
1:  .word vp_engine_layout_resume
2:  pop {r0-r3,ip,lr}
    ldr ip,3f
    ldr pc,[ip]
3:  .word vp_engine_layout_fallback
.size vp_engine_layout_entry,.-vp_engine_layout_entry

/* Pure tail forwards: exception propagation stays entirely on the guest
 * stack, through the original SDK runtime. No host C++ runtime is linked. */
.macro SDK_CPP_FORWARD name, offset
    .balign 4
    .global \name
    .type \name,%function
\name:
    ldr ip, 1f
    ldr pc, [ip, #\offset]
1:  .word vp_sdk_cpp_functions
    .size \name,.-\name
.endm
SDK_CPP_FORWARD __cxa_allocate_exception, 0
SDK_CPP_FORWARD __cxa_begin_catch, 4
SDK_CPP_FORWARD __cxa_end_catch, 8
SDK_CPP_FORWARD __cxa_end_cleanup, 12
SDK_CPP_FORWARD __cxa_throw, 16
SDK_CPP_FORWARD __gxx_personality_v0, 20
SDK_CPP_FORWARD __aeabi_unwind_cpp_pr0, 24
.global vp_sdk_argument_fixture
.type vp_sdk_argument_fixture,%function
vp_sdk_argument_fixture:
    mov ip, #2
    mla r0, r1, ip, r0
    mov ip, #3
    mla r0, r2, ip, r0
    add r0, r0, r3, lsl #2
    .irp i,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19
        ldr r1, [sp, #(\i*4)]
        mov ip, #(\i+5)
        mla r0, r1, ip, r0
    .endr
    bx lr
.global vp_sdk_argument_fixture_end
vp_sdk_argument_fixture_end:
.size vp_sdk_argument_fixture,.-vp_sdk_argument_fixture
/* Checked libunwind query, unlike _Unwind_Find_FDE which discards the error
 * return from unw_get_proc_info. No exception crosses this adapter. */
.global vp_sdk_unwind_query
.type vp_sdk_unwind_query,%function
vp_sdk_unwind_query:
    push {r4,r5,r6,r7,r8,lr}
    sub sp, sp, #1024
    mov r4, r0
    mov r5, r1
    mov r6, r2
    mov r0, sp
    ldr ip, [r6]
    blx ip
    cmp r0, #0
    bne 2f
    add r0, sp, #512
    mov r1, sp
    ldr ip, [r6, #4]
    blx ip
    cmp r0, #0
    bne 2f
    add r0, sp, #512
    mvn r1, #0
    mov r2, r4
    ldr ip, [r6, #8]
    blx ip
    cmp r0, #0
    bne 2f
    add r0, sp, #512
    mov r1, r5
    ldr ip, [r6, #12]
    blx ip
2:  add sp, sp, #1024
    pop {r4,r5,r6,r7,r8,pc}
.global vp_sdk_unwind_query_end
vp_sdk_unwind_query_end:
.size vp_sdk_unwind_query,.-vp_sdk_unwind_query
.bss
.balign 4
.global vp_sdk_cpp_functions
vp_sdk_cpp_functions: .space 28
.global _ZTIi
_ZTIi: .space 8
.section .note.GNU-stack,"",%progbits
