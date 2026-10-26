/* SPDX-License-Identifier: MIT OR GPL-2.0-only */
/*
 * agpu_om.S -- the Omega units of aien-gpu, byte for byte as the pinned omc wrote them, and the one
 * entry trampoline that calls them. Assembled unchanged into the kernel module and into the host
 * tests (tests/host), so the host tests run the very words the module runs.
 *
 * omc function mode (omega-mv docs/one_layer/OMEGESE.md 9.1; docs/osc/OSC-1-DESIGN.md section 7):
 * parameters in x0..x5 (an edit/view array arrives as its address), result in x0, the runtime table
 * pointer in x7, a trap loads the table into x0 and its code into x1 and branches (br, no return) to
 * the table entry at offset 16. Word 0 of a unit branches to its last function, the one entry.
 * The code uses x0..x16, x29, x30 and sp only; tools/kcheck.om refuses, per build, any unit word that
 * names x17..x28, a SIMD/FP register, a system instruction, an svc, a literal or adr load, or an
 * indirect branch other than the trap entry, and bounds its stack use (docs/OMEGA-IN-KERNEL.md).
 *
 * u64 agpu_om_enter(const void *entry, const u64 args[6], u64 *status)
 *   *status = 0 and the result, or *status = trap code (1..14) and 0, or 34 when the code reached a
 *   runtime entry other than trap (never, if kcheck passed). A trap unwinds by restoring sp from x19:
 *   the Omega frames hold no locks and no resources, so dropping them is the whole cleanup.
 * The trampoline saves x19..x28 (it uses x19 itself; the rest stay callee-saved for the C caller).
 * x18 (the kernel's shadow call stack pointer) is never touched here and kcheck keeps it out of
 * the units. No FP/SIMD register is used anywhere, so no kernel_neon_begin is needed.
 */

	.text
	.balign	8
	.globl	agpu_om_enter
	.type	agpu_om_enter, %function
agpu_om_enter:
	hint	#34			/* bti c: a C caller may reach us indirectly */
	stp	x29, x30, [sp, #-112]!
	mov	x29, sp
	stp	x19, x20, [sp, #16]
	stp	x21, x22, [sp, #32]
	stp	x23, x24, [sp, #48]
	stp	x25, x26, [sp, #64]
	stp	x27, x28, [sp, #80]
	str	x2, [sp, #96]		/* status pointer */
	mov	x19, sp			/* the frame a trap returns to */
	mov	x16, x0
	mov	x9, x1
	ldp	x0, x1, [x9]
	ldp	x2, x3, [x9, #16]
	ldp	x4, x5, [x9, #32]
	ldr	x7, .Lrt_addr
	blr	x16
	ldr	x2, [x19, #96]
	str	xzr, [x2]
	b	.Lout
.Ltrap:					/* entered by br from a unit: x0 = table, x1 = code */
	hint	#34
	mov	sp, x19
	ldr	x2, [sp, #96]
	str	x1, [x2]
	mov	x0, #0
.Lout:
	ldp	x19, x20, [sp, #16]
	ldp	x21, x22, [sp, #32]
	ldp	x23, x24, [sp, #48]
	ldp	x25, x26, [sp, #64]
	ldp	x27, x28, [sp, #80]
	ldp	x29, x30, [sp], #112
	ret
.Lbad:					/* any runtime entry but trap: status 34 */
	hint	#34
	mov	x1, #34
	b	.Ltrap
	.balign	8
.Lrt_addr:
	.quad	agpu_om_rt
	.size	agpu_om_enter, . - agpu_om_enter

	.section .rodata
	.balign	8
	.type	agpu_om_rt, %object
agpu_om_rt:				/* OscRt: 32 entries; offset 16 is trap */
	.quad	.Lbad, .Lbad, .Ltrap, .Lbad, .Lbad, .Lbad, .Lbad, .Lbad
	.quad	.Lbad, .Lbad, .Lbad, .Lbad, .Lbad, .Lbad, .Lbad, .Lbad
	.quad	.Lbad, .Lbad, .Lbad, .Lbad, .Lbad, .Lbad, .Lbad, .Lbad
	.quad	.Lbad, .Lbad, .Lbad, .Lbad, .Lbad, .Lbad, .Lbad, .Lbad
	.size	agpu_om_rt, . - agpu_om_rt

/* One unit: a bti landing pad, then the unit's words (word 0 branches to its entry function). */
	.macro	OMUNIT name, file
	.text
	.balign	8
	.globl	\name
	.type	\name, %function
\name:
	hint	#34
	.incbin	"\file"
	.size	\name, . - \name
	.endm

	OMUNIT	agpu_om_ident, "ident.bin"
	OMUNIT	agpu_om_boot, "boot.bin"
	OMUNIT	agpu_om_seq, "seq.bin"
	OMUNIT	agpu_om_seal, "seal.bin"
	OMUNIT	agpu_om_mqput, "mqput.bin"
	OMUNIT	agpu_om_mqget, "mqget.bin"
	OMUNIT	agpu_om_abi, "abi.bin"
