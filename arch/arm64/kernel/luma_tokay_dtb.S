/* SPDX-License-Identifier: GPL-2.0-only */

#include <linux/linkage.h>

	.section ".init.rodata", "a"
	.balign 8
SYM_DATA_START(__luma_tokay_dtb_start)
	.incbin "arch/arm64/boot/dts/exynos/google/zumapro-tokay.dtb"
SYM_DATA_END(__luma_tokay_dtb_start)

	.globl __luma_tokay_dtb_end
__luma_tokay_dtb_end:
	.balign 8
