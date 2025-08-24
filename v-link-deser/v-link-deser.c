// SPDX-License-Identifier: GPL-2.0+
/*
 * Videtronic v-link deserializer driver
 *
 * Copyright (C) 2024 Videtronic.
 * 
 * Based on MAX96714 driver by Collabora Ltd.
 */

#include <linux/delay.h>
#include <linux/bitfield.h>
#include <linux/device.h>
#include <linux/fwnode.h>
#include <linux/of_device.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/i2c-mux.h>
#include <linux/module.h>
#include <linux/regmap.h>

#include <media/v4l2-cci.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-subdev.h>

//#undef dev_dbg
//#define dev_dbg dev_info

#define MAX96714_MIPI_STDBY_N CCI_REG8(0x332)
#define MAX96714_MIPI_STDBY_MASK GENMASK(5, 4)
#define MAX96714_BACKTOP25 CCI_REG8(0x320)
#define CSI_DPLL_FREQ_MASK GENMASK(4, 0)
#define MAX96714_MIPI_LANE_CNT CCI_REG8(0x44a)
#define MAX96714_CSI2_LANE_CNT_MASK GENMASK(7, 6)
#define MAX96714_MIPI_POLARITY CCI_REG8(0x335)
#define MAX96714_MIPI_POLARITY_MASK GENMASK(5, 0)
#define MAX96714_MIPI_LANE_MAP CCI_REG8(0x333)

#define MAX96714_CSI_NLANES 4

#define MHZ(v) ((u32)((v)*1000000U))

struct v_link_deser_priv {
	struct i2c_client *client;
	struct regmap *regmap;
	struct gpio_desc *pd_gpio;
	u32 data_lanes[MAX96714_CSI_NLANES];
	u64 link_freq;
	u32 nlanes;
};

static int v_link_deser_enable_sources(struct v_link_deser_priv *priv)
{
	struct device *dev = &priv->client->dev;
	int ret;

	ret = cci_update_bits(priv->regmap, MAX96714_MIPI_STDBY_N,
			      MAX96714_MIPI_STDBY_MASK,
			      MAX96714_MIPI_STDBY_MASK, NULL);
	if (ret) {
		dev_err(dev, "Unable to start deserializer CSI port.");
		return ret;
	}

	return ret;
}

static int v_link_deser_disable_sources(struct v_link_deser_priv *priv)
{
	struct device *dev = &priv->client->dev;
	int ret;

	ret = cci_update_bits(priv->regmap, MAX96714_MIPI_STDBY_N,
			      MAX96714_MIPI_STDBY_MASK, 0, NULL);
	if (ret) {
		dev_err(dev, "Unable to stop deserializer CSI port.");
		return ret;
	}

	return ret;
}

static int v_link_deser_setup(struct v_link_deser_priv *priv)
{
	struct device *dev = &priv->client->dev;
	unsigned int val, lane;
	unsigned long lanes_used = 0;
	int ret = 0;

	val = 0xc4;
	cci_write(priv->regmap, CCI_REG8(0x0332), val, &ret);

	val = div_u64(priv->link_freq * 2, MHZ(100));
	cci_update_bits(priv->regmap, MAX96714_BACKTOP25, CSI_DPLL_FREQ_MASK,
			val, &ret);

	val = FIELD_PREP(MAX96714_CSI2_LANE_CNT_MASK, priv->nlanes - 1);
	cci_update_bits(priv->regmap, MAX96714_MIPI_LANE_CNT,
			MAX96714_CSI2_LANE_CNT_MASK, val, &ret);

	/* Fixed values for this register based on HW design */
	val = 0x3b; //0x23;
	cci_write(priv->regmap, MAX96714_MIPI_POLARITY, val, &ret);

	/* lanes mapping */
	val = 0;
	for (lane = 0; lane < priv->nlanes; lane++) {
		val |= (priv->data_lanes[lane] - 1) << (lane * 2);
		lanes_used |= BIT(priv->data_lanes[lane] - 1);
	}

	/*
	 * Unused lanes need to be mapped as well to not have
	 * the same lanes mapped twice.
	 */
	for (; lane < MAX96714_CSI_NLANES; lane++) {
		unsigned int idx =
			find_first_zero_bit(&lanes_used, MAX96714_CSI_NLANES);

		val |= idx << (lane * 2);
		lanes_used |= BIT(idx);
	}

	return cci_write(priv->regmap, MAX96714_MIPI_LANE_MAP, val, &ret);
}

static int v_link_deser_parse_dt(struct v_link_deser_priv *priv)
{
	struct device *dev = &priv->client->dev;
	int ret, i;

	ret = of_property_read_u64_array(dev->of_node, "link-frequency",
					 &priv->link_freq, 1);
	if (ret) {
		dev_err_probe(dev, ret, "Missing link-frequency property\n");
	}

	/* Min 50MHz, Max 1250MHz, 50MHz step */
	if (priv->link_freq < MHZ(50) || priv->link_freq > MHZ(1250) ||
	    (u32)priv->link_freq % MHZ(50)) {
		dev_err(dev, "Invalid link frequency");
		return -EINVAL;
	}

	ret = of_property_count_u32_elems(dev->of_node, "data-lanes");
	if (ret < 0) {
		dev_err_probe(dev, ret, "Missing data-lanes property\n");
		return ret;
	}

	priv->nlanes = ret;

	if (priv->nlanes < 1 || priv->nlanes > 4) {
		dev_err(dev, "Invalid number of data lanes must be 1 to 4");
		return -EINVAL;
	}

	ret = of_property_read_u32_array(dev->of_node, "data-lanes",
					priv->data_lanes, priv->nlanes);
	if (ret) {
		dev_err_probe(dev, ret, "Unable to parse data-lanes\n");
		return ret;
	}

	return 0;
}

static int v_link_deser_enable_core_hw(struct v_link_deser_priv *priv)
{
	if (priv->pd_gpio) {
		/* wait min 2 ms for reset to complete */
		gpiod_set_value_cansleep(priv->pd_gpio, 1);
		fsleep(2000);
		gpiod_set_value_cansleep(priv->pd_gpio, 0);
		/* wait min 2 ms for power up to finish */
		fsleep(2000);
	}

	return 0;
}

static void v_link_deser_disable_core_hw(struct v_link_deser_priv *priv)
{
	gpiod_set_value_cansleep(priv->pd_gpio, 1);
}

static int v_link_deser_get_hw_resources(struct v_link_deser_priv *priv)
{
	struct device *dev = &priv->client->dev;

	priv->regmap = devm_cci_regmap_init_i2c(priv->client, 16);
	if (IS_ERR(priv->regmap))
		return PTR_ERR(priv->regmap);

	priv->pd_gpio =
		devm_gpiod_get_optional(dev, "powerdown", GPIOD_OUT_HIGH);
	if (IS_ERR(priv->pd_gpio))
		return dev_err_probe(dev, PTR_ERR(priv->pd_gpio),
				     "Cannot get powerdown GPIO\n");
	return 0;
}

static int v_link_deser_probe(struct i2c_client *client)
{
	struct v_link_deser_priv *priv;
	struct device *dev;
	int ret;

	priv = devm_kzalloc(&client->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->client = client;

	dev = &priv->client->dev;

	i2c_set_clientdata(client, priv);

	ret = v_link_deser_get_hw_resources(priv);
	if (ret)
		return ret;

	ret = v_link_deser_enable_core_hw(priv);
	if (ret)
		return ret;

	ret = v_link_deser_parse_dt(priv);
	if (ret) {
		dev_err_probe(dev, ret, "Failed to parse dt\n");
		goto err_disable_core_hw;
	}

	ret = v_link_deser_setup(priv);
	if (ret) {
		dev_err(&client->dev, "Unable to setup max96714\n");
		goto err_disable_core_hw;
	}

	return v_link_deser_enable_sources(priv);

err_disable_core_hw:
	v_link_deser_disable_core_hw(priv);

	return ret;
}

static void v_link_deser_remove(struct i2c_client *client)
{
	struct v_link_deser_priv *priv = i2c_get_clientdata(client);
}

static const struct of_device_id v_link_deser_dt_ids[] = {
	{ .compatible = "videtronic,v-link-deser" },
	{},
};
MODULE_DEVICE_TABLE(of, v_link_deser_dt_ids);

static struct i2c_driver v_link_deser_i2c_driver = {
	.driver	= {
		.name		= "v-link-deser",
		.of_match_table	= v_link_deser_dt_ids,
	},
	.probe		= v_link_deser_probe,
	.remove		= v_link_deser_remove,
};

module_i2c_driver(v_link_deser_i2c_driver);

MODULE_DESCRIPTION("Videtronic v-link deserializer driver");
MODULE_AUTHOR("Jakub Kostiw");
MODULE_LICENSE("GPL");
