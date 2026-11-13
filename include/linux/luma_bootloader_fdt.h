/* SPDX-License-Identifier: GPL-2.0 */
/*
 * The bootloader's own FDT, kept by the kernel when it boots from an embedded
 * DT (CONFIG_LUMA_TOKAY_EMBEDDED_DIAGNOSTIC). It carries per-unit data, such
 * as the modem's IMEIs, that must never be copied into the live device tree:
 * /proc/device-tree is readable by every user. A driver that needs such a
 * value reads it from here straight into its own buffer.
 */
#ifndef _LINUX_LUMA_BOOTLOADER_FDT_H
#define _LINUX_LUMA_BOOTLOADER_FDT_H

#include <linux/errno.h>
#include <linux/types.h>

#ifdef CONFIG_LUMA_TOKAY_EMBEDDED_DIAGNOSTIC
/*
 * Copy property @name of node @path into @buf (at most @size bytes).
 * Returns the property's full length, or a negative errno: -ENODEV when no
 * bootloader FDT was kept, -ENOENT when the node or property is absent.
 */
int luma_bootloader_fdt_read_prop(const char *path, const char *name,
				  void *buf, size_t size);
#else
static inline int luma_bootloader_fdt_read_prop(const char *path,
						const char *name,
						void *buf, size_t size)
{
	return -ENODEV;
}
#endif

#endif /* _LINUX_LUMA_BOOTLOADER_FDT_H */
