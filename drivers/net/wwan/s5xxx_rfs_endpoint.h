/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _S5XXX_RFS_ENDPOINT_H
#define _S5XXX_RFS_ENDPOINT_H
#include "s5xxx_rfs_session.h"

struct device;
struct s5xxx_rfs_endpoint;
/*
 * Synchronous emission: return zero only after the full frame is published.
 * The modem callback must take its TX lock, then the supplied session's TX
 * gate, and retain both through the complete write/doorbell operation.
 */
typedef int (*s5xxx_rfs_emit_t)(void *context,
			      struct s5xxx_rfs_session *session, u64 epoch,
			      const void *frame, size_t length);
struct s5xxx_rfs_endpoint *s5xxx_rfs_endpoint_create(struct device *parent,
					 s5xxx_rfs_emit_t emit, void *context);
/* Stop new opens/TX, invalidate open files; retain the driver's reference. */
void s5xxx_rfs_endpoint_detach(struct s5xxx_rfs_endpoint *ep);
/* Driver drops this reference only after its RX/reset IRQs are quiesced. */
void s5xxx_rfs_endpoint_put(struct s5xxx_rfs_endpoint *ep);
void s5xxx_rfs_endpoint_reset(struct s5xxx_rfs_endpoint *ep);
int s5xxx_rfs_endpoint_rx(struct s5xxx_rfs_endpoint *ep, struct sk_buff *skb);
#endif
