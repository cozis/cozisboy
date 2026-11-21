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
	.align	3
	.quad	0x4a000000		/* PRELOADED_BL33_BASE marker for TF-A */
	.quad	_end - _start		/* image size: DT is at base + size */
entry:
	adr	x0, _end		/* device tree appended by the SPL */
	mov	x1, xzr
	mov	x2, xzr
	mov	x3, xzr
	ldr	x4, =KERNEL_ADDR
	br	x4
	.ltorg
	.align	3			/* size multiple of 8: SPL appends at ALIGN(end, 8) */
_end:
