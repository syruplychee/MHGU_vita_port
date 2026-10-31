#ifndef VP_GUEST_CONTEXT_H
#define VP_GUEST_CONTEXT_H
#include <stdint.h>

/* Independently model both guest thread pointer registers. No host TLS writes. */
typedef struct { uint32_t tls_ro, tls_rw; } VPGuestContext;
enum { VP_GUEST_ARGUMENT=-1, VP_GUEST_CONFLICT=-2, VP_GUEST_SEALED=-3, VP_GUEST_CAPACITY=-4 };
#define VP_GUEST_STACK_GRANULE 65536u
/* Mutate only before starting guest workers. Reset only after joining all workers. */
int vp_guest_stack_register(uint32_t, uint32_t, VPGuestContext *);
void vp_guest_registry_seal(void);
void vp_guest_registry_reset(void);
VPGuestContext *vp_guest_context_lookup(uint32_t);
extern VPGuestContext **vp_guest_stack_directory[256];
#endif
