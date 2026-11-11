/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __LUMA_TOKAY_FDT_H
#define __LUMA_TOKAY_FDT_H
#include <linux/init.h>
void __init luma_import_memory(const void *original);
void __init luma_tokay_reserve_firmware(const void *original);
int __init luma_tokay_import_wlan_address(const void *original);
int __init luma_tokay_import_bluetooth_address(const void *original);
int __init luma_import_board_info(const void *original);
#endif
