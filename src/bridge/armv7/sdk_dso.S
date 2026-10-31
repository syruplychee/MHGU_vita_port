.syntax unified
.arch armv7-a
.fpu neon-vfpv3
.arm
.text
#include "host_bridge.inc.S"
VP_HOST_BRIDGE_FUNCTION vp_sdk_alloc, 0x100
VP_HOST_BRIDGE_FUNCTION vp_sdk_free, 0x101
VP_HOST_BRIDGE_FUNCTION vp_sdk_ctor_gate, 0x230
VP_HOST_BRIDGE_FUNCTION vp_sdk_dso_enter, 0x231
VP_HOST_BRIDGE_FUNCTION vp_sdk_lock, 0x232
VP_HOST_BRIDGE_FUNCTION vp_sdk_unlock, 0x233
VP_HOST_BRIDGE_FUNCTION vp_sdk_exception_info, 0x234
VP_HOST_BRIDGE_FUNCTION vp_sdk_malloc, 0x235
VP_HOST_BRIDGE_FUNCTION vp_sdk_heap_free, 0x236
VP_HOST_BRIDGE_FUNCTION vp_sdk_tls_normal, 0x240
VP_HOST_BRIDGE_FUNCTION vp_sdk_tls_private, 0x241
VP_HOST_BRIDGE_FUNCTION vp_sdk_tls_free_bridge, 0x242
VP_HOST_BRIDGE_FUNCTION vp_sdk_tls_get_bridge, 0x243
VP_HOST_BRIDGE_FUNCTION vp_sdk_tls_set_bridge, 0x244
VP_HOST_BRIDGE_FUNCTION vp_sdk_try_lock, 0x245
VP_HOST_BRIDGE_FUNCTION vp_sdk_is_locked, 0x246
VP_HOST_BRIDGE_FUNCTION vp_sdk_os_unavailable, 0x249

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
