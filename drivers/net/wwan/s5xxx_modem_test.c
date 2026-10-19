// SPDX-License-Identifier: GPL-2.0-only
/* Real driver functions, fake registers; no PCI/CP hardware is touched. */
#include <kunit/test.h>
#include <kunit/device.h>
#include <linux/logic_iomem.h>
#include <linux/vmalloc.h>

#define TEST_IO_SIZE SZ_8M
#define TEST_DOORBELL SZ_4M

struct modem_fixture {
	struct s5xxx_modem sm;
	u8 *memory;
	unsigned int doorbells;
};

static struct modem_fixture *mapping_fixture;
static struct resource modem_test_resource = {
	.name = "kunit-s5xxx-registers",
	.start = 0x100000000ULL,
	.end = 0x100000000ULL + TEST_IO_SIZE - 1,
	.flags = IORESOURCE_MEM,
};

static unsigned long modem_test_read(void *priv, unsigned int offset, int size)
{
	struct modem_fixture *f = priv;
	unsigned long value = 0;

	if (WARN_ON(offset > TEST_IO_SIZE - size))
		return ~0UL;
	memcpy(&value, f->memory + offset, size);
	return value;
}

static void modem_test_write(void *priv, unsigned int offset, int size,
			     unsigned long value)
{
	struct modem_fixture *f = priv;

	if (WARN_ON(offset > TEST_IO_SIZE - size))
		return;
	memcpy(f->memory + offset, &value, size);
	if (offset == TEST_DOORBELL)
		f->doorbells++;
}

static void modem_test_copy_from(void *priv, void *buffer,
				 unsigned int offset, int size)
{
	struct modem_fixture *f = priv;

	if (WARN_ON(offset > TEST_IO_SIZE - size))
		return;
	memcpy(buffer, f->memory + offset, size);
}

static void modem_test_copy_to(void *priv, unsigned int offset,
			       const void *buffer, int size)
{
	struct modem_fixture *f = priv;

	if (WARN_ON(offset > TEST_IO_SIZE - size))
		return;
	memcpy(f->memory + offset, buffer, size);
}

static const struct logic_iomem_ops modem_test_io_ops = {
	.read = modem_test_read,
	.write = modem_test_write,
	.copy_from = modem_test_copy_from,
	.copy_to = modem_test_copy_to,
};

static long modem_test_map(unsigned long offset, size_t size,
			   const struct logic_iomem_ops **ops, void **priv)
{
	if (!mapping_fixture || offset + size > TEST_IO_SIZE)
		return -EINVAL;
	*ops = &modem_test_io_ops;
	*priv = mapping_fixture;
	return offset;
}

static const struct logic_iomem_region_ops modem_test_region_ops = {
	.map = modem_test_map,
};

static int modem_suite_init(struct kunit_suite *suite)
{
	return logic_iomem_add_region(&modem_test_resource, &modem_test_region_ops);
}

static void modem_test_cleanup(void *data)
{
	struct modem_fixture *f = data;

	if (f->sm.rfs_endpoint) {
		s5xxx_rfs_endpoint_detach(f->sm.rfs_endpoint);
		s5xxx_rfs_endpoint_put(f->sm.rfs_endpoint);
	}
	if (f->sm.ipc)
		iounmap(f->sm.ipc);
	vfree(f->memory);
	mapping_fixture = NULL;
}

static int modem_test_init(struct kunit *test)
{
	struct modem_fixture *f;
	struct device *parent;
	int ret;

	f = kunit_kzalloc(test, sizeof(*f), GFP_KERNEL);
	if (!f)
		return -ENOMEM;
	parent = kunit_device_register(test, test->name);
	if (IS_ERR(parent))
		return PTR_ERR(parent);
	ret = kunit_add_action_or_reset(test, modem_test_cleanup, f);
	if (ret)
		return ret;
	f->memory = vzalloc(TEST_IO_SIZE);
	if (!f->memory)
		return -ENOMEM;
	mapping_fixture = f;
	f->sm.ipc = ioremap(modem_test_resource.start, TEST_IO_SIZE);
	if (!f->sm.ipc)
		return -ENOMEM;
	f->sm.doorbell = f->sm.ipc + TEST_DOORBELL;
	f->sm.dev = parent;
	f->sm.link_up = true;
	f->sm.online = true;
	spin_lock_init(&f->sm.tx_lock);
	spin_lock_init(&f->sm.lock);
	f->sm.rfs_endpoint = s5xxx_rfs_endpoint_create(parent, s5xxx_rfs_emit, &f->sm);
	if (IS_ERR(f->sm.rfs_endpoint)) {
		ret = PTR_ERR(f->sm.rfs_endpoint);
		f->sm.rfs_endpoint = NULL;
		return ret;
	}
	test->priv = f;
	return 0;
}

/* The session is separate because endpoint internals remain private. The real
 * callback accepts precisely this session/epoch pair from the real endpoint.
 */
static struct s5xxx_rfs_session *modem_test_owner(struct kunit *test, u64 *epoch)
{
	struct s5xxx_rfs_session *s = kunit_kzalloc(test, sizeof(*s), GFP_KERNEL);

	if (!s)
		return NULL;
	s5xxx_rfs_session_init(s);
	if (s5xxx_rfs_session_open(s, epoch))
		return NULL;
	return s;
}

static void modem_real_tx_framing(struct kunit *test)
{
	struct modem_fixture *f = test->priv;
	struct s5xxx_rfs_session *s;
	u8 frame[8] = {3, 0, 7, 0};
	u8 *packet = f->memory + S5XXX_RAW_TXQ_BUFF;
	u64 epoch;

	s = modem_test_owner(test, &epoch);
	KUNIT_ASSERT_NOT_NULL(test, s);
	KUNIT_ASSERT_EQ(test, s5xxx_rfs_emit(&f->sm, s, epoch, frame, 8), 0);
	KUNIT_EXPECT_EQ(test, readl(f->sm.ipc + S5XXX_RAW_TXQ_HEAD), 24U);
	KUNIT_EXPECT_EQ(test, get_unaligned_le16(packet), (u16)S5XXX_SIT_SYNC);
	KUNIT_EXPECT_EQ(test, get_unaligned_le16(packet + 6), (u16)20);
	KUNIT_EXPECT_EQ(test, packet[8], (u8)S5XXX_CH_RFS);
	KUNIT_EXPECT_MEMEQ(test, packet + 12, frame, 8);
	KUNIT_EXPECT_EQ(test, f->doorbells, 1U);
	KUNIT_EXPECT_EQ(test, readl(f->sm.ipc + S5XXX_IPC_AP2CP_MSG),
			(u32)(S5XXX_INT_VALID | S5XXX_INT_SEND_RAW));
}

static void modem_real_tx_full_is_not_partial(struct kunit *test)
{
	struct modem_fixture *f = test->priv;
	struct s5xxx_rfs_session *s;
	u8 frame[8] = {};
	u64 epoch;

	s = modem_test_owner(test, &epoch);
	KUNIT_ASSERT_NOT_NULL(test, s);
	writel(1, f->sm.ipc + S5XXX_RAW_TXQ_TAIL);
	KUNIT_EXPECT_EQ(test, s5xxx_rfs_emit(&f->sm, s, epoch, frame, 8), -EAGAIN);
	KUNIT_EXPECT_EQ(test, readl(f->sm.ipc + S5XXX_RAW_TXQ_HEAD), 0U);
	KUNIT_EXPECT_EQ(test, f->sm.frame_seq, (u16)0);
	KUNIT_EXPECT_EQ(test, f->sm.rfs_ch_seq, (u8)0);
	KUNIT_EXPECT_EQ(test, f->doorbells, 0U);
	writel(0, f->sm.ipc + S5XXX_RAW_TXQ_TAIL);
	KUNIT_EXPECT_EQ(test, s5xxx_rfs_emit(&f->sm, s, epoch, frame, 8), 0);
	KUNIT_EXPECT_EQ(test, f->doorbells, 1U);
}

static void modem_real_tx_stale_epoch_is_not_published(struct kunit *test)
{
	struct modem_fixture *f = test->priv;
	struct s5xxx_rfs_session *s;
	u8 frame[8] = {};
	u64 epoch;

	s = modem_test_owner(test, &epoch);
	KUNIT_ASSERT_NOT_NULL(test, s);
	s5xxx_rfs_session_reset(s);
	KUNIT_EXPECT_EQ(test, s5xxx_rfs_emit(&f->sm, s, epoch, frame, 8), -EPIPE);
	KUNIT_EXPECT_EQ(test, readl(f->sm.ipc + S5XXX_RAW_TXQ_HEAD), 0U);
	KUNIT_EXPECT_EQ(test, f->doorbells, 0U);
}

static void modem_real_tx_wraps_header_and_payload(struct kunit *test)
{
	struct modem_fixture *f = test->priv;
	struct s5xxx_rfs_session *s;
	u8 frame[8] = {3, 0, 9, 0};
	u8 packet[20];
	u64 epoch;

	s = modem_test_owner(test, &epoch);
	KUNIT_ASSERT_NOT_NULL(test, s);
	writel(S5XXX_RAW_TXQ_SIZE - 8, f->sm.ipc + S5XXX_RAW_TXQ_HEAD);
	writel(128, f->sm.ipc + S5XXX_RAW_TXQ_TAIL);
	KUNIT_ASSERT_EQ(test, s5xxx_rfs_emit(&f->sm, s, epoch, frame, 8), 0);
	s5xxx_circ_read(packet, f->sm.ipc + S5XXX_RAW_TXQ_BUFF,
			S5XXX_RAW_TXQ_SIZE, S5XXX_RAW_TXQ_SIZE - 8, 20);
	KUNIT_EXPECT_EQ(test, get_unaligned_le16(packet), (u16)S5XXX_SIT_SYNC);
	KUNIT_EXPECT_MEMEQ(test, packet + 12, frame, 8);
	KUNIT_EXPECT_EQ(test, readl(f->sm.ipc + S5XXX_RAW_TXQ_HEAD), 16U);
	KUNIT_EXPECT_EQ(test, f->doorbells, 1U);
}

static void modem_real_queue_reset_clears_ring(struct kunit *test)
{
	struct modem_fixture *f = test->priv;

	writel(128, f->sm.ipc + S5XXX_RAW_TXQ_HEAD);
	writel(24, f->sm.ipc + S5XXX_RAW_RXQ_TAIL);
	f->sm.rfs_ch_seq = 23;
	s5xxx_init_ipc_queues(&f->sm);
	KUNIT_EXPECT_EQ(test, readl(f->sm.ipc + S5XXX_RAW_TXQ_HEAD), 0U);
	KUNIT_EXPECT_EQ(test, readl(f->sm.ipc + S5XXX_RAW_RXQ_TAIL), 0U);
	KUNIT_EXPECT_EQ(test, f->sm.rfs_ch_seq, (u8)0);
	KUNIT_EXPECT_EQ(test, readl(f->sm.ipc + S5XXX_IPC_MAGIC), (u32)S5XXX_IPC_MAGIC_VALUE);
}

static struct sk_buff *modem_test_rfs_frame(void)
{
	struct sk_buff *skb = alloc_skb(8, GFP_KERNEL);

	if (skb)
		skb_put_zero(skb, 8);
	return skb;
}

static void modem_test_raw_packet(struct modem_fixture *f, u16 sync,
				  u16 length, u32 declared_payload)
{
	u8 *packet = f->memory + S5XXX_RAW_RXQ_BUFF;

	put_unaligned_le16(sync, packet);
	put_unaligned_le16(length, packet + 6);
	packet[8] = S5XXX_CH_RFS;
	put_unaligned_le32(declared_payload, packet + S5XXX_SIT_HDR + 4);
	writel(ALIGN(max_t(u16, length, S5XXX_SIT_HDR), 8),
	       f->sm.ipc + S5XXX_RAW_RXQ_HEAD);
}

static void modem_real_rx_routes_one_record_without_reply(struct kunit *test)
{
	struct modem_fixture *f = test->priv;
	struct sk_buff *skb;
	int i;

	modem_test_raw_packet(f, S5XXX_SIT_SYNC, 20, 0);
	s5xxx_drain_raw_rxq(&f->sm, 0);
	KUNIT_EXPECT_EQ(test, readl(f->sm.ipc + S5XXX_RAW_RXQ_TAIL), 24U);
	KUNIT_EXPECT_EQ(test, readl(f->sm.ipc + S5XXX_RAW_TXQ_HEAD), 0U);
	KUNIT_EXPECT_EQ(test, f->doorbells, 0U);
	/* Exactly one pre-open record occupied the bounded native queue. */
	for (i = 0; i < S5XXX_RFS_RX_FRAMES - 1; i++) {
		skb = modem_test_rfs_frame();
		KUNIT_ASSERT_NOT_NULL(test, skb);
		KUNIT_ASSERT_EQ(test, s5xxx_rfs_endpoint_rx(f->sm.rfs_endpoint, skb), 0);
	}
	skb = modem_test_rfs_frame();
	KUNIT_ASSERT_NOT_NULL(test, skb);
	KUNIT_EXPECT_EQ(test, s5xxx_rfs_endpoint_rx(f->sm.rfs_endpoint, skb), -ENOBUFS);
}

static void modem_test_expect_revoked(struct kunit *test)
{
	struct modem_fixture *f = test->priv;
	struct sk_buff *skb = modem_test_rfs_frame();

	KUNIT_ASSERT_NOT_NULL(test, skb);
	KUNIT_EXPECT_EQ(test, s5xxx_rfs_endpoint_rx(f->sm.rfs_endpoint, skb), -EIO);
	KUNIT_EXPECT_EQ(test, readl(f->sm.ipc + S5XXX_RAW_TXQ_HEAD), 0U);
	KUNIT_EXPECT_EQ(test, f->doorbells, 0U);
}

static void modem_real_rx_invalid_rfs_revokes(struct kunit *test)
{
	struct modem_fixture *f = test->priv;

	modem_test_raw_packet(f, S5XXX_SIT_SYNC, 20, 1);
	s5xxx_drain_raw_rxq(&f->sm, 0);
	modem_test_expect_revoked(test);
}

static void modem_real_rx_bad_sync_revokes(struct kunit *test)
{
	struct modem_fixture *f = test->priv;

	modem_test_raw_packet(f, 0, 20, 0);
	s5xxx_drain_raw_rxq(&f->sm, 0);
	modem_test_expect_revoked(test);
}

static void modem_real_rx_bad_pointer_revokes(struct kunit *test)
{
	struct modem_fixture *f = test->priv;

	writel(S5XXX_RAW_RXQ_SIZE, f->sm.ipc + S5XXX_RAW_RXQ_HEAD);
	s5xxx_drain_raw_rxq(&f->sm, 0);
	modem_test_expect_revoked(test);
}

static void modem_real_rx_short_link_header_revokes(struct kunit *test)
{
	struct modem_fixture *f = test->priv;

	modem_test_raw_packet(f, S5XXX_SIT_SYNC, 1, 0);
	s5xxx_drain_raw_rxq(&f->sm, 0);
	modem_test_expect_revoked(test);
}

static void modem_real_reset_cancels_deferred_doorbell(struct kunit *test)
{
	struct modem_fixture *f = test->priv;

	f->sm.db_reserved = true;
	writel(S5XXX_INT_VALID | S5XXX_INT_SEND_RAW,
	       f->sm.ipc + S5XXX_IPC_AP2CP_MSG);
	s5xxx_rfs_revoke(&f->sm);
	KUNIT_EXPECT_FALSE(test, f->sm.online);
	KUNIT_EXPECT_FALSE(test, f->sm.db_reserved);
	KUNIT_EXPECT_EQ(test, readl(f->sm.ipc + S5XXX_IPC_AP2CP_MSG), 0U);
	KUNIT_EXPECT_EQ(test, f->doorbells, 0U);
}

static void modem_real_queue_reset_cancels_deferred_doorbell(struct kunit *test)
{
	struct modem_fixture *f = test->priv;

	f->sm.db_reserved = true;
	writel(S5XXX_INT_VALID | S5XXX_INT_SEND_RAW,
	       f->sm.ipc + S5XXX_IPC_AP2CP_MSG);
	s5xxx_init_ipc_queues(&f->sm);
	KUNIT_EXPECT_FALSE(test, f->sm.db_reserved);
	KUNIT_EXPECT_EQ(test, readl(f->sm.ipc + S5XXX_IPC_AP2CP_MSG), 0U);
	KUNIT_EXPECT_EQ(test, f->doorbells, 0U);
}

static void modem_real_tx_preserves_factory_open_extension(struct kunit *test)
{
	struct modem_fixture *f = test->priv;
	struct s5xxx_rfs_session *session;
	const u8 golden[] = { 3, 0, 0x34, 0x12, 8, 0, 0, 0,
			      0, 0, 0, 0, 40, 0, 0, 0, 1, 8, 0, 0 };
	u8 *packet = f->memory + S5XXX_RAW_TXQ_BUFF;
	u64 epoch;

	session = modem_test_owner(test, &epoch);
	KUNIT_ASSERT_NOT_NULL(test, session);
	KUNIT_ASSERT_EQ(test, s5xxx_rfs_emit(&f->sm, session, epoch,
					  golden, sizeof(golden)), 0);
	KUNIT_EXPECT_EQ(test, readl(f->sm.ipc + S5XXX_RAW_TXQ_HEAD), 32U);
	KUNIT_EXPECT_EQ(test, get_unaligned_le16(packet + 6), (u16)32);
	KUNIT_EXPECT_EQ(test, packet[8], (u8)S5XXX_CH_RFS);
	KUNIT_EXPECT_MEMEQ(test, packet + 12, golden, sizeof(golden));
	KUNIT_EXPECT_EQ(test, f->doorbells, 1U);
}

static struct kunit_case modem_cases[] = {
	KUNIT_CASE(modem_real_tx_preserves_factory_open_extension),
	KUNIT_CASE(modem_real_reset_cancels_deferred_doorbell),
	KUNIT_CASE(modem_real_queue_reset_cancels_deferred_doorbell),
	KUNIT_CASE(modem_real_rx_routes_one_record_without_reply),
	KUNIT_CASE(modem_real_rx_invalid_rfs_revokes),
	KUNIT_CASE(modem_real_rx_bad_sync_revokes),
	KUNIT_CASE(modem_real_rx_bad_pointer_revokes),
	KUNIT_CASE(modem_real_rx_short_link_header_revokes),
	KUNIT_CASE(modem_real_tx_framing),
	KUNIT_CASE(modem_real_tx_full_is_not_partial),
	KUNIT_CASE(modem_real_tx_stale_epoch_is_not_published),
	KUNIT_CASE(modem_real_tx_wraps_header_and_payload),
	KUNIT_CASE(modem_real_queue_reset_clears_ring),
	{}
};

static struct kunit_suite modem_suite = {
	.name = "s5xxx-modem-registers",
	.suite_init = modem_suite_init,
	.init = modem_test_init,
	.test_cases = modem_cases,
};
kunit_test_suite(modem_suite);
