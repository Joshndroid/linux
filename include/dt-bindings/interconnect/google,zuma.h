/* SPDX-License-Identifier: (GPL-2.0-only OR BSD-2-Clause) */
/*
 * Interconnect node IDs for the Google Tensor G3 (zuma) memory interconnect.
 */

#ifndef __DT_BINDINGS_INTERCONNECT_GOOGLE_ZUMA_H
#define __DT_BINDINGS_INTERCONNECT_GOOGLE_ZUMA_H

/* Masters */
#define ZUMA_MASTER_CPUCL0	0
#define ZUMA_MASTER_CPUCL1	1
#define ZUMA_MASTER_CPUCL2	2
#define ZUMA_MASTER_G3D		3

/* Slaves: the buses listed in google,buses, in this order */
#define ZUMA_SLAVE_MIF		4
#define ZUMA_SLAVE_DSU		5
#define ZUMA_SLAVE_INT		6
#define ZUMA_SLAVE_BCI		7

#endif /* __DT_BINDINGS_INTERCONNECT_GOOGLE_ZUMA_H */
