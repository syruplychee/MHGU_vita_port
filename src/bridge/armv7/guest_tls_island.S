/* Branch-entered patch islands for the SDK p15/c13 TLS access sites.
 *
 * Shape (all A32, unconditional):
 *   site word         : b <island>          <- written by the patch tool, 1 word
 *   island + 0x00     : leaf core           <- same body as the tested inline leaf
 *   island + fail     : udf #0x72           <- local trap; never branches into host code
 *   island + return   : b <site + 4>        <- placeholder word, written by the patch tool
 *
 * Entered with `b`, so LR, the condition flags and the stack pointer are untouched
 * on entry; the leaf therefore keeps its tested 12-byte stack delta. The island
 * never writes LR and never calls anything, so a site inside a function that does
 * not save LR (15 of the 57 audited sites) stays correct.
 *
 * Dropped zero placeholder for the return word and the movw/movt pair carrying
 * the guest stack directory address are the only words a placer rewrites: the
 * pair is located by pattern (movw then matching movt on the same register), and
 * the immediate halves are replaced while the destination register is kept.
 * Everything else in the island is position independent.
 */
.syntax unified
.arch armv7-a
.text
.balign 4
#include "guest_tls.inc.S"

.macro VP_TLS_ISLAND suffix, leaf
.global vp_tls_island_\suffix\()_begin, vp_tls_island_\suffix\()_end
.global vp_tls_island_\suffix\()_fail, vp_tls_island_\suffix\()_return
vp_tls_island_\suffix\()_begin:
    \leaf 2f
    b 3f
2:
vp_tls_island_\suffix\()_fail:
    udf #0x72
3:
vp_tls_island_\suffix\()_return:
    .word 0
vp_tls_island_\suffix\()_end:
.endm

VP_TLS_ISLAND read_ro, VP_TLS_READ_RO_R1
VP_TLS_ISLAND write_rw, VP_TLS_WRITE_RW_R4
VP_TLS_ISLAND read_rw, VP_TLS_READ_RW_R1
VP_TLS_ISLAND read_ro_r0, VP_TLS_READ_RO_R0
VP_TLS_ISLAND read_ro_r2, VP_TLS_READ_RO_R2
VP_TLS_ISLAND write_rw_r0, VP_TLS_WRITE_RW_R0

.section .note.GNU-stack,"",%progbits
