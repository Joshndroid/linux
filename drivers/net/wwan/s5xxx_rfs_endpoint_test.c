// SPDX-License-Identifier: GPL-2.0-only
#include <kunit/test.h>
#include <kunit/device.h>
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/kthread.h>
#include <linux/unaligned.h>

struct endpoint_fixture {
	struct s5xxx_rfs_endpoint *ep;
	struct file files[2];
	bool opened[2];
	int emitted, result, retries;
};

static int test_emit(void *data, struct s5xxx_rfs_session *s, u64 epoch,
		     const void *frame, size_t length)
{
	struct endpoint_fixture *f = data;
	unsigned long flags;
	int ret = s5xxx_rfs_session_tx_begin(s, epoch, &flags);

	if (ret)
		return ret;
	if (f->retries > 0) {
		f->retries--;
		s5xxx_rfs_session_tx_end(s, flags);
		return -EAGAIN;
	}
	if (!f->result)
		f->emitted++;
	s5xxx_rfs_session_tx_end(s, flags);
	return f->result;
}

static void endpoint_cleanup(void *data)
{
	struct endpoint_fixture *f = data;
	int i;

	for (i = 0; i < 2; i++)
		if (f->opened[i])
			endpoint_release(NULL, &f->files[i]);
	if (f->ep) {
		s5xxx_rfs_endpoint_detach(f->ep);
		s5xxx_rfs_endpoint_put(f->ep);
	}
}

static int endpoint_test_init(struct kunit *test)
{
	struct endpoint_fixture *f;
	struct device *parent;

	f = kunit_kzalloc(test, sizeof(*f), GFP_KERNEL);
	if (!f)
		return -ENOMEM;
	parent = kunit_device_register(test, test->name);
	if (IS_ERR(parent))
		return PTR_ERR(parent);
	f->ep = s5xxx_rfs_endpoint_create(parent, test_emit, f);
	if (IS_ERR(f->ep))
		return PTR_ERR(f->ep);
	test->priv = f;
	return kunit_add_action_or_reset(test, endpoint_cleanup, f);
}

static int test_open(struct endpoint_fixture *f, int i)
{
	int ret;

	f->files[i].private_data = &f->ep->misc;
	f->files[i].f_flags = O_NONBLOCK;
	ret = endpoint_open(NULL, &f->files[i]);
	if (!ret)
		f->opened[i] = true;
	return ret;
}

static ssize_t test_io(struct file *file, void *data, size_t size, bool write)
{
	struct kiocb iocb = { .ki_filp = file };
	struct kvec vector = { .iov_base = data, .iov_len = size };
	struct iov_iter iter;

	iov_iter_kvec(&iter, write ? ITER_SOURCE : ITER_DEST, &vector, 1, size);
	return write ? endpoint_write(&iocb, &iter) : endpoint_read(&iocb, &iter);
}

static int test_receive(struct endpoint_fixture *f)
{
	struct sk_buff *skb = alloc_skb(8, GFP_KERNEL);

	if (!skb)
		return -ENOMEM;
	skb_put_zero(skb, 8);
	return s5xxx_rfs_endpoint_rx(f->ep, skb);
}

static void endpoint_exclusive(struct kunit *test)
{
	struct endpoint_fixture *f = test->priv;

	KUNIT_EXPECT_EQ(test, f->ep->misc.mode, (umode_t)0600);
	KUNIT_ASSERT_EQ(test, test_open(f, 0), 0);
	KUNIT_EXPECT_EQ(test, test_open(f, 1), -EBUSY);
}

static void endpoint_record_reads(struct kunit *test)
{
	struct endpoint_fixture *f = test->priv;
	u8 bytes[8];

	KUNIT_ASSERT_EQ(test, test_open(f, 0), 0);
	KUNIT_EXPECT_EQ(test, test_io(&f->files[0], bytes, 8, false), (ssize_t)-EAGAIN);
	KUNIT_ASSERT_EQ(test, test_receive(f), 0);
	KUNIT_EXPECT_TRUE(test, endpoint_poll(&f->files[0], NULL) & EPOLLIN);
	KUNIT_EXPECT_EQ(test, test_io(&f->files[0], bytes, 7, false), (ssize_t)-EMSGSIZE);
	KUNIT_EXPECT_EQ(test, test_io(&f->files[0], bytes, 8, false), (ssize_t)8);
	KUNIT_EXPECT_EQ(test, test_io(&f->files[0], bytes, 8, false), (ssize_t)-EAGAIN);
}

static void endpoint_record_writes(struct kunit *test)
{
	struct endpoint_fixture *f = test->priv;
	u8 bytes[8] = {};

	KUNIT_ASSERT_EQ(test, test_open(f, 0), 0);
	KUNIT_EXPECT_EQ(test, test_io(&f->files[0], bytes, 8, true), (ssize_t)8);
	KUNIT_EXPECT_EQ(test, f->emitted, 1);
	put_unaligned_le32(1, bytes + 4);
	KUNIT_EXPECT_EQ(test, test_io(&f->files[0], bytes, 8, true), (ssize_t)-EPROTO);
	KUNIT_EXPECT_EQ(test, test_io(&f->files[0], bytes, 7, true), (ssize_t)-EMSGSIZE);
	KUNIT_EXPECT_EQ(test, f->emitted, 1);
}

static void endpoint_callback_failures(struct kunit *test)
{
	struct endpoint_fixture *f = test->priv;
	u8 bytes[8] = {};

	KUNIT_ASSERT_EQ(test, test_open(f, 0), 0);
	f->result = -EAGAIN;
	KUNIT_EXPECT_EQ(test, test_io(&f->files[0], bytes, 8, true), (ssize_t)-EAGAIN);
	f->result = 1;
	KUNIT_EXPECT_EQ(test, test_io(&f->files[0], bytes, 8, true), (ssize_t)-EIO);
	KUNIT_EXPECT_EQ(test, test_io(&f->files[0], bytes, 8, true), (ssize_t)-EPIPE);
	KUNIT_EXPECT_EQ(test, f->emitted, 0);
}

static void endpoint_reset_reopen(struct kunit *test)
{
	struct endpoint_fixture *f = test->priv;
	u8 bytes[8] = {};

	KUNIT_ASSERT_EQ(test, test_open(f, 0), 0);
	s5xxx_rfs_endpoint_reset(f->ep);
	KUNIT_ASSERT_EQ(test, test_receive(f), 0);
	KUNIT_EXPECT_EQ(test, test_io(&f->files[0], bytes, 8, false), (ssize_t)-EPIPE);
	KUNIT_EXPECT_EQ(test, test_io(&f->files[0], bytes, 8, true), (ssize_t)-EPIPE);
	endpoint_release(NULL, &f->files[0]);
	f->opened[0] = false;
	KUNIT_ASSERT_EQ(test, test_open(f, 1), 0);
	KUNIT_EXPECT_EQ(test, test_io(&f->files[1], bytes, 8, false), (ssize_t)8);
}

static void endpoint_detach_open_file(struct kunit *test)
{
	struct endpoint_fixture *f = test->priv;
	u8 bytes[8] = {};

	KUNIT_ASSERT_EQ(test, test_open(f, 0), 0);
	s5xxx_rfs_endpoint_detach(f->ep);
	KUNIT_EXPECT_EQ(test, test_open(f, 1), -ENODEV);
	s5xxx_rfs_endpoint_put(f->ep);
	f->ep = NULL;
	KUNIT_EXPECT_TRUE(test, endpoint_poll(&f->files[0], NULL) & EPOLLHUP);
	KUNIT_EXPECT_EQ(test, test_io(&f->files[0], bytes, 8, false), (ssize_t)-ENODEV);
	KUNIT_EXPECT_EQ(test, test_io(&f->files[0], bytes, 8, true), (ssize_t)-ENODEV);
}

static void endpoint_copy_faults(struct kunit *test)
{
	struct endpoint_fixture *f = test->priv;
	struct kiocb iocb = { .ki_filp = &f->files[0] };
	struct iov_iter iter;
	u8 bytes[8] = {};

	KUNIT_ASSERT_EQ(test, test_open(f, 0), 0);
	/* Out-of-range uaccess is rejected without needing a user mm. */
	iov_iter_ubuf(&iter, ITER_SOURCE, (void __user *)~0UL, 8);
	KUNIT_EXPECT_EQ(test, endpoint_write(&iocb, &iter), (ssize_t)-EFAULT);
	KUNIT_EXPECT_EQ(test, f->emitted, 0);
	KUNIT_EXPECT_EQ(test, test_io(&f->files[0], bytes, 8, true), (ssize_t)8);
	KUNIT_ASSERT_EQ(test, test_receive(f), 0);
	iov_iter_ubuf(&iter, ITER_DEST, (void __user *)~0UL, 8);
	KUNIT_EXPECT_EQ(test, endpoint_read(&iocb, &iter), (ssize_t)-EFAULT);
	KUNIT_EXPECT_EQ(test, test_io(&f->files[0], bytes, 8, true), (ssize_t)-EPIPE);
}

struct endpoint_reader {
	struct endpoint_fixture *fixture;
	struct task_struct *task;
	struct completion done;
	ssize_t result;
	bool write;
};

static int endpoint_reader_thread(void *data)
{
	struct endpoint_reader *reader = data;
	u8 bytes[8] = {};

	reader->result = test_io(&reader->fixture->files[0], bytes, 8, reader->write);
	complete(&reader->done);
	while (!kthread_should_stop()) {
		set_current_state(TASK_INTERRUPTIBLE);
		if (!kthread_should_stop())
			schedule();
	}
	__set_current_state(TASK_RUNNING);
	return 0;
}

static void endpoint_reader_cleanup(void *data)
{
	struct endpoint_reader *reader = data;

	s5xxx_rfs_endpoint_detach(reader->fixture->ep);
	kthread_stop(reader->task);
}

static void endpoint_detach_wakes_reader(struct kunit *test)
{
	struct endpoint_fixture *f = test->priv;
	struct endpoint_reader *reader;
	int i;

	KUNIT_ASSERT_EQ(test, test_open(f, 0), 0);
	f->files[0].f_flags &= ~O_NONBLOCK;
	reader = kunit_kzalloc(test, sizeof(*reader), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, reader);
	reader->fixture = f;
	init_completion(&reader->done);
	reader->task = kthread_run(endpoint_reader_thread, reader, "rfs-read-test");
	KUNIT_ASSERT_FALSE(test, IS_ERR(reader->task));
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, endpoint_reader_cleanup, reader), 0);
	for (i = 0; i < 100 && !waitqueue_active(&f->ep->session.wait); i++)
		usleep_range(1000, 2000);
	KUNIT_ASSERT_TRUE(test, waitqueue_active(&f->ep->session.wait));
	s5xxx_rfs_endpoint_detach(f->ep);
	KUNIT_ASSERT_NE(test, wait_for_completion_timeout(&reader->done, HZ), 0UL);
	KUNIT_EXPECT_EQ(test, reader->result, (ssize_t)-ENODEV);
}

static void endpoint_detach_wakes_backpressured_writer(struct kunit *test)
{
	struct endpoint_fixture *f = test->priv;
	struct endpoint_reader *reader;
	int i;

	KUNIT_ASSERT_EQ(test, test_open(f, 0), 0);
	f->files[0].f_flags &= ~O_NONBLOCK;
	reader = kunit_kzalloc(test, sizeof(*reader), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, reader);
	reader->fixture = f;
	reader->write = true;
	f->result = -EAGAIN;
	init_completion(&reader->done);
	reader->task = kthread_run(endpoint_reader_thread, reader, "rfs-write-test");
	KUNIT_ASSERT_FALSE(test, IS_ERR(reader->task));
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, endpoint_reader_cleanup, reader), 0);
	for (i = 0; i < 100 && !waitqueue_active(&f->ep->session.wait); i++)
		usleep_range(1000, 2000);
	KUNIT_ASSERT_TRUE(test, waitqueue_active(&f->ep->session.wait));
	s5xxx_rfs_endpoint_detach(f->ep);
	KUNIT_ASSERT_NE(test, wait_for_completion_timeout(&reader->done, HZ), 0UL);
	KUNIT_EXPECT_EQ(test, reader->result, (ssize_t)-ENODEV);
}

static void endpoint_blocking_write_retries_without_duplicate_send(struct kunit *test)
{
	struct endpoint_fixture *f = test->priv;
	u8 bytes[8] = {};

	KUNIT_ASSERT_EQ(test, test_open(f, 0), 0);
	f->files[0].f_flags &= ~O_NONBLOCK;
	f->retries = 2;
	KUNIT_EXPECT_EQ(test, test_io(&f->files[0], bytes, 8, true), (ssize_t)8);
	KUNIT_EXPECT_EQ(test, f->retries, 0);
	KUNIT_EXPECT_EQ(test, f->emitted, 1);
}

/* Exact CP41 factory successful OPEN: declared body 8 plus size extension. */
static void endpoint_factory_open_status_extension(struct kunit *test)
{
	struct endpoint_fixture *f = test->priv;
	u8 golden[21] = { 3, 0, 0x34, 0x12, 8, 0, 0, 0,
			  0, 0, 0, 0, 40, 0, 0, 0, 1, 8, 0, 0 };

	KUNIT_ASSERT_EQ(test, test_open(f, 0), 0);
	KUNIT_EXPECT_EQ(test, test_io(&f->files[0], golden, 20, true), (ssize_t)20);
	KUNIT_EXPECT_EQ(test, f->emitted, 1);
	golden[0] = 1;
	KUNIT_EXPECT_EQ(test, test_io(&f->files[0], golden, 20, true), (ssize_t)-EPROTO);
	golden[0] = 3;
	golden[8] = 1;
	KUNIT_EXPECT_EQ(test, test_io(&f->files[0], golden, 20, true), (ssize_t)-EPROTO);
	golden[8] = 0;
	KUNIT_EXPECT_EQ(test, test_io(&f->files[0], golden, 19, true), (ssize_t)-EPROTO);
	KUNIT_EXPECT_EQ(test, test_io(&f->files[0], golden, 21, true), (ssize_t)-EPROTO);
	put_unaligned_le32(U32_MAX, golden + 4);
	KUNIT_EXPECT_EQ(test, test_io(&f->files[0], golden, 20, true), (ssize_t)-EPROTO);
	KUNIT_EXPECT_EQ(test, f->emitted, 1);
}

static struct kunit_case endpoint_cases[] = {
	KUNIT_CASE(endpoint_factory_open_status_extension),
	KUNIT_CASE(endpoint_detach_wakes_backpressured_writer),
	KUNIT_CASE(endpoint_blocking_write_retries_without_duplicate_send),
	KUNIT_CASE(endpoint_detach_wakes_reader),
	KUNIT_CASE(endpoint_copy_faults),
	KUNIT_CASE(endpoint_exclusive),
	KUNIT_CASE(endpoint_record_reads),
	KUNIT_CASE(endpoint_record_writes),
	KUNIT_CASE(endpoint_callback_failures),
	KUNIT_CASE(endpoint_reset_reopen),
	KUNIT_CASE(endpoint_detach_open_file),
	{}
};

static struct kunit_suite endpoint_suite = {
	.name = "s5xxx-rfs-endpoint",
	.init = endpoint_test_init,
	.test_cases = endpoint_cases,
};
kunit_test_suite(endpoint_suite);
