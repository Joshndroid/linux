// SPDX-License-Identifier: GPL-2.0-only
/* Bounded single-owner RFS session state; no firmware or modem I/O. */
#include <linux/errno.h>
#include <linux/err.h>
#include <linux/limits.h>
#include <linux/module.h>
#include <linux/unaligned.h>

#include "s5xxx_rfs_session.h"

static void rfs_advance(struct s5xxx_rfs_session *s)
{
	lockdep_assert_held(&s->rxq.lock);
	/* Never recycle an epoch while an old descriptor can still exist. */
	if (s->epoch == U64_MAX)
		s->dead = true;
	else
		s->epoch++;
}

static int rfs_check(struct s5xxx_rfs_session *s, u64 epoch)
{
	lockdep_assert_held(&s->rxq.lock);
	if (s->dead)
		return -ENODEV;
	if (epoch != s->epoch)
		return -EPIPE;
	if (s->failed)
		return -EIO;
	if (!s->opened || epoch != s->open_epoch)
		return -ENOTCONN;
	return 0;
}

static void rfs_fail(struct s5xxx_rfs_session *s)
{
	lockdep_assert_held(&s->rxq.lock);
	if (s->dead || s->failed)
		return;
	__skb_queue_purge(&s->rxq);
	s->failed = true;
	rfs_advance(s);
}

void s5xxx_rfs_session_init(struct s5xxx_rfs_session *s)
{
	skb_queue_head_init(&s->rxq);
	init_waitqueue_head(&s->wait);
	s->epoch = 1;
	s->open_epoch = 0;
	s->opened = false;
	s->failed = false;
	s->dead = false;
}
EXPORT_SYMBOL_GPL(s5xxx_rfs_session_init);

int s5xxx_rfs_session_open(struct s5xxx_rfs_session *s, u64 *epoch)
{
	unsigned long flags;
	int ret = 0;

	spin_lock_irqsave(&s->rxq.lock, flags);
	if (s->dead)
		ret = -ENODEV;
	else if (s->failed)
		ret = -EIO;
	else if (s->opened)
		ret = -EBUSY;
	else {
		s->opened = true;
		s->open_epoch = s->epoch;
		*epoch = s->epoch;
	}
	spin_unlock_irqrestore(&s->rxq.lock, flags);
	return ret;
}
EXPORT_SYMBOL_GPL(s5xxx_rfs_session_open);

void s5xxx_rfs_session_close(struct s5xxx_rfs_session *s, u64 epoch)
{
	unsigned long flags;

	spin_lock_irqsave(&s->rxq.lock, flags);
	if (s->opened && epoch == s->open_epoch) {
		s->opened = false;
		if (!s->dead && epoch == s->epoch) {
			__skb_queue_purge(&s->rxq);
			rfs_advance(s);
		}
		/* Closing a revoked FD must retain newer pre-open requests. */
	}
	spin_unlock_irqrestore(&s->rxq.lock, flags);
	wake_up_interruptible(&s->wait);
}
EXPORT_SYMBOL_GPL(s5xxx_rfs_session_close);

void s5xxx_rfs_session_reset(struct s5xxx_rfs_session *s)
{
	unsigned long flags;

	spin_lock_irqsave(&s->rxq.lock, flags);
	if (!s->dead) {
		__skb_queue_purge(&s->rxq);
		rfs_advance(s);
		s->failed = false;
	}
	spin_unlock_irqrestore(&s->rxq.lock, flags);
	wake_up_interruptible(&s->wait);
}
EXPORT_SYMBOL_GPL(s5xxx_rfs_session_reset);

void s5xxx_rfs_session_lost(struct s5xxx_rfs_session *s, u64 epoch)
{
	unsigned long flags;

	spin_lock_irqsave(&s->rxq.lock, flags);
	/* A copy fault on a revoked FD must not poison newer CP requests. */
	if (!rfs_check(s, epoch))
		rfs_fail(s);
	spin_unlock_irqrestore(&s->rxq.lock, flags);
	wake_up_interruptible(&s->wait);
}
EXPORT_SYMBOL_GPL(s5xxx_rfs_session_lost);

void s5xxx_rfs_session_shutdown(struct s5xxx_rfs_session *s)
{
	unsigned long flags;

	spin_lock_irqsave(&s->rxq.lock, flags);
	s->dead = true;
	__skb_queue_purge(&s->rxq);
	spin_unlock_irqrestore(&s->rxq.lock, flags);
	wake_up_interruptible(&s->wait);
}
EXPORT_SYMBOL_GPL(s5xxx_rfs_session_shutdown);

bool s5xxx_rfs_frame_valid(const void *data, size_t length)
{
	return data && length >= 8 && length <= S5XXX_RFS_FRAME_MAX &&
		get_unaligned_le32((const u8 *)data + 4) == length - 8;
}
EXPORT_SYMBOL_GPL(s5xxx_rfs_frame_valid);

bool s5xxx_rfs_tx_frame_valid(const void *data, size_t length)
{
	const u8 *bytes = data;

	if (s5xxx_rfs_frame_valid(data, length))
		return true;
	/* Exact CP41 factory successful OPEN: body count excludes a trailing
	 * u32 file size. Only this bounded outbound status record is special;
	 * receive validation remains strict and still revokes malformed RX.
	 */
	return bytes && length == 20 &&
		get_unaligned_le16(bytes) == 3 &&
		get_unaligned_le32(bytes + 4) == 8 &&
		get_unaligned_le32(bytes + 8) == 0;
}
EXPORT_SYMBOL_GPL(s5xxx_rfs_tx_frame_valid);

int s5xxx_rfs_session_rx(struct s5xxx_rfs_session *s, struct sk_buff *skb)
{
	unsigned long flags;
	int ret;

	spin_lock_irqsave(&s->rxq.lock, flags);
	if (s->dead)
		ret = -ENODEV;
	else if (s->failed)
		ret = -EIO;
	else if (!skb) {
		ret = -ENOMEM;
		rfs_fail(s);
	} else if (skb_is_nonlinear(skb) ||
		   !s5xxx_rfs_frame_valid(skb->data, skb->len)) {
		ret = -EPROTO;
		rfs_fail(s);
	} else if (s->rxq.qlen >= S5XXX_RFS_RX_FRAMES) {
		ret = -ENOBUFS;
		rfs_fail(s);
	} else {
		__skb_queue_tail(&s->rxq, skb);
		skb = NULL;
		ret = 0;
	}
	spin_unlock_irqrestore(&s->rxq.lock, flags);
	kfree_skb(skb);
	wake_up_interruptible(&s->wait);
	return ret;
}
EXPORT_SYMBOL_GPL(s5xxx_rfs_session_rx);

struct sk_buff *s5xxx_rfs_session_take(struct s5xxx_rfs_session *s, u64 epoch,
				     size_t capacity)
{
	struct sk_buff *skb;
	unsigned long flags;
	int ret;

	spin_lock_irqsave(&s->rxq.lock, flags);
	ret = rfs_check(s, epoch);
	if (ret) {
		skb = ERR_PTR(ret);
		goto out;
	}
	skb = skb_peek(&s->rxq);
	if (!skb)
		skb = ERR_PTR(-EAGAIN);
	else if (skb->len > capacity)
		skb = ERR_PTR(-EMSGSIZE);
	else
		skb = __skb_dequeue(&s->rxq);
out:
	spin_unlock_irqrestore(&s->rxq.lock, flags);
	return skb;
}
EXPORT_SYMBOL_GPL(s5xxx_rfs_session_take);

__poll_t s5xxx_rfs_session_poll(struct s5xxx_rfs_session *s, u64 epoch)
{
	unsigned long flags;
	__poll_t mask;

	spin_lock_irqsave(&s->rxq.lock, flags);
	if (s->dead)
		mask = EPOLLERR | EPOLLHUP;
	else if (rfs_check(s, epoch))
		mask = EPOLLERR;
	else {
		mask = EPOLLOUT | EPOLLWRNORM;
		if (!skb_queue_empty(&s->rxq))
			mask |= EPOLLIN | EPOLLRDNORM;
	}
	spin_unlock_irqrestore(&s->rxq.lock, flags);
	return mask;
}
EXPORT_SYMBOL_GPL(s5xxx_rfs_session_poll);

int s5xxx_rfs_session_tx_begin(struct s5xxx_rfs_session *s, u64 epoch,
				 unsigned long *flags)
{
	int ret;

	spin_lock_irqsave(&s->rxq.lock, *flags);
	ret = rfs_check(s, epoch);
	if (ret)
		spin_unlock_irqrestore(&s->rxq.lock, *flags);
	return ret;
}
EXPORT_SYMBOL_GPL(s5xxx_rfs_session_tx_begin);

void s5xxx_rfs_session_tx_end(struct s5xxx_rfs_session *s, unsigned long flags)
{
	spin_unlock_irqrestore(&s->rxq.lock, flags);
}
EXPORT_SYMBOL_GPL(s5xxx_rfs_session_tx_end);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Single-owner bounded session state for native Exynos RFS");
