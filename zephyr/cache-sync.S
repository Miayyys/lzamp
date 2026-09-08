/* SPDX-License-Identifier: Apache-2.0 */
/* Cache maintenance used at the LZAMP CPU3 startup boundary. */

	.section .text.lzamp_cache_sync, "ax"
	.global lzamp_invalidate_private_cache
	.type lzamp_invalidate_private_cache, %function
lzamp_invalidate_private_cache:
	/* x0=start, x1=end (exclusive); derive the implemented D-cache line. */
	mrs	x2, ctr_el0
	ubfx	x2, x2, #16, #4
	mov	x3, #4
	lsl	x3, x3, x2
	sub	x4, x3, #1
	bic	x0, x0, x4
	add	x1, x1, x4
	bic	x1, x1, x4

	/* Publish C=0 initialization stores before invalidating old lines. */
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
	.size lzamp_invalidate_private_cache, . - lzamp_invalidate_private_cache
