// SPDX-License-Identifier: GPL-2.0-only
/*
 * Google Tensor G3 (zuma) memory interconnect provider
 *
 * The CPU clusters and the GPU vote for the memory (MIF), internal NoC
 * (INT), CPU cluster interconnect (DSU) and its bus interface (BCI) buses
 * through the opp-peak-kBps of their operating points. This provider aggregates those votes per bus and turns each bus's
 * total into a PM QoS minimum frequency on its devfreq device (an exynos-bus
 * node listed in google,buses), using that node's samsung,data-clock-ratio,
 * as the generic Exynos interconnect provider does for a single bus.
 */

#include <linux/device.h>
#include <linux/interconnect-provider.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/pm_qos.h>

#include <dt-bindings/interconnect/google,zuma.h>

#define ZUMA_NUM_NODES		(ZUMA_SLAVE_BCI + 1)
#define ZUMA_NUM_SLAVES		(ZUMA_SLAVE_BCI - ZUMA_SLAVE_MIF + 1)

struct zuma_icc_bus {
	struct device *dev;
	struct dev_pm_qos_request qos_req;
	u32 bus_clk_ratio;
};

struct zuma_icc {
	struct icc_provider provider;
	struct icc_onecell_data *data;
	struct zuma_icc_bus buses[ZUMA_NUM_SLAVES];
};

static const char * const zuma_icc_names[ZUMA_NUM_NODES] = {
	[ZUMA_MASTER_CPUCL0] = "cpucl0",
	[ZUMA_MASTER_CPUCL1] = "cpucl1",
	[ZUMA_MASTER_CPUCL2] = "cpucl2",
	[ZUMA_MASTER_G3D] = "g3d",
	[ZUMA_SLAVE_MIF] = "mif",
	[ZUMA_SLAVE_DSU] = "dsu",
	[ZUMA_SLAVE_INT] = "int",
	[ZUMA_SLAVE_BCI] = "bci",
};

static const struct {
	int master, slave;
} zuma_icc_links[] = {
	{ ZUMA_MASTER_CPUCL0, ZUMA_SLAVE_MIF },
	{ ZUMA_MASTER_CPUCL1, ZUMA_SLAVE_MIF },
	{ ZUMA_MASTER_CPUCL2, ZUMA_SLAVE_MIF },
	{ ZUMA_MASTER_G3D, ZUMA_SLAVE_MIF },
	{ ZUMA_MASTER_CPUCL0, ZUMA_SLAVE_DSU },
	{ ZUMA_MASTER_CPUCL1, ZUMA_SLAVE_DSU },
	{ ZUMA_MASTER_CPUCL2, ZUMA_SLAVE_DSU },
	{ ZUMA_MASTER_CPUCL0, ZUMA_SLAVE_INT },
	{ ZUMA_MASTER_CPUCL1, ZUMA_SLAVE_INT },
	{ ZUMA_MASTER_CPUCL2, ZUMA_SLAVE_INT },
	{ ZUMA_MASTER_G3D, ZUMA_SLAVE_INT },
	{ ZUMA_MASTER_CPUCL0, ZUMA_SLAVE_BCI },
	{ ZUMA_MASTER_CPUCL1, ZUMA_SLAVE_BCI },
	{ ZUMA_MASTER_CPUCL2, ZUMA_SLAVE_BCI },
};

static int zuma_icc_set(struct icc_node *src, struct icc_node *dst)
{
	struct zuma_icc_bus *bus = dst->data;
	s32 freq;
	int ret;

	/* Only the slave (bus) nodes carry a bus. */
	if (!bus)
		return 0;

	freq = max(dst->avg_bw, dst->peak_bw) / bus->bus_clk_ratio;

	ret = dev_pm_qos_update_request(&bus->qos_req, freq);
	if (ret < 0) {
		dev_err(dst->provider->dev, "failed to update PM QoS of %s\n",
			dst->name);
		return ret;
	}

	return 0;
}

/*
 * The buses run at the bootloader's levels until the first vote: no
 * boot-time floor, so nothing waits for every consumer to probe.
 */
static int zuma_icc_get_bw(struct icc_node *node, u32 *avg, u32 *peak)
{
	*avg = 0;
	*peak = 0;
	return 0;
}

static void zuma_icc_put_buses(struct zuma_icc *icc)
{
	int i;

	for (i = 0; i < ZUMA_NUM_SLAVES; i++) {
		if (!icc->buses[i].dev)
			continue;
		if (dev_pm_qos_request_active(&icc->buses[i].qos_req))
			dev_pm_qos_remove_request(&icc->buses[i].qos_req);
		put_device(icc->buses[i].dev);
		icc->buses[i].dev = NULL;
	}
}

static int zuma_icc_get_buses(struct device *dev, struct zuma_icc *icc)
{
	struct zuma_icc_bus *bus;
	struct platform_device *pdev;
	struct device_node *np;
	int i, ret;

	for (i = 0; i < ZUMA_NUM_SLAVES; i++) {
		bus = &icc->buses[i];

		np = of_parse_phandle(dev->of_node, "google,buses", i);
		if (!np)
			return dev_err_probe(dev, -EINVAL, "no bus %d\n", i);

		if (of_property_read_u32(np, "samsung,data-clock-ratio",
					 &bus->bus_clk_ratio) ||
		    !bus->bus_clk_ratio) {
			of_node_put(np);
			return dev_err_probe(dev, -EINVAL,
					     "%pOF: no samsung,data-clock-ratio\n",
					     np);
		}

		pdev = of_find_device_by_node(np);
		of_node_put(np);
		if (!pdev)
			return -EPROBE_DEFER;
		bus->dev = &pdev->dev;

		/* The QoS request only takes effect once devfreq is there. */
		if (!device_is_bound(bus->dev))
			return -EPROBE_DEFER;

		ret = dev_pm_qos_add_request(bus->dev, &bus->qos_req,
					     DEV_PM_QOS_MIN_FREQUENCY, 0);
		if (ret < 0)
			return dev_err_probe(dev, ret, "no PM QoS request\n");
	}

	return 0;
}

static int zuma_icc_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct icc_provider *provider;
	struct icc_node *node;
	struct zuma_icc *icc;
	int i, ret;

	icc = devm_kzalloc(dev, sizeof(*icc), GFP_KERNEL);
	if (!icc)
		return -ENOMEM;

	icc->data = devm_kzalloc(dev, struct_size(icc->data, nodes,
						  ZUMA_NUM_NODES), GFP_KERNEL);
	if (!icc->data)
		return -ENOMEM;
	icc->data->num_nodes = ZUMA_NUM_NODES;

	ret = zuma_icc_get_buses(dev, icc);
	if (ret)
		goto err_buses;

	provider = &icc->provider;
	provider->dev = dev;
	provider->set = zuma_icc_set;
	provider->aggregate = icc_std_aggregate;
	provider->get_bw = zuma_icc_get_bw;
	provider->xlate = of_icc_xlate_onecell;
	provider->data = icc->data;
	icc_provider_init(provider);

	for (i = 0; i < ZUMA_NUM_NODES; i++) {
		node = icc_node_create_dyn();
		if (IS_ERR(node)) {
			ret = PTR_ERR(node);
			goto err_nodes;
		}
		ret = icc_node_set_name(node, provider, zuma_icc_names[i]);
		if (ret) {
			icc_node_destroy(node->id);
			goto err_nodes;
		}
		if (i >= ZUMA_SLAVE_MIF)
			node->data = &icc->buses[i - ZUMA_SLAVE_MIF];
		icc_node_add(node, provider);
		icc->data->nodes[i] = node;
	}

	for (i = 0; i < ARRAY_SIZE(zuma_icc_links); i++) {
		ret = icc_link_nodes(icc->data->nodes[zuma_icc_links[i].master],
				     &icc->data->nodes[zuma_icc_links[i].slave]);
		if (ret)
			goto err_nodes;
	}

	ret = icc_provider_register(provider);
	if (ret)
		goto err_nodes;

	platform_set_drvdata(pdev, icc);
	return 0;

err_nodes:
	icc_nodes_remove(provider);
err_buses:
	zuma_icc_put_buses(icc);
	return ret;
}

static void zuma_icc_remove(struct platform_device *pdev)
{
	struct zuma_icc *icc = platform_get_drvdata(pdev);

	icc_provider_deregister(&icc->provider);
	icc_nodes_remove(&icc->provider);
	zuma_icc_put_buses(icc);
}

static const struct of_device_id zuma_icc_of_match[] = {
	{ .compatible = "google,zuma-icc" },
	{ }
};
MODULE_DEVICE_TABLE(of, zuma_icc_of_match);

static struct platform_driver zuma_icc_driver = {
	.probe = zuma_icc_probe,
	.remove = zuma_icc_remove,
	.driver = {
		.name = "zuma-icc",
		.of_match_table = zuma_icc_of_match,
	},
};
module_platform_driver(zuma_icc_driver);

MODULE_DESCRIPTION("Google Tensor G3 (zuma) memory interconnect provider");
MODULE_LICENSE("GPL");
