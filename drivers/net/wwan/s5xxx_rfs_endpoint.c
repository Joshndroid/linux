// SPDX-License-Identifier: GPL-2.0-only
/* Root-only record transport. This component itself performs no modem I/O. */
#include <linux/device.h>
#include <linux/err.h>
#include <linux/fs.h>
#include <linux/kref.h>
#include <linux/jiffies.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/uio.h>
#include "s5xxx_rfs_endpoint.h"

struct s5xxx_rfs_endpoint {
	struct miscdevice misc;
	struct kref refs;
	struct mutex io_lock;
	struct s5xxx_rfs_session session;
	atomic_t registered;
	s5xxx_rfs_emit_t emit;
	void *context;
};

struct rfs_open_file {
	struct s5xxx_rfs_endpoint *ep;
	u64 epoch;
};

static void endpoint_free(struct kref *refs)
{
	struct s5xxx_rfs_endpoint *ep =
		container_of(refs, struct s5xxx_rfs_endpoint, refs);

	WARN_ON(atomic_read(&ep->registered));
	s5xxx_rfs_session_shutdown(&ep->session);
	kfree(ep->misc.name);
	kfree(ep);
}

static int endpoint_open(struct inode *inode, struct file *file)
{
	struct miscdevice *misc = file->private_data;
	struct s5xxx_rfs_endpoint *ep =
		container_of(misc, struct s5xxx_rfs_endpoint, misc);
	struct rfs_open_file *opened;
	int ret;

	opened = kzalloc(sizeof(*opened), GFP_KERNEL);
	if (!opened)
		return -ENOMEM;
	mutex_lock(&ep->io_lock);
	ret = s5xxx_rfs_session_open(&ep->session, &opened->epoch);
	if (!ret) {
		kref_get(&ep->refs);
		opened->ep = ep;
		file->private_data = opened;
	}
	mutex_unlock(&ep->io_lock);
	if (ret) {
		kfree(opened);
		return ret;
	}
	return stream_open(inode, file);
}

static int endpoint_release(struct inode *inode, struct file *file)
{
	struct rfs_open_file *opened = file->private_data;
	struct s5xxx_rfs_endpoint *ep = opened->ep;

	mutex_lock(&ep->io_lock);
	s5xxx_rfs_session_close(&ep->session, opened->epoch);
	mutex_unlock(&ep->io_lock);
	kfree(opened);
	file->private_data = NULL;
	kref_put(&ep->refs, endpoint_free);
	return 0;
}

static ssize_t endpoint_read(struct kiocb *iocb, struct iov_iter *to)
{
	struct rfs_open_file *opened = iocb->ki_filp->private_data;
	struct s5xxx_rfs_endpoint *ep = opened->ep;
	struct sk_buff *skb;
	__poll_t mask;
	ssize_t ret;

	if (!iov_iter_count(to))
		return 0;
	for (;;) {
		ret = mutex_lock_interruptible(&ep->io_lock);
		if (ret)
			return ret;
		skb = s5xxx_rfs_session_take(&ep->session, opened->epoch,
					   iov_iter_count(to));
		if (!IS_ERR(skb))
			break;
		ret = PTR_ERR(skb);
		mutex_unlock(&ep->io_lock);
		if (ret != -EAGAIN || (iocb->ki_filp->f_flags & O_NONBLOCK))
			return ret;
		ret = wait_event_interruptible(ep->session.wait,
			s5xxx_rfs_session_poll(&ep->session, opened->epoch) &
			(EPOLLIN | EPOLLERR | EPOLLHUP));
		if (ret)
			return ret;
	}
	if (copy_to_iter(skb->data, skb->len, to) != skb->len) {
		/* Never let a partially copied record advance the protocol. */
		s5xxx_rfs_session_lost(&ep->session, opened->epoch);
		ret = -EFAULT;
	} else {
		mask = s5xxx_rfs_session_poll(&ep->session, opened->epoch);
		ret = (mask & EPOLLHUP) ? -ENODEV :
		      (mask & EPOLLERR) ? -EPIPE : skb->len;
	}
	kfree_skb(skb);
	mutex_unlock(&ep->io_lock);
	return ret;
}

static ssize_t endpoint_write(struct kiocb *iocb, struct iov_iter *from)
{
	struct rfs_open_file *opened = iocb->ki_filp->private_data;
	struct s5xxx_rfs_endpoint *ep = opened->ep;
	size_t length = iov_iter_count(from);
	__poll_t mask;
	void *frame;
	unsigned long deadline = jiffies + msecs_to_jiffies(30000);
	long waited;
	int ret;

	if (length < 8 || length > S5XXX_RFS_FRAME_MAX)
		return -EMSGSIZE;
	frame = kmalloc(length, GFP_KERNEL);
	if (!frame)
		return -ENOMEM;
	if (!copy_from_iter_full(frame, length, from)) {
		ret = -EFAULT;
		goto free;
	}
	if (!s5xxx_rfs_tx_frame_valid(frame, length)) {
		ret = -EPROTO;
		goto free;
	}
retry:
	ret = mutex_lock_interruptible(&ep->io_lock);
	if (ret)
		goto free;
	mask = s5xxx_rfs_session_poll(&ep->session, opened->epoch);
	if (!ep->emit || (mask & EPOLLHUP))
		ret = -ENODEV;
	else if (mask & EPOLLERR)
		ret = -EPIPE;
	else {
		ret = ep->emit(ep->context, &ep->session, opened->epoch,
			       frame, length);
		if (ret > 0) {
			/* The callback must never report an ambiguous short send. */
			s5xxx_rfs_session_lost(&ep->session, opened->epoch);
			ret = -EIO;
		}
	}
	mutex_unlock(&ep->io_lock);
	if (ret == -EAGAIN && !(iocb->ki_filp->f_flags & O_NONBLOCK) &&
	    time_before(jiffies, deadline)) {
		/* No record was published. Retain this frame and epoch while waiting.
		 * CP reset/removal wakes immediately; ring space is retried at 50 Hz.
		 * Keep io_lock released so reads and detach cannot deadlock behind TX.
		 */
		waited = wait_event_interruptible_timeout(ep->session.wait,
			s5xxx_rfs_session_poll(&ep->session, opened->epoch) &
			(EPOLLERR | EPOLLHUP), msecs_to_jiffies(20));
		if (waited < 0) {
			ret = waited;
			goto free;
		}
		goto retry;
	}
free:
	kfree(frame);
	if (ret)
		return ret;
	return length;
}

static __poll_t endpoint_poll(struct file *file, poll_table *wait)
{
	struct rfs_open_file *opened = file->private_data;
	struct s5xxx_rfs_endpoint *ep = opened->ep;

	poll_wait(file, &ep->session.wait, wait);
	return s5xxx_rfs_session_poll(&ep->session, opened->epoch);
}

static const struct file_operations endpoint_fops = {
	.owner = THIS_MODULE,
	.open = endpoint_open,
	.release = endpoint_release,
	.read_iter = endpoint_read,
	.write_iter = endpoint_write,
	.poll = endpoint_poll,
};

struct s5xxx_rfs_endpoint *s5xxx_rfs_endpoint_create(struct device *parent,
					 s5xxx_rfs_emit_t emit, void *context)
{
	struct s5xxx_rfs_endpoint *ep;
	int ret;

	if (!parent || !emit)
		return ERR_PTR(-EINVAL);
	ep = kzalloc(sizeof(*ep), GFP_KERNEL);
	if (!ep)
		return ERR_PTR(-ENOMEM);
	kref_init(&ep->refs);
	mutex_init(&ep->io_lock);
	s5xxx_rfs_session_init(&ep->session);
	ep->emit = emit;
	ep->context = context;
	ep->misc.name = kasprintf(GFP_KERNEL, "s5xxx-rfs-%s", dev_name(parent));
	if (!ep->misc.name) {
		ret = -ENOMEM;
		goto fail;
	}
	ep->misc.minor = MISC_DYNAMIC_MINOR;
	ep->misc.fops = &endpoint_fops;
	ep->misc.parent = parent;
	ep->misc.mode = 0600;
	ret = misc_register(&ep->misc);
	if (ret)
		goto fail;
	atomic_set(&ep->registered, 1);
	return ep;
fail:
	kref_put(&ep->refs, endpoint_free);
	return ERR_PTR(ret);
}
EXPORT_SYMBOL_GPL(s5xxx_rfs_endpoint_create);

void s5xxx_rfs_endpoint_detach(struct s5xxx_rfs_endpoint *ep)
{
	/* misc_open and misc_deregister serialize new opens with the misc lock. */
	if (atomic_xchg(&ep->registered, 0))
		misc_deregister(&ep->misc);
	mutex_lock(&ep->io_lock);
	s5xxx_rfs_session_shutdown(&ep->session);
	ep->emit = NULL;
	ep->context = NULL;
	mutex_unlock(&ep->io_lock);
}
EXPORT_SYMBOL_GPL(s5xxx_rfs_endpoint_detach);

void s5xxx_rfs_endpoint_put(struct s5xxx_rfs_endpoint *ep)
{
	kref_put(&ep->refs, endpoint_free);
}
EXPORT_SYMBOL_GPL(s5xxx_rfs_endpoint_put);

void s5xxx_rfs_endpoint_reset(struct s5xxx_rfs_endpoint *ep)
{
	s5xxx_rfs_session_reset(&ep->session);
}
EXPORT_SYMBOL_GPL(s5xxx_rfs_endpoint_reset);

int s5xxx_rfs_endpoint_rx(struct s5xxx_rfs_endpoint *ep, struct sk_buff *skb)
{
	return s5xxx_rfs_session_rx(&ep->session, skb);
}
EXPORT_SYMBOL_GPL(s5xxx_rfs_endpoint_rx);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Reference-counted record endpoint for native Exynos RFS");

#if IS_ENABLED(CONFIG_S5XXX_RFS_SESSION_KUNIT_TEST)
#include "s5xxx_rfs_endpoint_test.c"
#endif
