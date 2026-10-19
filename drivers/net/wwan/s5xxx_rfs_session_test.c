// SPDX-License-Identifier: GPL-2.0-only
#include <kunit/test.h>
#include <linux/completion.h>
#include <linux/err.h>
#include <linux/kthread.h>
#include <linux/slab.h>
#include <linux/unaligned.h>
#include "s5xxx_rfs_session.h"

static void cleanup(void *data)
{
	s5xxx_rfs_session_shutdown(data);
}

static struct s5xxx_rfs_session *session(struct kunit *test)
{
	struct s5xxx_rfs_session *s;

	s = kunit_kzalloc(test, sizeof(*s), GFP_KERNEL);
	if (!s)
		return NULL;
	s5xxx_rfs_session_init(s);
	if (kunit_add_action_or_reset(test, cleanup, s))
		return NULL;
	return s;
}

static struct sk_buff *frame(u16 token)
{
	struct sk_buff *skb = alloc_skb(8, GFP_KERNEL);
	u8 *p;

	if (!skb)
		return NULL;
	p = skb_put_zero(skb, 8);
	put_unaligned_le16(3, p);
	put_unaligned_le16(token, p + 2);
	return skb;
}

static void exclusive_owner(struct kunit *test)
{
	struct s5xxx_rfs_session *s = session(test);
	u64 first, second;

	KUNIT_ASSERT_NOT_NULL(test, s);
	KUNIT_ASSERT_EQ(test, s5xxx_rfs_session_open(s, &first), 0);
	KUNIT_EXPECT_EQ(test, s5xxx_rfs_session_open(s, &second), -EBUSY);
	s5xxx_rfs_session_close(s, first);
	KUNIT_ASSERT_EQ(test, s5xxx_rfs_session_open(s, &second), 0);
	KUNIT_EXPECT_NE(test, first, second);
}

static void preopen_fifo(struct kunit *test)
{
	struct s5xxx_rfs_session *s = session(test);
	struct sk_buff *skb;
	u64 epoch;
	int i;

	KUNIT_ASSERT_NOT_NULL(test, s);
	for (i = 0; i < 8; i++)
		KUNIT_ASSERT_EQ(test, s5xxx_rfs_session_rx(s, frame(i)), 0);
	KUNIT_ASSERT_EQ(test, s5xxx_rfs_session_open(s, &epoch), 0);
	for (i = 0; i < 8; i++) {
		skb = s5xxx_rfs_session_take(s, epoch, 4096);
		KUNIT_ASSERT_NOT_ERR_OR_NULL(test, skb);
		KUNIT_EXPECT_EQ(test, get_unaligned_le16(skb->data + 2), i);
		kfree_skb(skb);
	}
	KUNIT_EXPECT_PTR_EQ(test, s5xxx_rfs_session_take(s, epoch, 4096),
			    ERR_PTR(-EAGAIN));
}

static void undersized_read_retains_record(struct kunit *test)
{
	struct s5xxx_rfs_session *s = session(test);
	struct sk_buff *skb;
	u64 epoch;

	KUNIT_ASSERT_NOT_NULL(test, s);
	KUNIT_ASSERT_EQ(test, s5xxx_rfs_session_open(s, &epoch), 0);
	KUNIT_ASSERT_EQ(test, s5xxx_rfs_session_rx(s, frame(4)), 0);
	KUNIT_EXPECT_PTR_EQ(test, s5xxx_rfs_session_take(s, epoch, 7),
			    ERR_PTR(-EMSGSIZE));
	skb = s5xxx_rfs_session_take(s, epoch, 8);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, skb);
	KUNIT_EXPECT_EQ(test, get_unaligned_le16(skb->data + 2), 4);
	kfree_skb(skb);
}

static void reset_fences_old_fd_and_preserves_new_requests(struct kunit *test)
{
	struct s5xxx_rfs_session *s = session(test);
	struct sk_buff *skb;
	unsigned long flags;
	u64 old, next;

	KUNIT_ASSERT_NOT_NULL(test, s);
	KUNIT_ASSERT_EQ(test, s5xxx_rfs_session_open(s, &old), 0);
	KUNIT_ASSERT_EQ(test, s5xxx_rfs_session_rx(s, frame(1)), 0);
	s5xxx_rfs_session_reset(s);
	KUNIT_ASSERT_EQ(test, s5xxx_rfs_session_rx(s, frame(2)), 0);
	KUNIT_EXPECT_PTR_EQ(test, s5xxx_rfs_session_take(s, old, 4096),
			    ERR_PTR(-EPIPE));
	KUNIT_EXPECT_EQ(test, s5xxx_rfs_session_tx_begin(s, old, &flags), -EPIPE);
	KUNIT_EXPECT_EQ(test, s5xxx_rfs_session_open(s, &next), -EBUSY);
	s5xxx_rfs_session_close(s, old);
	KUNIT_ASSERT_EQ(test, s5xxx_rfs_session_open(s, &next), 0);
	skb = s5xxx_rfs_session_take(s, next, 4096);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, skb);
	KUNIT_EXPECT_EQ(test, get_unaligned_le16(skb->data + 2), 2);
	kfree_skb(skb);
}

static void stale_close_cannot_release_new_owner(struct kunit *test)
{
	struct s5xxx_rfs_session *s = session(test);
	u64 old, next, competing;

	KUNIT_ASSERT_NOT_NULL(test, s);
	KUNIT_ASSERT_EQ(test, s5xxx_rfs_session_open(s, &old), 0);
	s5xxx_rfs_session_close(s, old);
	KUNIT_ASSERT_EQ(test, s5xxx_rfs_session_open(s, &next), 0);
	s5xxx_rfs_session_close(s, old);
	KUNIT_EXPECT_EQ(test, s5xxx_rfs_session_open(s, &competing), -EBUSY);
}

static void close_discards_unfinished_session_records(struct kunit *test)
{
	struct s5xxx_rfs_session *s = session(test);
	u64 old, next;

	KUNIT_ASSERT_NOT_NULL(test, s);
	KUNIT_ASSERT_EQ(test, s5xxx_rfs_session_open(s, &old), 0);
	KUNIT_ASSERT_EQ(test, s5xxx_rfs_session_rx(s, frame(1)), 0);
	s5xxx_rfs_session_close(s, old);
	KUNIT_ASSERT_EQ(test, s5xxx_rfs_session_open(s, &next), 0);
	KUNIT_EXPECT_PTR_EQ(test, s5xxx_rfs_session_take(s, next, 4096),
			    ERR_PTR(-EAGAIN));
}

static void overflow_requires_explicit_reset(struct kunit *test)
{
	struct s5xxx_rfs_session *s = session(test);
	u64 epoch;
	int i;

	KUNIT_ASSERT_NOT_NULL(test, s);
	for (i = 0; i < S5XXX_RFS_RX_FRAMES; i++)
		KUNIT_ASSERT_EQ(test, s5xxx_rfs_session_rx(s, frame(i)), 0);
	KUNIT_ASSERT_EQ(test, s5xxx_rfs_session_rx(s, frame(99)), -ENOBUFS);
	KUNIT_EXPECT_EQ(test, skb_queue_len(&s->rxq), 0);
	KUNIT_EXPECT_EQ(test, s5xxx_rfs_session_open(s, &epoch), -EIO);
	s5xxx_rfs_session_reset(s);
	KUNIT_ASSERT_EQ(test, s5xxx_rfs_session_open(s, &epoch), 0);
	KUNIT_EXPECT_PTR_EQ(test, s5xxx_rfs_session_take(s, epoch, 4096),
			    ERR_PTR(-EAGAIN));
}

static void rx_allocation_failure_revokes_session(struct kunit *test)
{
	struct s5xxx_rfs_session *s = session(test);
	u64 epoch;

	KUNIT_ASSERT_NOT_NULL(test, s);
	KUNIT_ASSERT_EQ(test, s5xxx_rfs_session_open(s, &epoch), 0);
	KUNIT_ASSERT_EQ(test, s5xxx_rfs_session_rx(s, NULL), -ENOMEM);
	KUNIT_EXPECT_PTR_EQ(test, s5xxx_rfs_session_take(s, epoch, 4096),
			    ERR_PTR(-EPIPE));
	s5xxx_rfs_session_close(s, epoch);
	KUNIT_EXPECT_EQ(test, s5xxx_rfs_session_open(s, &epoch), -EIO);
}

static void malformed_record_revokes_session(struct kunit *test)
{
	struct s5xxx_rfs_session *s = session(test);
	struct sk_buff *skb = frame(1);
	u64 epoch;

	KUNIT_ASSERT_NOT_NULL(test, s);
	KUNIT_ASSERT_NOT_NULL(test, skb);
	KUNIT_ASSERT_EQ(test, s5xxx_rfs_session_open(s, &epoch), 0);
	put_unaligned_le32(1, skb->data + 4);
	KUNIT_EXPECT_EQ(test, s5xxx_rfs_session_rx(s, skb), -EPROTO);
	KUNIT_EXPECT_PTR_EQ(test, s5xxx_rfs_session_take(s, epoch, 4096),
			    ERR_PTR(-EPIPE));
}

static void frame_bounds_are_checked_before_header_access(struct kunit *test)
{
	u8 *data = kunit_kzalloc(test, 4097, GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, data);
	KUNIT_EXPECT_FALSE(test, s5xxx_rfs_frame_valid(NULL, 8));
	KUNIT_EXPECT_FALSE(test, s5xxx_rfs_frame_valid(data, 7));
	KUNIT_EXPECT_TRUE(test, s5xxx_rfs_frame_valid(data, 8));
	put_unaligned_le32(4088, data + 4);
	KUNIT_EXPECT_TRUE(test, s5xxx_rfs_frame_valid(data, 4096));
	KUNIT_EXPECT_FALSE(test, s5xxx_rfs_frame_valid(data, 4097));
	put_unaligned_le32(U32_MAX, data + 4);
	KUNIT_EXPECT_FALSE(test, s5xxx_rfs_frame_valid(data, 4096));
}

static void shutdown_is_permanent_and_wakes_pollers(struct kunit *test)
{
	struct s5xxx_rfs_session *s = session(test);
	u64 epoch, next;
	unsigned long flags;

	KUNIT_ASSERT_NOT_NULL(test, s);
	KUNIT_ASSERT_EQ(test, s5xxx_rfs_session_open(s, &epoch), 0);
	KUNIT_ASSERT_EQ(test, s5xxx_rfs_session_rx(s, frame(1)), 0);
	s5xxx_rfs_session_shutdown(s);
	KUNIT_EXPECT_PTR_EQ(test, s5xxx_rfs_session_take(s, epoch, 4096),
			    ERR_PTR(-ENODEV));
	KUNIT_EXPECT_EQ(test, s5xxx_rfs_session_tx_begin(s, epoch, &flags), -ENODEV);
	KUNIT_EXPECT_TRUE(test, !!(s5xxx_rfs_session_poll(s, epoch) & EPOLLHUP));
	KUNIT_EXPECT_EQ(test, s5xxx_rfs_session_rx(s, frame(2)), -ENODEV);
	s5xxx_rfs_session_reset(s);
	s5xxx_rfs_session_close(s, epoch);
	KUNIT_EXPECT_EQ(test, s5xxx_rfs_session_open(s, &next), -ENODEV);
}

static void epoch_exhaustion_fails_closed(struct kunit *test)
{
	struct s5xxx_rfs_session *s = session(test);
	unsigned long flags;
	u64 epoch, next;

	KUNIT_ASSERT_NOT_NULL(test, s);
	spin_lock_irqsave(&s->rxq.lock, flags);
	s->epoch = U64_MAX;
	spin_unlock_irqrestore(&s->rxq.lock, flags);
	KUNIT_ASSERT_EQ(test, s5xxx_rfs_session_open(s, &epoch), 0);
	s5xxx_rfs_session_reset(s);
	s5xxx_rfs_session_close(s, epoch);
	KUNIT_EXPECT_EQ(test, s5xxx_rfs_session_open(s, &next), -ENODEV);
}

static void tx_gate_accepts_only_the_live_owner(struct kunit *test)
{
	struct s5xxx_rfs_session *s = session(test);
	unsigned long flags;
	u64 epoch;
	int ret;

	KUNIT_ASSERT_NOT_NULL(test, s);
	KUNIT_ASSERT_EQ(test, s5xxx_rfs_session_open(s, &epoch), 0);
	ret = s5xxx_rfs_session_tx_begin(s, epoch, &flags);
	if (!ret)
		s5xxx_rfs_session_tx_end(s, flags);
	KUNIT_EXPECT_EQ(test, ret, 0);
	s5xxx_rfs_session_close(s, epoch);
	KUNIT_EXPECT_EQ(test, s5xxx_rfs_session_tx_begin(s, epoch, &flags), -EPIPE);
}

struct waiter {
	struct s5xxx_rfs_session *s;
	u64 epoch;
	struct completion ready;
	struct completion woke;
	__poll_t mask;
};

static int wait_task(void *data)
{
	struct waiter *w = data;
	DEFINE_WAIT(wait);
	bool announced = false;

	for (;;) {
		int ret = prepare_to_wait_event(&w->s->wait, &wait,
					       TASK_INTERRUPTIBLE);

		if (!announced) {
			announced = true;
			complete(&w->ready);
		}
		if (ret || kthread_should_stop() ||
		    (s5xxx_rfs_session_poll(w->s, w->epoch) &
		     (EPOLLIN | EPOLLERR | EPOLLHUP)))
			break;
		schedule();
	}
	finish_wait(&w->s->wait, &wait);
	w->mask = s5xxx_rfs_session_poll(w->s, w->epoch);
	complete(&w->woke);
	/* Keep the task valid until the test joins it with kthread_stop(). */
	set_current_state(TASK_INTERRUPTIBLE);
	while (!kthread_should_stop()) {
		schedule();
		set_current_state(TASK_INTERRUPTIBLE);
	}
	__set_current_state(TASK_RUNNING);
	return 0;
}

static void reset_wakes_a_blocked_reader(struct kunit *test)
{
	struct s5xxx_rfs_session *s = session(test);
	struct task_struct *task;
	struct waiter w = { .s = s };
	unsigned long woke;

	KUNIT_ASSERT_NOT_NULL(test, s);
	KUNIT_ASSERT_EQ(test, s5xxx_rfs_session_open(s, &w.epoch), 0);
	init_completion(&w.ready);
	init_completion(&w.woke);
	task = kthread_run(wait_task, &w, "rfs-kunit-wait");
	KUNIT_ASSERT_FALSE(test, IS_ERR(task));
	wait_for_completion(&w.ready);
	s5xxx_rfs_session_reset(s);
	woke = wait_for_completion_timeout(&w.woke, HZ);
	kthread_stop(task);
	KUNIT_EXPECT_NE(test, woke, 0);
	KUNIT_EXPECT_TRUE(test, !!(w.mask & EPOLLERR));
}

static void factory_open_extension_is_not_accepted_as_incoming(struct kunit *test)
{
	struct s5xxx_rfs_session *s = session(test);
	const u8 golden[] = { 3, 0, 0x34, 0x12, 8, 0, 0, 0,
			      0, 0, 0, 0, 40, 0, 0, 0, 1, 8, 0, 0 };
	struct sk_buff *skb = alloc_skb(sizeof(golden), GFP_KERNEL);
	u64 epoch;

	KUNIT_ASSERT_NOT_NULL(test, s);
	KUNIT_ASSERT_NOT_NULL(test, skb);
	KUNIT_ASSERT_EQ(test, s5xxx_rfs_session_open(s, &epoch), 0);
	skb_put_data(skb, golden, sizeof(golden));
	KUNIT_EXPECT_FALSE(test, s5xxx_rfs_frame_valid(golden, sizeof(golden)));
	KUNIT_EXPECT_EQ(test, s5xxx_rfs_session_rx(s, skb), -EPROTO);
	KUNIT_EXPECT_PTR_EQ(test, s5xxx_rfs_session_take(s, epoch, 4096),
			    ERR_PTR(-EPIPE));
}

static struct kunit_case rfs_cases[] = {
	KUNIT_CASE(factory_open_extension_is_not_accepted_as_incoming),
	KUNIT_CASE(exclusive_owner),
	KUNIT_CASE(preopen_fifo),
	KUNIT_CASE(undersized_read_retains_record),
	KUNIT_CASE(reset_fences_old_fd_and_preserves_new_requests),
	KUNIT_CASE(stale_close_cannot_release_new_owner),
	KUNIT_CASE(close_discards_unfinished_session_records),
	KUNIT_CASE(overflow_requires_explicit_reset),
	KUNIT_CASE(rx_allocation_failure_revokes_session),
	KUNIT_CASE(malformed_record_revokes_session),
	KUNIT_CASE(frame_bounds_are_checked_before_header_access),
	KUNIT_CASE(shutdown_is_permanent_and_wakes_pollers),
	KUNIT_CASE(epoch_exhaustion_fails_closed),
	KUNIT_CASE(tx_gate_accepts_only_the_live_owner),
	KUNIT_CASE(reset_wakes_a_blocked_reader),
	{}
};

static struct kunit_suite rfs_suite = {
	.name = "s5xxx-rfs-session",
	.test_cases = rfs_cases,
};
kunit_test_suite(rfs_suite);
MODULE_LICENSE("GPL");
