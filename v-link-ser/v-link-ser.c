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

/* PAD0 - image sink, PAD1 - metadata sink */
#define V_LINK_SER_SINK_PAD0 0
#define V_LINK_SER_SINK_PAD1 1
#define V_LINK_SER_N_SINK_PADS 2

/* PAD2 - image source, PAD3 - metadata source */
#define V_LINK_SER_SRC_PAD0 2
#define V_LINK_SER_SRC_PAD1 3
#define V_LINK_SER_N_SRC_PADS 2

#define V_LINK_SER_N_PADS 4

struct v_link_ser_source {
	struct v4l2_subdev *sd;
	struct fwnode_handle *fwnode;
};

struct v_link_ser_priv {
	struct i2c_client *client;
	struct i2c_mux_core *mux;
	struct regmap *regmap;
	struct gpio_desc *pd_gpio;
	struct media_pad pads[V_LINK_SER_N_PADS];
	struct v_link_ser_source source;
	struct gpio_chip gpio_chip;
	struct v4l2_subdev sd;
	struct v4l2_async_notifier notifier;
	struct v4l2_ctrl_handler ctrls;
	struct v4l2_mbus_config_mipi_csi2 mipi_csi2;
	bool metadata_pad;
};

static inline struct v_link_ser_priv *sd_to_v_link_ser(struct v4l2_subdev *sd)
{
	return container_of(sd, struct v_link_ser_priv, sd);
}

static int v_link_ser_i2c_mux_select(struct i2c_mux_core *mux, u32 chan)
{
	return 0;
}

static int v_link_ser_i2c_mux_init(struct v_link_ser_priv *priv)
{
	priv->mux = i2c_mux_alloc(priv->client->adapter, &priv->client->dev, 1,
				  0, I2C_MUX_LOCKED | I2C_MUX_GATE,
				  v_link_ser_i2c_mux_select, NULL);
	if (!priv->mux)
		return -ENOMEM;

	return i2c_mux_add_adapter(priv->mux, 0, 0, 0);
}

static int v_link_ser_notify_bound(struct v4l2_async_notifier *notifier,
				   struct v4l2_subdev *subdev,
				   struct v4l2_async_connection *asd)
{
	struct v_link_ser_priv *priv = sd_to_v_link_ser(notifier->sd);
	struct device *dev = &priv->client->dev;
	int ret;
	unsigned int source_pad = 0;

	dev_dbg(dev, "Got subdev: %s", subdev->entity.name);

	priv->source.sd = subdev;

	for (int i = 0; i < 2; i++) {
		/* Find next source pad */
		while (source_pad < subdev->entity.num_pads &&
		       !(subdev->entity.pads[source_pad].flags &
			 MEDIA_PAD_FL_SOURCE))
			source_pad++;

		if (source_pad < subdev->entity.num_pads) {
			ret = media_create_pad_link(
				&subdev->entity, source_pad, &priv->sd.entity,
				i,
				MEDIA_LNK_FL_IMMUTABLE | MEDIA_LNK_FL_ENABLED);
		}

		if (ret) {
			dev_err(dev, "Unable to link %u to %d", source_pad, i);
			return ret;
		}

		dev_dbg(dev, "Linked %s:%u -> %s:%u", priv->source.sd->name,
			source_pad, priv->sd.name, i);

		source_pad++;
	}

	return 0;
}

static const struct v4l2_async_notifier_operations v_link_ser_notify_ops = {
	.bound = v_link_ser_notify_bound,
};

static int v_link_ser_v4l2_notifier_register(struct v_link_ser_priv *priv)
{
	struct device *dev = &priv->client->dev;
	struct v_link_ser_source *rxport = &priv->source;
	struct v4l2_async_connection *asd;
	int ret;

	if (!rxport->fwnode) {
		dev_err(dev, "No sink port");
		return 0;
	}

	v4l2_async_subdev_nf_init(&priv->notifier, &priv->sd);

	asd = v4l2_async_nf_add_fwnode(&priv->notifier, rxport->fwnode,
				       struct v4l2_async_connection);
	if (IS_ERR(asd)) {
		dev_err(dev, "Failed to add subdev: %p", asd);
		v4l2_async_nf_cleanup(&priv->notifier);
		return PTR_ERR(asd);
	}

	priv->notifier.ops = &v_link_ser_notify_ops;

	ret = v4l2_async_nf_register(&priv->notifier);
	if (ret) {
		dev_err(dev, "Failed to register subdev_notifier");
		v4l2_async_nf_cleanup(&priv->notifier);
		return ret;
	}

	return 0;
}

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

static int v_link_ser_s_stream(struct v4l2_subdev *sd, int enable)
{
	struct v_link_ser_priv *priv = sd_to_v_link_ser(sd);
	struct device *dev = &priv->client->dev;
	int ret = 0;

	if (enable) {
		v_link_ser_enable_sources(priv);

		/* Enable camera */
		ret = v4l2_subdev_call(priv->source.sd, video, s_stream, 1);
		if (ret)
			return ret;

		dev_dbg(dev, "Stream started");
	} else {
		/* Disable camera */
		ret = v4l2_subdev_call(priv->source.sd, video, s_stream, 0);
		if (ret)
			return ret;

		v_link_ser_disable_sources(priv);

		dev_dbg(dev, "Stream stopped");
	}

	return 0;
}

/*
	Returns a pad number to which this device 'pad' is connected to.
*/
static int v_link_ser_get_connected_pad(struct v_link_ser_priv *priv, int pad)
{
	struct device *dev = &priv->client->dev;
	struct media_pad *sink_pad;
	struct media_pad *src_pad;
	struct v4l2_subdev *connected_subdev;
	struct media_link *link;
	struct media_entity *src_entity;
	int opposite_pad = -1;

	sink_pad = &priv->sd.entity.pads[pad];
	if (!sink_pad)
		return -EINVAL;

	list_for_each_entry(link, &priv->sd.entity.links, list) {
		if (!(link->flags & MEDIA_LNK_FL_ENABLED))
			continue;

		if (link->sink == sink_pad) {
			src_pad = link->source;

			opposite_pad = src_pad->index;

			src_entity = src_pad->entity;

			connected_subdev =
				media_entity_to_v4l2_subdev(src_entity);

			dev_dbg(dev, "Pad %d is connected to pad %d of %s", pad,
				opposite_pad, connected_subdev->name);
		}
	}

	return opposite_pad;
}

static int v_link_ser_init_cfg(struct v4l2_subdev *sd,
			       struct v4l2_subdev_state *state)
{
	struct v_link_ser_priv *priv = sd_to_v_link_ser(sd);
	struct v4l2_mbus_framefmt *format;
	struct device *dev = &priv->client->dev;

	dev_dbg(dev, "Init cfg");

	/* Initialize the format. */
	format = v4l2_subdev_get_pad_format(sd, state, 0);
	format->width = 640, format->height = 480;
	format->code = MEDIA_BUS_FMT_SRGGB10_1X10;
	format->field = V4L2_FIELD_NONE;
	format->colorspace = V4L2_COLORSPACE_RAW;
	format->ycbcr_enc = V4L2_YCBCR_ENC_601;
	format->quantization = V4L2_QUANTIZATION_FULL_RANGE;
	format->xfer_func = V4L2_XFER_FUNC_NONE;

	/* Initialize embedded metadata pad */
	if (priv->metadata_pad) {
		format = v4l2_subdev_get_pad_format(sd, state, 1);
		format->code = MEDIA_BUS_FMT_SENSOR_DATA;
		format->width = 16384;
		format->height = 1;
		format->field = V4L2_FIELD_NONE;
	}

	return 0;
}

static int v_link_ser_set_fmt(struct v4l2_subdev *sd,
			      struct v4l2_subdev_state *sd_state,
			      struct v4l2_subdev_format *format)
{
	struct v_link_ser_priv *priv = sd_to_v_link_ser(sd);
	struct device *dev = &priv->client->dev;

	dev_dbg(dev, "Set fmt for pad: %d", format->pad);

	return 0;
}

static int v_link_ser_get_fmt(struct v4l2_subdev *sd,
			      struct v4l2_subdev_state *sd_state,
			      struct v4l2_subdev_format *format)
{
	struct v_link_ser_priv *priv = sd_to_v_link_ser(sd);
	struct device *dev = &priv->client->dev;
	int subdev_pad;
	int ret = 0;
	int num_sink_pad;

	dev_dbg(dev, "Get fmt for pad: %d", format->pad);

	num_sink_pad = (priv->metadata_pad) ? V_LINK_SER_N_SINK_PADS : 1;

	/* If asked for sink pad format we simply return opposite source format */
	if (format->pad >= num_sink_pad) {
		format->pad = format->pad - num_sink_pad;
	}

	subdev_pad = v_link_ser_get_connected_pad(priv, format->pad);
	if (subdev_pad < 0) {
		dev_warn(dev, "Unable to determine subdevice connection");
		return v4l2_subdev_get_fmt(sd, sd_state, format);
		//return -EINVAL;
	}

	ret = v4l2_subdev_call(priv->source.sd, pad, get_fmt,
			       priv->source.sd->active_state, format);

	dev_dbg(dev, "W: %d, H: %d, code: %d", format->format.width,
		format->format.height, format->format.code);

	return ret;
}

static const struct v4l2_subdev_video_ops v_link_ser_video_ops = {
	.s_stream = v_link_ser_s_stream,
};

static const struct v4l2_subdev_pad_ops v_link_ser_pad_ops = {
	.init_cfg = v_link_ser_init_cfg,
	.get_fmt = v_link_ser_get_fmt,
	.set_fmt = v_link_ser_set_fmt,
};

static const struct v4l2_subdev_ops v_link_ser_subdev_ops = {
	.video = &v_link_ser_video_ops,
	.pad = &v_link_ser_pad_ops,
};

static const struct media_entity_operations v_link_ser_media_ops = {
	.link_validate = v4l2_subdev_link_validate
};

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
	struct v4l2_mbus_config_mipi_csi2 *mipi = &priv->mipi_csi2;
	unsigned long lanes_used = 0;
	unsigned int nlanes, lane, val = 0;
	int ret = 0;

	nlanes = mipi->num_data_lanes;

	ret = cci_update_bits(priv->regmap, MAX96717_MIPI_RX1,
			      MAX96717_MIPI_LANES_CNT,
			      FIELD_PREP(MAX96717_MIPI_LANES_CNT, nlanes - 1),
			      NULL);

	/* Fixed values for this register based on HW design */
	val = 0x07;
	cci_update_bits(priv->regmap, MAX96717_MIPI_RX5,
			MAX96717_PHY2_LANES_POL,
			FIELD_PREP(MAX96717_PHY2_LANES_POL, val), &ret);

	/* Fixed values for this register based on HW design */
	val = 0x03;
	cci_update_bits(priv->regmap, MAX96717_MIPI_RX4,
			MAX96717_PHY1_LANES_POL,
			FIELD_PREP(MAX96717_PHY1_LANES_POL, val >> 3), &ret);

	/* lanes mapping */
	for (lane = 0, val = 0; lane < nlanes; lane++) {
		val |= (mipi->data_lanes[lane] - 1) << (lane * 2);
		lanes_used |= BIT(mipi->data_lanes[lane] - 1);
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

static int v_link_ser_create_subdev(struct v_link_ser_priv *priv)
{
	struct device *dev = &priv->client->dev;
	int ret, num_pads;

	v4l2_i2c_subdev_init(&priv->sd, priv->client, &v_link_ser_subdev_ops);

	v4l2_ctrl_handler_init(&priv->ctrls, 1);
	priv->sd.ctrl_handler = &priv->ctrls;

	priv->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	priv->sd.entity.function = MEDIA_ENT_F_VID_IF_BRIDGE;
	priv->sd.entity.ops = &v_link_ser_media_ops;

	if (priv->metadata_pad) {
		priv->pads[V_LINK_SER_SRC_PAD0].flags = MEDIA_PAD_FL_SOURCE;
		priv->pads[V_LINK_SER_SRC_PAD1].flags = MEDIA_PAD_FL_SOURCE;
		priv->pads[V_LINK_SER_SINK_PAD0].flags = MEDIA_PAD_FL_SINK;
		priv->pads[V_LINK_SER_SINK_PAD1].flags = MEDIA_PAD_FL_SINK;
	} else {
		priv->pads[0].flags = MEDIA_PAD_FL_SINK;
		priv->pads[1].flags = MEDIA_PAD_FL_SOURCE;
	}

	num_pads = (priv->metadata_pad) ? V_LINK_SER_N_PADS :
					  V_LINK_SER_N_PADS - 2;
	ret = media_entity_pads_init(&priv->sd.entity, num_pads, priv->pads);
	if (ret) {
		dev_err(dev, "Media pads init failed");
		goto err_free_ctrl;
	}

	priv->sd.state_lock = priv->sd.ctrl_handler->lock;

	ret = v4l2_subdev_init_finalize(&priv->sd);
	if (ret)
		goto err_entity_cleanup;

	/* Register v4l2 async notifiers for connected Camera subdevices */
	ret = v_link_ser_v4l2_notifier_register(priv);
	if (ret) {
		dev_err(dev, "Unable to register V4L2 async notifiers");
		goto err_subdev_cleanup;
	}

	ret = v4l2_async_register_subdev(&priv->sd);
	if (ret < 0) {
		dev_err(dev, "Unable to register subdevice");
		goto err_unreg_notif;
	}

	return 0;

err_unreg_notif:
	v4l2_async_nf_unregister(&priv->notifier);
	v4l2_async_nf_cleanup(&priv->notifier);
err_subdev_cleanup:
	v4l2_subdev_cleanup(&priv->sd);
err_entity_cleanup:
	media_entity_cleanup(&priv->sd.entity);
err_free_ctrl:
	v4l2_ctrl_handler_free(&priv->ctrls);

	return ret;
}

static int v_link_ser_parse_dt_sink(struct v_link_ser_priv *priv)
{
	struct device *dev = &priv->client->dev;
	struct v_link_ser_source *rxport = &priv->source;
	struct fwnode_handle *ep_fwnode;
	struct v4l2_fwnode_endpoint vep = { .bus_type = V4L2_MBUS_CSI2_DPHY };
	int ret;

	ep_fwnode = fwnode_graph_get_endpoint_by_id(dev_fwnode(dev), 0, 0, 0);
	if (!ep_fwnode) {
		dev_err(dev, "Failed to get sink endpoint\n");
		return -ENOENT;
	}

	ret = v4l2_fwnode_endpoint_alloc_parse(ep_fwnode, &vep);
	rxport->fwnode = fwnode_graph_get_remote_endpoint(ep_fwnode);
	fwnode_handle_put(ep_fwnode);

	if (!rxport->fwnode || ret) {
		dev_err(dev, "No remote endpoint (sink)");
		return -EINVAL;
	}

	dev_dbg(dev, "(sink) num lanes: %u", vep.bus.mipi_csi2.num_data_lanes);

	priv->mipi_csi2 = vep.bus.mipi_csi2;

	return 0;
}

static int v_link_ser_parse_dt(struct v_link_ser_priv *priv)
{
	struct device *dev = &priv->client->dev;
	int ret;

	ret = v_link_ser_parse_dt_sink(priv);
	if (ret)
		return ret;

	if (of_property_read_bool(dev->of_node, "no-metadata")) {
		dev_dbg(dev, "Metadata pad not requested.");
		priv->metadata_pad = false;
	} else {
		dev_dbg(dev, "Metadata pad requested");
		priv->metadata_pad = true;
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

	ret = v_link_ser_get_hw_resources(priv);
	if (ret)
		return ret;

	ret = v_link_ser_parse_dt(priv);
	if (ret) {
		dev_err_probe(dev, ret, "Failed to parse dt\n");
		return ret;
	}

	ret = v_link_ser_i2c_mux_init(priv);
	if (ret) {
		dev_err(&client->dev, "Unable to initialize I2C multiplexer");
		goto err_disable;
	}

	ret = v_link_ser_setup(priv);
	if (ret) {
		dev_err(&client->dev, "Unable to setup serializer");
		goto err_del_mux;
	}

	ret = v_link_ser_create_subdev(priv);
	if (ret)
		goto err_del_mux;

	return 0;

err_del_mux:
	i2c_mux_del_adapters(priv->mux);
err_disable:
	fwnode_handle_put(priv->source.fwnode);
	return ret;
}

static void v_link_ser_remove(struct i2c_client *client)
{
	struct v_link_ser_priv *priv =
		sd_to_v_link_ser(i2c_get_clientdata(client));

	v4l2_async_nf_unregister(&priv->notifier);
	v4l2_async_nf_cleanup(&priv->notifier);
	v4l2_async_unregister_subdev(&priv->sd);
	v4l2_subdev_cleanup(&priv->sd);
	media_entity_cleanup(&priv->sd.entity);
	v4l2_ctrl_handler_free(&priv->ctrls);
	i2c_mux_del_adapters(priv->mux);
	fwnode_handle_put(priv->source.fwnode);
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
