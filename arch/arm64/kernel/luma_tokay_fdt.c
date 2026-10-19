// SPDX-License-Identifier: GPL-2.0-only
/* Tokay lab handover: retain firmware-owned RAM without importing Android
 * device bindings. Only validated provisioned WLAN and Bluetooth addresses
 * are handed over as native properties; other identifiers remain excluded.
 * Provenance: live bootloader FDT from CP41.260814.003.B1, audited against
 * the native DT and /proc/iomem. All work is bounded by the arm64 FDT limit.
 */
#include <linux/init.h>
#include <linux/etherdevice.h>
#include <linux/hex.h>
#include <linux/kernel.h>
#include <linux/libfdt.h>
#include <linux/memblock.h>
#include <linux/of.h>
#include <linux/of_fdt.h>
#include <asm/memory.h>
#include <asm/sections.h>
#include "luma_tokay_fdt.h"

/* Preserve authentic provisioned radio addresses. The vendor consumes
 * /chosen/config; expose only the validated addresses through standard
 * native OF properties instead of importing the entire private subtree.
 */
static int __init luma_tokay_import_address(const void *original,
		const char *source_property, const char *node_path,
		const char *native_property, bool reverse)
{
	struct device_node *node;
	struct property *prop;
	const char *text;
	u8 mac[ETH_ALEN];
	int chosen, len, ret;

	chosen = fdt_path_offset(original, "/chosen/config");
	if (chosen < 0)
		return -ENOENT;
	text = fdt_getprop(original, chosen, source_property, &len);
	if (!text || len != 18 || text[17] || !mac_pton(text, mac) ||
	    !is_valid_ether_addr(mac) || is_local_ether_addr(mac))
		return -EINVAL;
	if (reverse) {
		swap(mac[0], mac[5]);
		swap(mac[1], mac[4]);
		swap(mac[2], mac[3]);
	}
	node = of_find_node_by_path(node_path);
	if (!node)
		return -ENODEV;
	prop = kzalloc(sizeof(*prop), GFP_KERNEL);
	if (!prop) {
		of_node_put(node);
		return -ENOMEM;
	}
	prop->name = kstrdup(native_property, GFP_KERNEL);
	prop->value = kmemdup(mac, ETH_ALEN, GFP_KERNEL);
	prop->length = ETH_ALEN;
	OF_MARK_DYNAMIC(prop);
	ret = (!prop->name || !prop->value) ? -ENOMEM :
		of_update_property(node, prop);
	of_node_put(node);
	if (ret) {
		kfree(prop->name);
		kfree(prop->value);
		kfree(prop);
	}
	return ret;
}

int __init luma_tokay_import_wlan_address(const void *original)
{
	return luma_tokay_import_address(original, "wlan_mac1", "/wlan",
					"local-mac-address", false);
}

int __init luma_tokay_import_bluetooth_address(const void *original)
{
	/* Bluetooth's standard local-bd-address uses least-significant byte
	 * first, unlike the bootloader's conventional colon-separated text.
	 */
	return luma_tokay_import_address(original, "bt_addr", "/bluetooth",
					"local-bd-address", true);
}

static bool __init node_available(const void *fdt, int node)
{
	int len;
	const char *status = fdt_getprop(fdt, node, "status", &len);

	return !status || (len == 5 && !memcmp(status, "okay", 5)) ||
		(len == 3 && !memcmp(status, "ok", 3));
}

/* Native OF must remain responsible for its own memory-region bindings.
 * Return 1 for full coverage, 0 for a separate region, and reject a partially
 * conflicting description rather than silently changing native ownership.
 */
static int __init native_covers(phys_addr_t base, phys_addr_t size)
{
	const void *fdt = initial_boot_params;
	int parent = fdt_path_offset(fdt, "/reserved-memory");
	int ac, sc, node;
	bool overlap = false;

	if (parent < 0)
		return 0;
	ac = fdt_address_cells(fdt, parent);
	sc = fdt_size_cells(fdt, parent);
	if (ac < 1 || ac > 2 || sc < 1 || sc > 2)
		panic("Luma FDT: unsupported native reservation cells");

	fdt_for_each_subnode(node, fdt, parent) {
		int len, i, cells = ac + sc;
		const __be32 *reg;

		if (!node_available(fdt, node))
			continue;
		reg = fdt_getprop(fdt, node, "reg", &len);
		if (!reg)
			continue;
		if (len % (cells * sizeof(*reg)))
			panic("Luma FDT: malformed native reservation");
		for (i = 0; i < len / sizeof(*reg); i += cells) {
			phys_addr_t start = of_read_number(reg + i, ac);
			phys_addr_t bytes = of_read_number(reg + i + ac, sc);

			if (!bytes)
				continue;
			if (start + bytes < start)
				panic("Luma FDT: native reservation overflow");
			if (base >= start && base + size <= start + bytes)
				return 1;
			if (base < start + bytes && start < base + size)
				overlap = true;
		}
	}
	return overlap ? -ERANGE : 0;
}

static bool __init protect_range(phys_addr_t base, phys_addr_t size, bool nomap)
{
	phys_addr_t kernel_start = __pa_symbol(_text);
	phys_addr_t kernel_end = __pa_symbol(_end);
	int covered, ret;

	if (!size)
		return false;
	if (base + size < base)
		panic("Luma FDT: firmware reservation overflow");
	/* Some vendor entries describe SRAM/MMIO. They are not allocator RAM. */
	if (!memblock_is_region_memory(base, size))
		return false;
	if (base < kernel_end && kernel_start < base + size)
		panic("Luma FDT: firmware reservation overlaps kernel");
	covered = native_covers(base, size);
	if (covered < 0)
		panic("Luma FDT: conflicting firmware/native reservations");
	if (covered)
		return false;

	ret = nomap ? memblock_mark_nomap(base, size) : memblock_reserve(base, size);
	if (ret)
		panic("Luma FDT: cannot retain firmware reservation");
	return true;
}

void __init luma_tokay_reserve_firmware(const void *original)
{
	int parent, ac, sc, node, i, retained = 0;
	uint64_t base, size;

	/* Use the libfdt routines linked by Linux, and reject malformed or
	 * non-progressing structure walks before reading reservation nodes.
	 */
	if (fdt_check_header(original))
		panic("Luma FDT: malformed original header");
	node = -1;
	for (;;) {
		int next = fdt_next_node(original, node, NULL);

		if (next == -FDT_ERR_NOTFOUND)
			break;
		if (next < 0 || next <= node)
			panic("Luma FDT: malformed original structure");
		node = next;
	}
	for (i = 0; i < fdt_num_mem_rsv(original); i++) {
		if (fdt_get_mem_rsv(original, i, &base, &size))
			panic("Luma FDT: malformed reserve map");
		retained += protect_range(base, size, false);
	}
	parent = fdt_path_offset(original, "/reserved-memory");
	if (parent < 0)
		return;
	ac = fdt_address_cells(original, parent);
	sc = fdt_size_cells(original, parent);
	if (ac < 1 || ac > 2 || sc < 1 || sc > 2)
		panic("Luma FDT: unsupported firmware reservation cells");
	fdt_for_each_subnode(node, original, parent) {
		int len, cells = ac + sc;
		const __be32 *reg;
		bool nomap;

		if (!node_available(original, node))
			continue;
		reg = fdt_getprop(original, node, "reg", &len);
		if (!reg)
			continue;
		if (len % (cells * sizeof(*reg)))
			panic("Luma FDT: malformed firmware reservation");
		nomap = fdt_getprop(original, node, "no-map", NULL) != NULL;
		for (i = 0; i < len / sizeof(*reg); i += cells) {
			base = of_read_number(reg + i, ac);
			size = of_read_number(reg + i + ac, sc);
			retained += protect_range(base, size, nomap);
		}
	}
	pr_info("Luma lab: retained %d additional bootloader RAM reservations\n",
		retained);
}
