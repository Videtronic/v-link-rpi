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

#undef dev_dbg
#define dev_dbg dev_info

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

/* PAD0 - image sink, PAD1 - metadata sink */
#define V_LINK_DESER_SINK_PAD0 0
#define V_LINK_DESER_SINK_PAD1 1
#define V_LINK_DESER_N_SINK_PADS 2

/* PAD2 - image source, PAD3 - metadata source */
#define V_LINK_DESER_SRC_PAD0 2
#define V_LINK_DESER_SRC_PAD1 3
#define V_LINK_DESER_N_SRC_PADS 2

#define V_LINK_DESER_N_PADS 4

struct v_link_deser_source {
	struct v4l2_subdev *sd;
	struct fwnode_handle *fwnode;
};

struct v_link_deser_priv {
	struct i2c_client *client;
	struct i2c_mux_core *mux;
	struct regmap *regmap;
	struct gpio_desc *pd_gpio;
	struct media_pad pads[V_LINK_DESER_N_PADS];
	struct v_link_deser_source source;
	struct v4l2_subdev sd;
	struct v4l2_async_notifier notifier;
	struct v4l2_ctrl_handler ctrls;
	struct v4l2_mbus_config_mipi_csi2 mipi_csi2;

	s64 link_freq;
	bool metadata_pad;
};

static inline struct v_link_deser_priv *
sd_to_v_link_deser(struct v4l2_subdev *sd)
{
	return container_of(sd, struct v_link_deser_priv, sd);
}

static int v_link_deser_i2c_mux_select(struct i2c_mux_core *mux, u32 chan)
{
	return 0;
}

static int v_link_deser_i2c_mux_init(struct v_link_deser_priv *priv)
{
	priv->mux = i2c_mux_alloc(priv->client->adapter, &priv->client->dev, 1,
				  0, I2C_MUX_LOCKED | I2C_MUX_GATE,
				  v_link_deser_i2c_mux_select, NULL);
	if (!priv->mux)
		return -ENOMEM;

	return i2c_mux_add_adapter(priv->mux, 0, 0);
}

static int v_link_deser_notify_bound(struct v4l2_async_notifier *notifier,
				     struct v4l2_subdev *subdev,
				     struct v4l2_async_connection *asd)
{
	struct v_link_deser_priv *priv = sd_to_v_link_deser(notifier->sd);
	struct device *dev = &priv->client->dev;
	int ret;
	unsigned int source_pad = 0;

	dev_dbg(dev, "Got subdev: %s", subdev->entity.name);

	priv->source.sd = subdev;

	for (int i = 0; i < V_LINK_DESER_N_SINK_PADS; i++) {
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

static const struct v4l2_async_notifier_operations v_link_deser_notify_ops = {
	.bound = v_link_deser_notify_bound,
};

static int v_link_deser_v4l2_notifier_register(struct v_link_deser_priv *priv)
{
	struct device *dev = &priv->client->dev;
	struct v_link_deser_source *rxport = &priv->source;
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

	priv->notifier.ops = &v_link_deser_notify_ops;

	ret = v4l2_async_nf_register(&priv->notifier);
	if (ret) {
		dev_err(dev, "Failed to register subdev_notifier");
		v4l2_async_nf_cleanup(&priv->notifier);
		return ret;
	}

	return 0;
}

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

static int v_link_deser_s_stream(struct v4l2_subdev *sd, int enable)
{
	struct v_link_deser_priv *priv = sd_to_v_link_deser(sd);
	struct device *dev = &priv->client->dev;
	int ret = 0;

	if (enable) {
		v_link_deser_enable_sources(priv);

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

		v_link_deser_disable_sources(priv);

		dev_dbg(dev, "Stream stopped");
	}

	return 0;
}

/*
	Returns a pad number to which this device 'pad' is connected to.
*/
static int v_link_deser_get_connected_pad(struct v_link_deser_priv *priv,
					  int pad)
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

static int v_link_deser_set_fmt(struct v4l2_subdev *sd,
				struct v4l2_subdev_state *sd_state,
				struct v4l2_subdev_format *format)
{
	return 0;
}

static int v_link_deser_get_fmt(struct v4l2_subdev *sd,
				struct v4l2_subdev_state *sd_state,
				struct v4l2_subdev_format *format)
{
	struct v_link_deser_priv *priv = sd_to_v_link_deser(sd);
	struct device *dev = &priv->client->dev;
	int subdev_pad;
	int ret = 0;
	int num_sink_pad;

	dev_dbg(dev, "Get fmt for pad: %d", format->pad);

	num_sink_pad = (priv->metadata_pad) ? V_LINK_DESER_N_SINK_PADS : 1;

	/* If asked for sink pad format we simply return opposite source format */
	if (format->pad >= num_sink_pad) {
		format->pad = format->pad - num_sink_pad;
	}

	subdev_pad = v_link_deser_get_connected_pad(priv, format->pad);
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

static const struct v4l2_subdev_video_ops v_link_deser_video_ops = {
	.s_stream = v_link_deser_s_stream,
};

static const struct v4l2_subdev_pad_ops v_link_deser_pad_ops = {
	.get_fmt = v_link_deser_get_fmt,
	.set_fmt = v_link_deser_set_fmt,
};

static const struct v4l2_subdev_ops v_link_deser_subdev_ops = {
	.video = &v_link_deser_video_ops,
	.pad = &v_link_deser_pad_ops,
};

static const struct media_entity_operations v_link_deser_media_ops = {
	.link_validate = v4l2_subdev_link_validate
};

static int v_link_deser_setup(struct v_link_deser_priv *priv)
{
	struct device *dev = &priv->client->dev;
	struct v4l2_mbus_config_mipi_csi2 *mipi;
	unsigned int val, lane;
	unsigned long lanes_used = 0;
	int ret = 0;

	mipi = &priv->mipi_csi2;

	val = 0xc4;
	cci_write(priv->regmap, CCI_REG8(0x0332), val, &ret);

	val = div_u64(priv->link_freq * 2, MHZ(100));
	cci_update_bits(priv->regmap, MAX96714_BACKTOP25, CSI_DPLL_FREQ_MASK,
			val, &ret);

	val = FIELD_PREP(MAX96714_CSI2_LANE_CNT_MASK, mipi->num_data_lanes - 1);
	cci_update_bits(priv->regmap, MAX96714_MIPI_LANE_CNT,
			MAX96714_CSI2_LANE_CNT_MASK, val, &ret);

	/* Fixed values for this register based on HW design */
	val = 0x3b; //0x23;
	cci_write(priv->regmap, MAX96714_MIPI_POLARITY, val, &ret);

	/* lanes mapping */
	val = 0;
	for (lane = 0; lane < mipi->num_data_lanes; lane++) {
		val |= (mipi->data_lanes[lane] - 1) << (lane * 2);
		lanes_used |= BIT(mipi->data_lanes[lane] - 1);
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

static int v_link_deser_create_subdev(struct v_link_deser_priv *priv)
{
	struct device *dev = &priv->client->dev;
	int ret, num_pads;

	v4l2_i2c_subdev_init(&priv->sd, priv->client, &v_link_deser_subdev_ops);

	v4l2_ctrl_handler_init(&priv->ctrls, 1);
	priv->sd.ctrl_handler = &priv->ctrls;

	v4l2_ctrl_new_int_menu(&priv->ctrls, NULL, V4L2_CID_LINK_FREQ, 0, 0,
			       &priv->link_freq);

	if (priv->ctrls.error) {
		ret = priv->ctrls.error;
		goto err_free_ctrl;
	}

	priv->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	priv->sd.entity.function = MEDIA_ENT_F_VID_IF_BRIDGE;
	priv->sd.entity.ops = &v_link_deser_media_ops;

	if (priv->metadata_pad) {
		priv->pads[V_LINK_DESER_SRC_PAD0].flags = MEDIA_PAD_FL_SOURCE;
		priv->pads[V_LINK_DESER_SRC_PAD1].flags = MEDIA_PAD_FL_SOURCE;
		priv->pads[V_LINK_DESER_SINK_PAD1].flags = MEDIA_PAD_FL_SINK;
		priv->pads[V_LINK_DESER_SINK_PAD0].flags = MEDIA_PAD_FL_SINK;
	} else {
		priv->pads[0].flags = MEDIA_PAD_FL_SINK;
		priv->pads[1].flags = MEDIA_PAD_FL_SOURCE;
	}

	num_pads = (priv->metadata_pad) ? V_LINK_DESER_N_PADS :
					  V_LINK_DESER_N_PADS - 2;
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
	ret = v_link_deser_v4l2_notifier_register(priv);
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

static int v_link_deser_parse_dt_sink(struct v_link_deser_priv *priv)
{
	struct device *dev = &priv->client->dev;
	struct v_link_deser_source *rxport = &priv->source;
	struct fwnode_handle *ep_fwnode;
	struct v4l2_fwnode_endpoint vep = { .bus_type = V4L2_MBUS_CSI2_DPHY };
	int ret;

	ep_fwnode = fwnode_graph_get_endpoint_by_id(dev_fwnode(dev), 0, 0, 0);
	if (!ep_fwnode) {
		dev_err(dev, "Failed to get sink endpoint");
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

	return 0;
}

static int v_link_deser_parse_dt_source(struct v_link_deser_priv *priv)
{
	struct device *dev = &priv->client->dev;
	struct v4l2_fwnode_endpoint vep = { .bus_type = V4L2_MBUS_CSI2_DPHY };
	struct fwnode_handle *ep_fwnode;
	u32 num_data_lanes;
	int ret;

	ep_fwnode = fwnode_graph_get_endpoint_by_id(dev_fwnode(dev), 1, 0, 0);
	if (!ep_fwnode) {
		dev_err(dev, "Failed to get src endpoint");
		return -ENOENT;
	}

	ret = v4l2_fwnode_endpoint_alloc_parse(ep_fwnode, &vep);
	fwnode_handle_put(ep_fwnode);
	if (ret) {
		dev_err(dev, "Failed to parse endpoint data");
		return -EINVAL;
	}

	if (vep.nr_of_link_frequencies != 1) {
		ret = -EINVAL;
		dev_err(dev, "Invalid link frequency count (%d)",
			vep.nr_of_link_frequencies);
		goto err_free_vep;
	}

	priv->link_freq = vep.link_frequencies[0];

	/* Min 50MHz, Max 1250MHz, 50MHz step */
	if (priv->link_freq < MHZ(50) || priv->link_freq > MHZ(1250) ||
	    (u32)priv->link_freq % MHZ(50)) {
		dev_err(dev, "Invalid link frequency");
		ret = -EINVAL;
		goto err_free_vep;
	}

	num_data_lanes = vep.bus.mipi_csi2.num_data_lanes;
	if (num_data_lanes < 1 || num_data_lanes > 4) {
		dev_err(dev, "Invalid number of data lanes must be 1 to 4");
		ret = -EINVAL;
		goto err_free_vep;
	}

	priv->mipi_csi2 = vep.bus.mipi_csi2;

	dev_dbg(dev, "(source) link freq: %llu, num lanes: %u", priv->link_freq,
		num_data_lanes);

	return 0;

err_free_vep:
	v4l2_fwnode_endpoint_free(&vep);

	return ret;
}

static int v_link_deser_parse_dt(struct v_link_deser_priv *priv)
{
	struct device *dev = &priv->client->dev;
	int ret;

	ret = v_link_deser_parse_dt_sink(priv);
	if (ret)
		return ret;

	ret = v_link_deser_parse_dt_source(priv);
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

	ret = v_link_deser_i2c_mux_init(priv);
	if (ret) {
		dev_err(&client->dev, "Unable to initialize I2C multiplexer\n");
		goto err_disable;
	}

	ret = v_link_deser_setup(priv);
	if (ret) {
		dev_err(&client->dev, "Unable to setup max96714\n");
		goto err_del_mux;
	}

	ret = v_link_deser_create_subdev(priv);
	if (ret)
		goto err_del_mux;

	return 0;

err_del_mux:
	i2c_mux_del_adapters(priv->mux);
err_disable:
	fwnode_handle_put(priv->source.fwnode);
err_disable_core_hw:
	v_link_deser_disable_core_hw(priv);
	return ret;
}

static void v_link_deser_remove(struct i2c_client *client)
{
	struct v_link_deser_priv *priv =
		sd_to_v_link_deser(i2c_get_clientdata(client));

	v4l2_async_nf_unregister(&priv->notifier);
	v4l2_async_nf_cleanup(&priv->notifier);
	v4l2_async_unregister_subdev(&priv->sd);
	v4l2_subdev_cleanup(&priv->sd);
	media_entity_cleanup(&priv->sd.entity);
	v4l2_ctrl_handler_free(&priv->ctrls);
	i2c_mux_del_adapters(priv->mux);
	fwnode_handle_put(priv->source.fwnode);
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
