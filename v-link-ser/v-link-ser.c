// SPDX-License-Identifier: GPL-2.0+
/*
 * Videtronic v-link serializer driver
 *
 * Copyright (C) 2024 Videtronic.
 * 
 * Based on MAX96717 driver by Collabora Ltd.
 * Copyright (C) 2024 Collabora Ltd.
 */

#include <linux/delay.h>
#include <linux/bitfield.h>
#include <linux/device.h>
#include <linux/fwnode.h>
#include <linux/of_device.h>
#include <linux/gpio/consumer.h>
#include <linux/gpio/driver.h>
#include <linux/i2c.h>
#include <linux/i2c-mux.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/regmap.h>

#include <media/v4l2-cci.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-subdev.h>

//#undef dev_dbg
//#define dev_dbg dev_info

#define MAX96717_NUM_GPIO 1
#define MAX96717_GPIO_REG_A(gpio) CCI_REG8(0x2be + (gpio)*3)
#define MAX96717_GPIO_OUT BIT(4)
#define MAX96717_GPIO_IN BIT(3)
#define MAX96717_GPIO_RX_EN BIT(2)
#define MAX96717_GPIO_TX_EN BIT(1)
#define MAX96717_GPIO_OUT_DIS BIT(0)
#define MAX96717_MIPI_RX1 CCI_REG8(0x331)
#define MAX96717_MIPI_LANES_CNT GENMASK(5, 4)
#define MAX96717_MIPI_RX2 CCI_REG8(0x332)
#define MAX96717_PHY2_LANES_MAP GENMASK(7, 4)
#define MAX96717_MIPI_RX3 CCI_REG8(0x333)
#define MAX96717_PHY1_LANES_MAP GENMASK(3, 0)
#define MAX96717_MIPI_RX4 CCI_REG8(0x334)
#define MAX96717_PHY1_LANES_POL GENMASK(6, 4)
#define MAX96717_MIPI_RX5 CCI_REG8(0x335)
#define MAX96717_PHY2_LANES_POL GENMASK(2, 0)
#define MAX96717_FRONTOP0 CCI_REG8(0x308)
#define MAX96717_START_PORT_B BIT(5)

#define MAX96717_CSI_NLANES 4

#define MHZ(v) ((u32)((v)*1000000U))

struct v_link_ser_priv {
	struct i2c_client *client;
	struct regmap *regmap;
	struct gpio_desc *pd_gpio;
	struct gpio_chip gpio_chip;
	u32 data_lanes[MAX96717_CSI_NLANES];
	u64 link_freq;
	u32 nlanes;
};

static int v_link_ser_enable_sources(struct v_link_ser_priv *priv)
{
	struct device *dev = &priv->client->dev;

	int ret;

	ret = cci_update_bits(priv->regmap, MAX96717_FRONTOP0,
			      MAX96717_START_PORT_B, MAX96717_START_PORT_B,
			      NULL);
	if (ret) {
		dev_err(dev, "Unable to start serializer CSI port.");
		return ret;
	}

	return ret;
}

static int v_link_ser_disable_sources(struct v_link_ser_priv *priv)
{
	struct device *dev = &priv->client->dev;
	int ret;

	ret = cci_update_bits(priv->regmap, MAX96717_FRONTOP0,
			      MAX96717_START_PORT_B, 0, NULL);
	if (ret) {
		dev_err(dev, "Unable to stop serializer CSI port.");
		return ret;
	}

	return ret;
}

static int v_link_ser_gpiochip_get(struct gpio_chip *gpiochip,
				   unsigned int offset)
{
	struct v_link_ser_priv *priv = gpiochip_get_data(gpiochip);
	u64 val;
	int ret;

	ret = cci_read(priv->regmap, MAX96717_GPIO_REG_A(offset), &val, NULL);
	if (ret)
		return ret;

	if (val & MAX96717_GPIO_OUT_DIS)
		return !!(val & MAX96717_GPIO_IN);
	else
		return !!(val & MAX96717_GPIO_OUT);
}

static void v_link_ser_gpiochip_set(struct gpio_chip *gpiochip,
				    unsigned int offset, int value)
{
	struct v_link_ser_priv *priv = gpiochip_get_data(gpiochip);

	cci_update_bits(priv->regmap, MAX96717_GPIO_REG_A(offset),
			MAX96717_GPIO_OUT, MAX96717_GPIO_OUT, NULL);
}

static int v_link_ser_gpio_get_direction(struct gpio_chip *gpiochip,
					 unsigned int offset)
{
	struct v_link_ser_priv *priv = gpiochip_get_data(gpiochip);
	u64 val;
	int ret;

	ret = cci_read(priv->regmap, MAX96717_GPIO_REG_A(offset), &val, NULL);
	if (ret < 0)
		return ret;

	return !!(val & MAX96717_GPIO_OUT_DIS);
}

static int v_link_ser_gpio_direction_out(struct gpio_chip *gpiochip,
					 unsigned int offset, int value)
{
	struct v_link_ser_priv *priv = gpiochip_get_data(gpiochip);

	return cci_update_bits(priv->regmap, MAX96717_GPIO_REG_A(offset),
			       MAX96717_GPIO_OUT_DIS | MAX96717_GPIO_OUT,
			       value ? MAX96717_GPIO_OUT : 0, NULL);
}

static int v_link_ser_gpio_direction_in(struct gpio_chip *gpiochip,
					unsigned int offset)
{
	struct v_link_ser_priv *priv = gpiochip_get_data(gpiochip);

	return cci_update_bits(priv->regmap, MAX96717_GPIO_REG_A(offset),
			       MAX96717_GPIO_OUT_DIS, MAX96717_GPIO_OUT_DIS,
			       NULL);
}

static int v_link_ser_gpiochip_probe(struct v_link_ser_priv *priv)
{
	struct device *dev = &priv->client->dev;
	struct gpio_chip *gc = &priv->gpio_chip;
	int i, ret = 0;

	gc->label = dev_name(dev);
	gc->parent = dev;
	gc->owner = THIS_MODULE;
	gc->ngpio =
		MAX96717_NUM_GPIO; /* We only use camera reset so 1 is enough */
	gc->base = -1;
	gc->can_sleep = true;
	gc->get_direction = v_link_ser_gpio_get_direction;
	gc->direction_input = v_link_ser_gpio_direction_in;
	gc->direction_output = v_link_ser_gpio_direction_out;
	gc->set = v_link_ser_gpiochip_set;
	gc->get = v_link_ser_gpiochip_get;
	gc->of_gpio_n_cells = 2;

	/* Disable GPIO forwarding */
	for (i = 0; i < gc->ngpio; i++)
		cci_update_bits(priv->regmap, MAX96717_GPIO_REG_A(i),
				MAX96717_GPIO_RX_EN | MAX96717_GPIO_TX_EN, 0,
				&ret);

	if (ret)
		return ret;

	ret = devm_gpiochip_add_data(dev, gc, priv);
	if (ret) {
		dev_err(dev, "Unable to create gpio_chip");
		return ret;
	}

	return 0;
}

static int v_link_ser_setup(struct v_link_ser_priv *priv)
{
	unsigned long lanes_used = 0;
	unsigned int lane, val = 0;
	int ret = 0;

	ret = cci_update_bits(
		priv->regmap, MAX96717_MIPI_RX1, MAX96717_MIPI_LANES_CNT,
		FIELD_PREP(MAX96717_MIPI_LANES_CNT, priv->nlanes - 1), NULL);

	/* Fixed values for this register based on HW design */
	val = 0x07;
	cci_update_bits(priv->regmap, MAX96717_MIPI_RX5,
			MAX96717_PHY2_LANES_POL,
			FIELD_PREP(MAX96717_PHY2_LANES_POL, val), &ret);

	/* Fixed values for this register based on HW design */
	val = 0x03;
	cci_update_bits(priv->regmap, MAX96717_MIPI_RX4,
			MAX96717_PHY1_LANES_POL,
			FIELD_PREP(MAX96717_PHY1_LANES_POL, val), &ret);

	/* lanes mapping */
	for (lane = 0, val = 0; lane < priv->nlanes; lane++) {
		val |= (priv->data_lanes[lane] - 1) << (lane * 2);
		lanes_used |= BIT(priv->data_lanes[lane] - 1);
	}

	/*
		* Unused lanes need to be mapped as well to not have
		* the same lanes mapped twice.
		*/
	for (; lane < MAX96717_CSI_NLANES; lane++) {
		unsigned int idx =
			find_first_zero_bit(&lanes_used, MAX96717_CSI_NLANES);

		val |= idx << (lane * 2);
		lanes_used |= BIT(idx);
	}

	cci_update_bits(priv->regmap, MAX96717_MIPI_RX3,
			MAX96717_PHY1_LANES_MAP,
			FIELD_PREP(MAX96717_PHY1_LANES_MAP, val), &ret);

	cci_update_bits(priv->regmap, MAX96717_MIPI_RX2,
			MAX96717_PHY2_LANES_MAP,
			FIELD_PREP(MAX96717_PHY2_LANES_MAP, val >> 4), &ret);

	return v_link_ser_gpiochip_probe(priv);
}

static int v_link_ser_parse_dt(struct v_link_ser_priv *priv)
{
	struct device *dev = &priv->client->dev;
	int ret, i;

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

	return ret;
}

static int v_link_ser_get_hw_resources(struct v_link_ser_priv *priv)
{
	priv->regmap = devm_cci_regmap_init_i2c(priv->client, 16);
	if (IS_ERR(priv->regmap))
		return PTR_ERR(priv->regmap);

	return 0;
}

static int v_link_ser_probe(struct i2c_client *client)
{
	struct v_link_ser_priv *priv;
	struct device *dev;
	int ret;

	priv = devm_kzalloc(&client->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->client = client;

	dev = &priv->client->dev;

	i2c_set_clientdata(client, priv);

	ret = v_link_ser_get_hw_resources(priv);
	if (ret)
		return ret;

	ret = v_link_ser_parse_dt(priv);
	if (ret) {
		dev_err_probe(dev, ret, "Failed to parse dt\n");
		return ret;
	}

	ret = v_link_ser_setup(priv);
	if (ret) {
		dev_err(&client->dev, "Unable to setup serializer");
		return ret;;
	}

	return v_link_ser_enable_sources(priv);
}

static void v_link_ser_remove(struct i2c_client *client)
{
	struct v_link_ser_priv *priv = i2c_get_clientdata(client);

}

static const struct of_device_id v_link_ser_dt_ids[] = {
	{ .compatible = "videtronic,v-link-ser" },
	{},
};
MODULE_DEVICE_TABLE(of, v_link_ser_dt_ids);

static struct i2c_driver v_link_ser_i2c_driver = {
	 .driver	= {
		 .name		= "v-link-ser",
		 .of_match_table	= v_link_ser_dt_ids,
	 },
	 .probe		= v_link_ser_probe,
	 .remove		= v_link_ser_remove,
 };

module_i2c_driver(v_link_ser_i2c_driver);

MODULE_DESCRIPTION("Videtronic v-link serializer driver");
MODULE_AUTHOR("Jakub Kostiw");
MODULE_LICENSE("GPL");
