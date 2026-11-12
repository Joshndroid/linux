// SPDX-License-Identifier: GPL-2.0-only
/*
 * Raise CPU frequency floors while a touchscreen is in use.
 *
 * schedutil sizes the frequency from utilisation, which lags for the short
 * per-frame bursts of an application drawing a scroll on a small core. While
 * a direct touch device reports events, and for hold_ms after the last one
 * (a fling keeps scrolling after the finger lifts), each cpufreq policy
 * whose first CPU has a non-zero entry in freqs= gets that minimum
 * frequency through a freq QoS request.
 *
 * Off by default: hold_ms=0 or no freqs leaves every policy untouched.
 *   echo 1704000 > /sys/module/cpufreq_touch_boost/parameters/freqs
 *   echo 3000 > /sys/module/cpufreq_touch_boost/parameters/hold_ms
 */

#include <linux/cpufreq.h>
#include <linux/input.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pm_qos.h>
#include <linux/slab.h>
#include <linux/workqueue.h>

#define TB_MAX_CPUS 16

static unsigned int hold_ms;
module_param(hold_ms, uint, 0644);
MODULE_PARM_DESC(hold_ms, "boost held this long after the last touch event; 0 disables");

static unsigned int freqs[TB_MAX_CPUS];
static int nr_freqs;
module_param_array(freqs, uint, &nr_freqs, 0644);
MODULE_PARM_DESC(freqs, "minimum kHz per CPU, used for the policy that CPU leads");

struct tb_policy {
	struct cpufreq_policy *policy;
	struct freq_qos_request req;
};

static struct tb_policy tb_policies[TB_MAX_CPUS];
static DEFINE_MUTEX(tb_lock);
static unsigned long tb_last;	/* jiffies of the last touch event */
static bool tb_boosted;

static void tb_apply(bool on)
{
	unsigned int cpu;

	for (cpu = 0; cpu < TB_MAX_CPUS && cpu < nr_cpu_ids; cpu++) {
		struct tb_policy *tp = &tb_policies[cpu];

		if (!tp->policy) {
			struct cpufreq_policy *policy;

			if (!on || !freqs[cpu])
				continue;
			policy = cpufreq_cpu_get(cpu);
			if (!policy)
				continue;
			if (policy->cpu != cpu ||
			    freq_qos_add_request(&policy->constraints, &tp->req,
						 FREQ_QOS_MIN, FREQ_QOS_MIN_DEFAULT_VALUE) < 0) {
				cpufreq_cpu_put(policy);
				continue;
			}
			tp->policy = policy;
		}
		freq_qos_update_request(&tp->req, on ? freqs[cpu] : FREQ_QOS_MIN_DEFAULT_VALUE);
	}
}

static void tb_unboost_fn(struct work_struct *work);
static DECLARE_DELAYED_WORK(tb_unboost_work, tb_unboost_fn);

static void tb_unboost_fn(struct work_struct *work)
{
	unsigned long until = READ_ONCE(tb_last) + msecs_to_jiffies(READ_ONCE(hold_ms));

	mutex_lock(&tb_lock);
	if (time_before(jiffies, until)) {
		schedule_delayed_work(&tb_unboost_work, until - jiffies);
	} else if (tb_boosted) {
		tb_apply(false);
		WRITE_ONCE(tb_boosted, false);
	}
	mutex_unlock(&tb_lock);
}

static void tb_boost_fn(struct work_struct *work)
{
	unsigned int hold = READ_ONCE(hold_ms);

	if (!hold)
		return;
	mutex_lock(&tb_lock);
	if (!tb_boosted) {
		tb_apply(true);
		WRITE_ONCE(tb_boosted, true);
	}
	mutex_unlock(&tb_lock);
	schedule_delayed_work(&tb_unboost_work, msecs_to_jiffies(hold));
}
static DECLARE_WORK(tb_boost_work, tb_boost_fn);

/* Atomic context: note the time and leave the QoS update to a worker. */
static void tb_event(struct input_handle *handle, unsigned int type,
		     unsigned int code, int value)
{
	if (!READ_ONCE(hold_ms))
		return;
	WRITE_ONCE(tb_last, jiffies);
	if (!READ_ONCE(tb_boosted))
		queue_work(system_highpri_wq, &tb_boost_work);
}

static bool tb_match(struct input_handler *handler, struct input_dev *dev)
{
	return test_bit(INPUT_PROP_DIRECT, dev->propbit) &&
	       test_bit(ABS_MT_POSITION_X, dev->absbit);
}

static int tb_connect(struct input_handler *handler, struct input_dev *dev,
		      const struct input_device_id *id)
{
	struct input_handle *handle;
	int ret;

	handle = kzalloc(sizeof(*handle), GFP_KERNEL);
	if (!handle)
		return -ENOMEM;
	handle->dev = dev;
	handle->handler = handler;
	handle->name = "cpufreq-touch-boost";

	ret = input_register_handle(handle);
	if (ret)
		goto err_free;
	ret = input_open_device(handle);
	if (ret)
		goto err_unregister;
	return 0;

err_unregister:
	input_unregister_handle(handle);
err_free:
	kfree(handle);
	return ret;
}

static void tb_disconnect(struct input_handle *handle)
{
	input_close_device(handle);
	input_unregister_handle(handle);
	kfree(handle);
}

static const struct input_device_id tb_ids[] = {
	{
		.flags = INPUT_DEVICE_ID_MATCH_EVBIT | INPUT_DEVICE_ID_MATCH_ABSBIT,
		.evbit = { BIT_MASK(EV_ABS) },
		.absbit = { [BIT_WORD(ABS_MT_POSITION_X)] = BIT_MASK(ABS_MT_POSITION_X) },
	},
	{ }
};
MODULE_DEVICE_TABLE(input, tb_ids);

static struct input_handler tb_handler = {
	.event		= tb_event,
	.match		= tb_match,
	.connect	= tb_connect,
	.disconnect	= tb_disconnect,
	.name		= "cpufreq-touch-boost",
	.id_table	= tb_ids,
};

static int __init tb_init(void)
{
	return input_register_handler(&tb_handler);
}

static void __exit tb_exit(void)
{
	unsigned int cpu;

	input_unregister_handler(&tb_handler);
	cancel_work_sync(&tb_boost_work);
	cancel_delayed_work_sync(&tb_unboost_work);
	for (cpu = 0; cpu < TB_MAX_CPUS; cpu++) {
		if (!tb_policies[cpu].policy)
			continue;
		freq_qos_remove_request(&tb_policies[cpu].req);
		cpufreq_cpu_put(tb_policies[cpu].policy);
	}
}

module_init(tb_init);
module_exit(tb_exit);

MODULE_DESCRIPTION("Raise CPU frequency floors while a touchscreen is in use");
MODULE_LICENSE("GPL");
