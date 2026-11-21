/*
 * Fast boot trampoline: sits where U-Boot proper would be (BL33 at 0x4a000000)
 * so TF-A treats it like U-Boot:
 *  - TF-A scans the BL33 image for a 64-bit word equal to its load address
 *    followed by the image size, and finds the device tree at base + size
 *    (the SPL appends the FIT's device tree right there). TF-A then programs
 *    the PMIC from it and adds its reservations.
 *  - TF-A enters BL33 with x0..x3 = 0. Linux wants x0 = device tree, so we
 *    load it and jump to the kernel.
 */
	.global _start
_start:
	b	entry
	/* TF-A must find the DT: with none it calls the PMIC setup with a NULL fdt */
	.align	3
	.quad	0x4a000000		/* PRELOADED_BL33_BASE marker for TF-A */
	.quad	_end - _start		/* image size: DT is at base + size */
entry:
#ifdef DEBUG_LED
	/* Debug: green LED (PI12) off = trampoline reached; Linux's LED driver
	 * switches it back on once the kernel is up. */
	ldr	x5, =0x0300b130		/* PIO bank I data register */
	ldr	w6, [x5]
	bic	w6, w6, #(1 << 12)
	str	w6, [x5]
#endif
#ifdef DEBUG_BLINK
	/* Debug: blink the green LED forever instead of starting Linux */
	ldr	x5, =0x0300b130
1:	ldr	w6, [x5]
	eor	w6, w6, #(1 << 12)
	str	w6, [x5]
	mrs	x7, cntpct_el0
	ldr	x8, =6000000		/* 0.25 s at 24 MHz */
	add	x7, x7, x8
2:	mrs	x9, cntpct_el0
	cmp	x9, x7
	b.lo	2b
	b	1b
#endif
	adr	x0, _end		/* device tree appended by the SPL */
	mov	x1, xzr
	mov	x2, xzr
	mov	x3, xzr
	ldr	x4, =KERNEL_ADDR
	br	x4
	.ltorg
	.align	3			/* size multiple of 8: SPL appends at ALIGN(end, 8) */
_end:
