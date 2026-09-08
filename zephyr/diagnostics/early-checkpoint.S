/* SPDX-License-Identifier: MIT */
/* Early checkpoints required by the locally instrumented Zephyr workspace. */
	.section .text.z_early_checkpoint, "ax"
	.global z_arm64_el2_plat_prep_c
	.type z_arm64_el2_plat_prep_c, %function
z_arm64_el2_plat_prep_c:
	movz	x0, #0xf000
	movk	x0, #0x500f, lsl #16
	movz	x1, #0x3250                 /* "EL2P" */
	movk	x1, #0x454c, lsl #16
	str	x1, [x0]
	mrs	x1, CurrentEL
	str	x1, [x0, #8]
	dc	cvac, x0
	add	x0, x0, #8
	dc	cvac, x0
	dsb	sy
	ret
	.size z_arm64_el2_plat_prep_c, . - z_arm64_el2_plat_prep_c

	.global r1_amp_checkpoint_post_el1_reset
	.type r1_amp_checkpoint_post_el1_reset, %function
r1_amp_checkpoint_post_el1_reset:
	movz	x0, #0xf000
	movk	x0, #0x500f, lsl #16
	movz	x1, #0x5431                 /* "RST1" */
	movk	x1, #0x5253, lsl #16
	str	x1, [x0]
	mrs	x1, CurrentEL
	str	x1, [x0, #8]
	dc	cvac, x0
	add	x0, x0, #8
	dc	cvac, x0
	dsb	sy
	ret
	.size r1_amp_checkpoint_post_el1_reset, . - r1_amp_checkpoint_post_el1_reset

	.global r1_amp_early_checkpoint
	.type r1_amp_early_checkpoint, %function
r1_amp_early_checkpoint:
	movz	x1, #0xf000
	movk	x1, #0x503f, lsl #16
	str	x0, [x1]
	mrs	x2, CurrentEL
	str	x2, [x1, #8]
	dc	cvac, x1
	add	x1, x1, #8
	dc	cvac, x1
	dsb	sy
	ret
	.size r1_amp_early_checkpoint, . - r1_amp_early_checkpoint

	/*
	 * Enable EL1 MMU/cache and record a marker without a C call or stack
	 * access across the architectural synchronization boundary.  x0 carries
	 * the SCTLR value prepared by enable_mmu_el1().
	 */
	.global r1_amp_enable_mmu_checkpoint
	.type r1_amp_enable_mmu_checkpoint, %function
r1_amp_enable_mmu_checkpoint:
	mov	x2, x0
	movz	x1, #0xf000
	movk	x1, #0x503f, lsl #16
	movz	x0, #0x3341                 /* "MM3A" */
	movk	x0, #0x4d4d, lsl #16
	str	x0, [x1]
	mrs	x3, CurrentEL
	str	x3, [x1, #8]
	dc	cvac, x1
	add	x1, x1, #8
	dc	cvac, x1
	dsb	sy

	msr	sctlr_el1, x2
	isb

	movz	x1, #0xf000
	movk	x1, #0x503f, lsl #16
	movz	x0, #0x3345                 /* "MM3E" */
	movk	x0, #0x4d4d, lsl #16
	str	x0, [x1]
	mrs	x3, CurrentEL
	str	x3, [x1, #8]
	dc	cvac, x1
	add	x1, x1, #8
	dc	cvac, x1
	dsb	sy
	ret
	.size r1_amp_enable_mmu_checkpoint, . - r1_amp_enable_mmu_checkpoint

	/* Invalidate only the private Zephyr runtime range while C is disabled. */
	.global r1_amp_invalidate_private_cache
	.type r1_amp_invalidate_private_cache, %function
r1_amp_invalidate_private_cache:
	/* x0=start, x1=end (exclusive); derive the implemented D-cache line. */
	mrs	x2, ctr_el0
	ubfx	x2, x2, #16, #4
	mov	x3, #4
	lsl	x3, x3, x2
	sub	x4, x3, #1
	bic	x0, x0, x4
	add	x1, x1, x4
	bic	x1, x1, x4
	/* Publish the C=0 initialization stores before invalidating stale lines. */
	dsb	sy
1:
	cmp	x0, x1
	b.hs	2f
	dc	ivac, x0
	add	x0, x0, x3
	b	1b
2:
	dsb	sy
	isb
	ret
	.size r1_amp_invalidate_private_cache, . - r1_amp_invalidate_private_cache

	/* Enable EL1 D-cache after translation is already active. */
	.global r1_amp_enable_cache_checkpoint
	.type r1_amp_enable_cache_checkpoint, %function
r1_amp_enable_cache_checkpoint:
	mov	x2, x0
	movz	x1, #0xf000
	movk	x1, #0x503f, lsl #16
	movz	x0, #0x3030                 /* "CA00" */
	movk	x0, #0x4341, lsl #16
	str	x0, [x1]
	mrs	x3, CurrentEL
	str	x3, [x1, #8]
	dc	cvac, x1
	add	x1, x1, #8
	dc	cvac, x1
	dsb	sy

	msr	sctlr_el1, x2
	isb

	movz	x1, #0xf000
	movk	x1, #0x503f, lsl #16
	movz	x0, #0x3031                 /* "CA01" */
	movk	x0, #0x4341, lsl #16
	str	x0, [x1]
	mrs	x3, CurrentEL
	str	x3, [x1, #8]
	dc	cvac, x1
	add	x1, x1, #8
	dc	cvac, x1
	dsb	sy
	ret
	.size r1_amp_enable_cache_checkpoint, . - r1_amp_enable_cache_checkpoint

	/*
	 * Temporary fatal-exception capture. SError enters before the normal
	 * vector prologue; unhandled sync enters after the original exception
	 * frame and recoverable FP/SVC dispatch. The capture itself uses no
	 * stack, C calls, global data, or cache-mode changes. The
	 * local Zephyr vector_table.S hooks are diagnostic-only, not a recovery
	 * mechanism. Preserve the existing MMU/cache failure for inspection.
	 *
	 * status.magic: EX00/EX01 (legacy raw sync hooks, unused),
	 *               EX02 (SError/SP0), EX03 (SError/SPx),
	 *               EX05 (unhandled sync, after exception prologue).
	 * status.current_el remains CurrentEL.
	 * mbox_observation: ESR[31:0] / ELR[31:0] / FAR[31:0] / SPSR[31:0].
	 * Additional full-width snapshot at control+0x150: ELR,FAR,SCTLR,SP,LR.
	 * For EX05, SP points to the original Zephyr exception frame and LR
	 * belongs to the handler; the interrupted registers remain in that frame.
	 * These bytes are diagnostic state only; the CPU parks without returning.
	 */
	.global r1_amp_fault_sync_sp0
	.type r1_amp_fault_sync_sp0, %function
r1_amp_fault_sync_sp0:
	mov	x0, #0
	b	r1_amp_fault_capture
	.size r1_amp_fault_sync_sp0, . - r1_amp_fault_sync_sp0

	.global r1_amp_fault_sync_spx
	.type r1_amp_fault_sync_spx, %function
r1_amp_fault_sync_spx:
	mov	x0, #1
	b	r1_amp_fault_capture
	.size r1_amp_fault_sync_spx, . - r1_amp_fault_sync_spx

	.global r1_amp_fault_serror_sp0
	.type r1_amp_fault_serror_sp0, %function
r1_amp_fault_serror_sp0:
	mov	x0, #2
	b	r1_amp_fault_capture
	.size r1_amp_fault_serror_sp0, . - r1_amp_fault_serror_sp0

	.global r1_amp_fault_serror_spx
	.type r1_amp_fault_serror_spx, %function
r1_amp_fault_serror_spx:
	mov	x0, #3
	b	r1_amp_fault_capture
	.size r1_amp_fault_serror_spx, . - r1_amp_fault_serror_spx

	.global r1_amp_fault_unhandled
	.type r1_amp_fault_unhandled, %function
r1_amp_fault_unhandled:
	mov	x0, #5
	b	r1_amp_fault_capture
	.size r1_amp_fault_unhandled, . - r1_amp_fault_unhandled

	.type r1_amp_fault_capture, %function
r1_amp_fault_capture:
	msr	DAIFSet, #0xf
	mrs	x1, ESR_EL1
	mrs	x2, ELR_EL1
	mrs	x3, FAR_EL1
	mrs	x4, SPSR_EL1
	mrs	x5, SCTLR_EL1
	mov	x6, sp
	tbnz	x0, #0, 1f
	mrs	x6, SP_EL0
1:
	mrs	x7, CurrentEL
	movz	x10, #0xf000
	movk	x10, #0x503f, lsl #16
	add	x11, x10, #0x140
	stp	w1, w2, [x11]
	stp	w3, w4, [x11, #8]
	stp	x2, x3, [x11, #16]
	stp	x5, x6, [x11, #32]
	str	x30, [x11, #48]
	dc	cvac, x11
	dsb	sy
	movz	x8, #0x3030
	movk	x8, #0x4558, lsl #16
	add	x8, x8, x0
	str	x7, [x10, #8]
	str	x8, [x10]
	dc	cvac, x10
	dsb	sy
2:
	wfe
	b	2b
	.size r1_amp_fault_capture, . - r1_amp_fault_capture
