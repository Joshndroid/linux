/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _S5XXX_RFS_SESSION_H
#define _S5XXX_RFS_SESSION_H

#include <linux/poll.h>
#include <linux/skbuff.h>
#include <linux/types.h>
#include <linux/wait.h>

#define S5XXX_RFS_FRAME_MAX 4096
#define S5XXX_RFS_RX_FRAMES 32

/* All state is protected by rxq.lock. Caller owns the object's lifetime. */
struct s5xxx_rfs_session {
	struct sk_buff_head rxq;
	wait_queue_head_t wait;
	u64 epoch;
	u64 open_epoch;
	bool opened;
	bool failed;
	bool dead;
};

void s5xxx_rfs_session_init(struct s5xxx_rfs_session *s);
int s5xxx_rfs_session_open(struct s5xxx_rfs_session *s, u64 *epoch);
void s5xxx_rfs_session_close(struct s5xxx_rfs_session *s, u64 epoch);
void s5xxx_rfs_session_reset(struct s5xxx_rfs_session *s);
void s5xxx_rfs_session_lost(struct s5xxx_rfs_session *s, u64 epoch);
void s5xxx_rfs_session_shutdown(struct s5xxx_rfs_session *s);
/* Consumes skb on every return, including rejection. NULL signals RX loss. */
int s5xxx_rfs_session_rx(struct s5xxx_rfs_session *s, struct sk_buff *skb);
/* One full record; a short user buffer leaves the record queued. */
struct sk_buff *s5xxx_rfs_session_take(struct s5xxx_rfs_session *s, u64 epoch,
				     size_t capacity);
__poll_t s5xxx_rfs_session_poll(struct s5xxx_rfs_session *s, u64 epoch);
bool s5xxx_rfs_frame_valid(const void *data, size_t length);
bool s5xxx_rfs_tx_frame_valid(const void *data, size_t length);
/*
 * A successful begin retains rxq.lock until end: the caller must publish the
 * entire TX record while fenced against reset/loss. In the modem driver the
 * lock order must be modem TX lock -> session lock; never take TX below it.
 * This gate is IRQ-safe and the protected operation must never sleep.
 */
int s5xxx_rfs_session_tx_begin(struct s5xxx_rfs_session *s, u64 epoch,
				 unsigned long *flags);
void s5xxx_rfs_session_tx_end(struct s5xxx_rfs_session *s, unsigned long flags);
#endif
