/* SPDX-License-Identifier: MIT OR GPL-2.0-only */
/* omhost.h -- host-side declarations of the Omega units in kmod/agpu_om.S (the same object the module
 * links). Host tests call the very words the kernel module runs, through the same trampoline. */
#ifndef AGPU_OMHOST_H
#define AGPU_OMHOST_H
#include <stdint.h>

uint64_t agpu_om_enter(const void *entry, const uint64_t args[6], uint64_t *status);
extern const char agpu_om_ident[], agpu_om_boot[], agpu_om_seq[], agpu_om_seal[], agpu_om_mqput[],
	agpu_om_mqget[], agpu_om_abi[];

static inline uint64_t om_call(const void *entry, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
			       uint64_t a4, uint64_t a5, uint64_t *status)
{
	const uint64_t a[6] = { a0, a1, a2, a3, a4, a5 };
	return agpu_om_enter(entry, a, status);
}
#define P(x) ((uint64_t)(uintptr_t)(x))
#endif
