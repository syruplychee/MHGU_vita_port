.syntax unified
.arch armv7-a
.fpu neon-vfpv3
.arm
.text
#include "host_bridge.inc.S"
/* Deliberately rejects the first unavailable SDK initialization service.
 * It never supplies a successful replacement for __nnmusl_init_dso. */
VP_HOST_BRIDGE_FUNCTION vp_loader_dso_unavailable, 0x226
VP_HOST_BRIDGE_FUNCTION vp_loader_fixture_init, 0x227
VP_HOST_BRIDGE_FUNCTION vp_loader_fixture_fini, 0x228
VP_HOST_BRIDGE_FUNCTION vp_loader_fixture_entry, 0x229
.section .note.GNU-stack,"",%progbits
