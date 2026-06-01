// SPDX-License-Identifier: GPL-2.0
/*
 * Simplified I2C driver for Sony IMX219
 *
 * Copyright (C) 2025 Spacemit Ltd.
 */
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/init.h>
#include <linux/ioctl.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/regulator/consumer.h>
#include <linux/slab.h>
#include <linux/types.h>

#include <media/media-entity.h>
#include <media/v4l2-async.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-subdev.h>

/*
 * Sensor Configuration: 1920x1080 @ 30fps, 2-lane MIPI
 *
 * MCLK:           24MHz
 * Resolution:     1920x1080
 * Bit Depth:      10bit
 * HTS:            3448 (0x0d78)
 * VTS:            1766 (0x06e6)
 */

#define IMX219_IOC_MAGIC		'I'
#define IMX219_IOCTL_POWER_ON		_IO(IMX219_IOC_MAGIC, 1)
#define IMX219_IOCTL_POWER_OFF		_IO(IMX219_IOC_MAGIC, 2)
#define IMX219_IOCTL_INIT_REGS		_IO(IMX219_IOC_MAGIC, 3)
#define IMX219_IOCTL_STREAM_ON		_IO(IMX219_IOC_MAGIC, 4)
#define IMX219_IOCTL_STREAM_OFF		_IO(IMX219_IOC_MAGIC, 5)
#define IMX219_IOCTL_DETECT		_IO(IMX219_IOC_MAGIC, 6)

#define IMX219_LINK_FREQ		456000000ULL
#define IMX219_PIXEL_RATE		182400000
#define IMX219_WIDTH			1920
#define IMX219_HEIGHT			1080
#define IMX219_HTS			3448
#define IMX219_VTS			1766
#define IMX219_EXPOSURE_MIN		4
#define IMX219_EXPOSURE_MAX		(IMX219_VTS - 4)
#define IMX219_EXPOSURE_DEF		0x640
#define IMX219_GAIN_MIN			0
#define IMX219_GAIN_MAX			232
#define IMX219_GAIN_DEF			0
#define IMX219_LINE_LENGTH_MAX		0x7ff0

static const s64 imx219_link_freq_menu[] = {
	IMX219_LINK_FREQ,
};

static struct imx219 *global_imx219;

struct imx219 {
	struct i2c_client *client;
	struct gpio_desc *pwdn;
	struct gpio_desc *i2c_mux;
	struct mutex lock;
	struct regulator *vdd;
	struct miscdevice miscdev;
	struct v4l2_subdev sd;
	struct media_pad pad;
	struct v4l2_ctrl_handler ctrl_handler;
	struct v4l2_ctrl *link_freq;
	struct v4l2_ctrl *exposure;
	struct v4l2_mbus_framefmt fmt;
	bool powered;
	bool streaming;
};

struct regval_list {
	u16 addr;
	u8 data;
};

static inline struct imx219 *to_imx219(struct v4l2_subdev *sd)
{
	return container_of(sd, struct imx219, sd);
}

static int imx219_write_init_regs(struct imx219 *sensor);
static int imx219_power_on(struct imx219 *sensor);
static void imx219_power_off(struct imx219 *sensor);
static int imx219_detect(struct imx219 *sensor);

static struct regval_list imx219_1080p_regs[] = {
	{0x30EB, 0x05},
	{0x30EB, 0x0C},
	{0x300A, 0xFF},
	{0x300B, 0xFF},
	{0x30EB, 0x05},
	{0x30EB, 0x09},
	{0x0114, 0x01},
	{0x0128, 0x00},
	{0x012A, 0x18},
	{0x012B, 0x00},
	{0x0160, 0x06},
	{0x0161, 0xE6},
	{0x0162, 0x0D},
	{0x0163, 0x78},
	{0x0164, 0x02},
	{0x0165, 0xA8},
	{0x0166, 0x0A},
	{0x0167, 0x27},
	{0x0168, 0x02},
	{0x0169, 0xB4},
	{0x016A, 0x06},
	{0x016B, 0xEB},
	{0x016C, 0x07},
	{0x016D, 0x80},
	{0x016E, 0x04},
	{0x016F, 0x38},
	{0x0170, 0x01},
	{0x0171, 0x01},
	{0x0174, 0x00},
	{0x0175, 0x00},
	{0x018C, 0x0A},
	{0x018D, 0x0A},
	{0x0301, 0x05},
	{0x0303, 0x01},
	{0x0304, 0x03},
	{0x0305, 0x03},
	{0x0306, 0x00},
	{0x0307, 0x39},
	{0x0309, 0x0A},
	{0x030B, 0x01},
	{0x030C, 0x00},
	{0x030D, 0x72},
	{0x455E, 0x00},
	{0x471E, 0x4B},
	{0x4767, 0x0F},
	{0x4750, 0x14},
	{0x4540, 0x00},
	{0x47B4, 0x14},
	{0x0100, 0x00},
};

static int imx219_write(struct imx219 *sensor, u16 reg, u8 val)
{
	struct i2c_adapter *adapter = sensor->client->adapter;
	struct i2c_msg msg;
	u8 data[3];
	int ret;

	data[0] = (reg >> 8) & 0xff;
	data[1] = reg & 0xff;
	data[2] = val & 0xff;

	msg.addr = sensor->client->addr;
	msg.flags = 0;
	msg.len = sizeof(data);
	msg.buf = data;

	mutex_lock(&sensor->lock);
	ret = i2c_transfer(adapter, &msg, 1);
	mutex_unlock(&sensor->lock);

	if (ret != 1) {
		dev_err(&sensor->client->dev,
			"imx219: I2C write failed, reg=0x%x, val=0x%x, ret=%d\n",
			reg, val, ret);
		return ret < 0 ? ret : -EIO;
	}

	return 0;
}

static int imx219_write16(struct imx219 *sensor, u16 reg, u16 val)
{
	int ret;

	ret = imx219_write(sensor, reg, (val >> 8) & 0xff);
	if (ret < 0)
		return ret;

	return imx219_write(sensor, reg + 1, val & 0xff);
}

static int imx219_read(struct imx219 *sensor, u16 reg, u8 *val)
{
	struct i2c_adapter *adapter = sensor->client->adapter;
	struct i2c_msg msgs[2];
	u8 reg_buf[2];
	u8 data_buf[1];
	int ret;

	reg_buf[0] = (reg >> 8) & 0xff;
	reg_buf[1] = reg & 0xff;

	msgs[0].addr = sensor->client->addr;
	msgs[0].flags = 0;
	msgs[0].len = sizeof(reg_buf);
	msgs[0].buf = reg_buf;

	msgs[1].addr = sensor->client->addr;
	msgs[1].flags = I2C_M_RD;
	msgs[1].len = sizeof(data_buf);
	msgs[1].buf = data_buf;

	mutex_lock(&sensor->lock);
	ret = i2c_transfer(adapter, msgs, 2);
	mutex_unlock(&sensor->lock);

	if (ret != 2) {
		dev_err(&sensor->client->dev,
			"imx219: I2C read failed, reg=0x%x, ret=%d\n",
			reg, ret);
		return ret < 0 ? ret : -EIO;
	}

	*val = data_buf[0];
	return 0;
}

static int imx219_stream_on(struct imx219 *sensor)
{
	return imx219_write(sensor, 0x0100, 0x01);
}

static int imx219_stream_off(struct imx219 *sensor)
{
	return imx219_write(sensor, 0x0100, 0x00);
}

static int imx219_set_exposure(struct imx219 *sensor, u32 exposure)
{
	return imx219_write16(sensor, 0x015a, exposure);
}

static int imx219_set_analogue_gain(struct imx219 *sensor, u32 gain)
{
	return imx219_write(sensor, 0x0157, gain & 0xff);
}

static int imx219_set_vblank(struct imx219 *sensor, u32 vblank)
{
	return imx219_write16(sensor, 0x0160, IMX219_HEIGHT + vblank);
}

static int imx219_set_hblank(struct imx219 *sensor, u32 hblank)
{
	return imx219_write16(sensor, 0x0162, IMX219_WIDTH + hblank);
}

static int imx219_set_ctrl(struct v4l2_ctrl *ctrl)
{
	struct imx219 *sensor =
		container_of(ctrl->handler, struct imx219, ctrl_handler);
	int exposure_max;

	if (ctrl->id == V4L2_CID_VBLANK) {
		exposure_max = IMX219_HEIGHT + ctrl->val - 4;
		__v4l2_ctrl_modify_range(sensor->exposure,
					 sensor->exposure->minimum,
					 exposure_max, sensor->exposure->step,
					 min(IMX219_EXPOSURE_DEF, exposure_max));
	}

	if (!sensor->powered)
		return 0;

	switch (ctrl->id) {
	case V4L2_CID_EXPOSURE:
		return imx219_set_exposure(sensor, ctrl->val);
	case V4L2_CID_ANALOGUE_GAIN:
		return imx219_set_analogue_gain(sensor, ctrl->val);
	case V4L2_CID_VBLANK:
		return imx219_set_vblank(sensor, ctrl->val);
	case V4L2_CID_HBLANK:
		return imx219_set_hblank(sensor, ctrl->val);
	default:
		return 0;
	}
}

static const struct v4l2_ctrl_ops imx219_ctrl_ops = {
	.s_ctrl = imx219_set_ctrl,
};

static int imx219_init_controls(struct imx219 *sensor)
{
	struct v4l2_ctrl_handler *hdl = &sensor->ctrl_handler;
	u32 hblank = IMX219_HTS - IMX219_WIDTH;
	u32 vblank = IMX219_VTS - IMX219_HEIGHT;
	int ret;

	v4l2_ctrl_handler_init(hdl, 6);
	v4l2_ctrl_new_std(hdl, &imx219_ctrl_ops, V4L2_CID_PIXEL_RATE,
			  IMX219_PIXEL_RATE, IMX219_PIXEL_RATE, 1,
			  IMX219_PIXEL_RATE);
	sensor->link_freq = v4l2_ctrl_new_int_menu(hdl, &imx219_ctrl_ops,
						   V4L2_CID_LINK_FREQ, 0, 0,
						   imx219_link_freq_menu);
	if (sensor->link_freq)
		sensor->link_freq->flags |= V4L2_CTRL_FLAG_READ_ONLY;
	v4l2_ctrl_new_std(hdl, &imx219_ctrl_ops, V4L2_CID_HBLANK,
			  hblank, IMX219_LINE_LENGTH_MAX - IMX219_WIDTH, 1,
			  hblank);
	v4l2_ctrl_new_std(hdl, &imx219_ctrl_ops, V4L2_CID_VBLANK,
			  32, 0xffff - IMX219_HEIGHT, 1, vblank);
	sensor->exposure = v4l2_ctrl_new_std(hdl, &imx219_ctrl_ops,
					     V4L2_CID_EXPOSURE,
					     IMX219_EXPOSURE_MIN,
					     IMX219_EXPOSURE_MAX, 1,
					     IMX219_EXPOSURE_DEF);
	v4l2_ctrl_new_std(hdl, &imx219_ctrl_ops, V4L2_CID_ANALOGUE_GAIN,
			  IMX219_GAIN_MIN, IMX219_GAIN_MAX, 1,
			  IMX219_GAIN_DEF);

	if (hdl->error) {
		ret = hdl->error;
		v4l2_ctrl_handler_free(hdl);
		return ret;
	}

	sensor->sd.ctrl_handler = hdl;
	return 0;
}

static int imx219_s_stream(struct v4l2_subdev *sd, int enable)
{
	struct imx219 *sensor = to_imx219(sd);
	int ret = 0;

	if (enable) {
		if (!sensor->powered) {
			ret = imx219_power_on(sensor);
			if (ret)
				return ret;

			ret = imx219_write_init_regs(sensor);
			if (ret)
				goto err_power;

			ret = v4l2_ctrl_handler_setup(&sensor->ctrl_handler);
			if (ret)
				goto err_power;
		}

		ret = imx219_stream_on(sensor);
		if (!ret)
			sensor->streaming = true;
	} else {
		if (sensor->streaming)
			ret = imx219_stream_off(sensor);
		sensor->streaming = false;
		if (sensor->powered)
			imx219_power_off(sensor);
	}

	return ret;

err_power:
	imx219_power_off(sensor);
	return ret;
}

static int imx219_enum_mbus_code(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *state,
				 struct v4l2_subdev_mbus_code_enum *code)
{
	if (code->index)
		return -EINVAL;

	code->code = MEDIA_BUS_FMT_SRGGB10_1X10;
	return 0;
}

static int imx219_get_fmt(struct v4l2_subdev *sd,
			  struct v4l2_subdev_state *state,
			  struct v4l2_subdev_format *fmt)
{
	struct imx219 *sensor = to_imx219(sd);

	fmt->format = sensor->fmt;
	return 0;
}

static int imx219_set_fmt(struct v4l2_subdev *sd,
			  struct v4l2_subdev_state *state,
			  struct v4l2_subdev_format *fmt)
{
	struct imx219 *sensor = to_imx219(sd);

	fmt->format.code = MEDIA_BUS_FMT_SRGGB10_1X10;
	fmt->format.width = IMX219_WIDTH;
	fmt->format.height = IMX219_HEIGHT;
	fmt->format.field = V4L2_FIELD_NONE;
	fmt->format.colorspace = V4L2_COLORSPACE_RAW;

	if (fmt->which == V4L2_SUBDEV_FORMAT_ACTIVE)
		sensor->fmt = fmt->format;

	return 0;
}

static int imx219_enum_frame_size(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *state,
				  struct v4l2_subdev_frame_size_enum *fse)
{
	if (fse->index || fse->code != MEDIA_BUS_FMT_SRGGB10_1X10)
		return -EINVAL;

	fse->min_width = IMX219_WIDTH;
	fse->max_width = IMX219_WIDTH;
	fse->min_height = IMX219_HEIGHT;
	fse->max_height = IMX219_HEIGHT;
	return 0;
}

static const struct v4l2_subdev_video_ops imx219_video_ops = {
	.s_stream = imx219_s_stream,
};

static const struct v4l2_subdev_pad_ops imx219_pad_ops = {
	.enum_mbus_code = imx219_enum_mbus_code,
	.get_fmt = imx219_get_fmt,
	.set_fmt = imx219_set_fmt,
	.enum_frame_size = imx219_enum_frame_size,
};

static const struct v4l2_subdev_ops imx219_subdev_ops = {
	.video = &imx219_video_ops,
	.pad = &imx219_pad_ops,
};

static long imx219_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct imx219 *sensor = file->private_data;
	int ret = 0;

	if (!sensor)
		return -ENODEV;

	switch (cmd) {
	case IMX219_IOCTL_POWER_ON:
		ret = imx219_power_on(sensor);
		break;
	case IMX219_IOCTL_POWER_OFF:
		imx219_power_off(sensor);
		break;
	case IMX219_IOCTL_INIT_REGS:
		ret = imx219_write_init_regs(sensor);
		break;
	case IMX219_IOCTL_STREAM_ON:
		ret = imx219_stream_on(sensor);
		if (!ret)
			sensor->streaming = true;
		break;
	case IMX219_IOCTL_STREAM_OFF:
		ret = imx219_stream_off(sensor);
		if (!ret)
			sensor->streaming = false;
		break;
	case IMX219_IOCTL_DETECT:
		ret = imx219_detect(sensor);
		break;
	default:
		ret = -EINVAL;
	}

	return ret;
}

static int imx219_dev_open(struct inode *inode, struct file *file)
{
	struct miscdevice *misc = file->private_data;
	struct imx219 *sensor;

	if (!misc)
		return -ENODEV;

	sensor = container_of(misc, struct imx219, miscdev);
	file->private_data = sensor;
	return 0;
}

static const struct file_operations imx219_fops = {
	.owner = THIS_MODULE,
	.open = imx219_dev_open,
	.unlocked_ioctl = imx219_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = imx219_ioctl,
#endif
};

static int imx219_write_init_regs(struct imx219 *sensor)
{
	int i, ret;

	dev_info(&sensor->client->dev, "imx219: write init regs, total=%zu\n",
		 ARRAY_SIZE(imx219_1080p_regs));

	for (i = 0; i < ARRAY_SIZE(imx219_1080p_regs); i++) {
		const struct regval_list *reg = &imx219_1080p_regs[i];

		ret = imx219_write(sensor, reg->addr, reg->data);
		if (ret < 0) {
			dev_err(&sensor->client->dev,
				"imx219: write init reg failed, idx=%d, reg=0x%x, val=0x%x, ret=%d\n",
				i, reg->addr, reg->data, ret);
			return ret;
		}
	}

	return 0;
}

static int imx219_detect(struct imx219 *sensor)
{
	u8 hi, lo;
	int ret;

	ret = imx219_read(sensor, 0x0000, &hi);
	if (ret < 0)
		return ret;
	if ((hi & 0x0f) != 0x02) {
		dev_err(&sensor->client->dev, "imx219: ID high mismatch: 0x%x\n",
			hi);
		return -ENODEV;
	}

	ret = imx219_read(sensor, 0x0001, &lo);
	if (ret < 0)
		return ret;
	if (lo != 0x19) {
		dev_err(&sensor->client->dev, "imx219: ID low mismatch: 0x%x\n",
			lo);
		return -ENODEV;
	}

	dev_info(&sensor->client->dev, "imx219: detected IMX219 (id %02x%02x)\n",
		 hi, lo);
	return 0;
}

static int imx219_power_on(struct imx219 *sensor)
{
	int ret;

	if (sensor->powered)
		return 0;

	if (sensor->i2c_mux)
		gpiod_set_value_cansleep(sensor->i2c_mux, 1);

	if (sensor->vdd) {
		ret = regulator_enable(sensor->vdd);
		if (ret < 0)
			return ret;

		/*
		 * Some board regulators are fixed or policy-managed and reject
		 * runtime voltage changes. The DTS supply already describes the
		 * rail, so do not fail probe only because set_voltage is denied.
		 */
		ret = regulator_set_voltage(sensor->vdd, 3300000, 3300000);
		if (ret < 0)
			dev_warn(&sensor->client->dev,
				 "imx219: failed to set vdd voltage: %d, keep current voltage\n",
				 ret);
	}

	if (sensor->pwdn)
		gpiod_set_value_cansleep(sensor->pwdn, 0);

	usleep_range(10000, 12000);

	if (sensor->pwdn)
		gpiod_set_value_cansleep(sensor->pwdn, 1);

	usleep_range(30000, 31000);
	sensor->powered = true;
	return 0;
}

static void imx219_power_off(struct imx219 *sensor)
{
	if (!sensor->powered)
		return;

	if (sensor->pwdn)
		gpiod_set_value_cansleep(sensor->pwdn, 0);

	if (sensor->vdd)
		regulator_disable(sensor->vdd);

	if (sensor->i2c_mux)
		gpiod_set_value_cansleep(sensor->i2c_mux, 0);

	sensor->powered = false;
}

static int imx219_probe(struct i2c_client *client)
{
	struct imx219 *sensor;
	struct device *dev = &client->dev;
	int ret;

	dev_info(dev, "imx219: probe enter, client addr=0x%x\n", client->addr);

	sensor = devm_kzalloc(dev, sizeof(*sensor), GFP_KERNEL);
	if (!sensor)
		return -ENOMEM;

	sensor->client = client;
	mutex_init(&sensor->lock);
	i2c_set_clientdata(client, sensor);
	sensor->fmt.code = MEDIA_BUS_FMT_SRGGB10_1X10;
	sensor->fmt.width = IMX219_WIDTH;
	sensor->fmt.height = IMX219_HEIGHT;
	sensor->fmt.field = V4L2_FIELD_NONE;
	sensor->fmt.colorspace = V4L2_COLORSPACE_RAW;

	sensor->pwdn = devm_gpiod_get_optional(
		dev, "pwdn", GPIOD_OUT_LOW | GPIOD_FLAGS_BIT_NONEXCLUSIVE);
	if (IS_ERR(sensor->pwdn)) {
		ret = PTR_ERR(sensor->pwdn);
		goto err_mutex;
	}

	sensor->i2c_mux = devm_gpiod_get_optional(dev, "i2c-mux",
		GPIOD_OUT_LOW | GPIOD_FLAGS_BIT_NONEXCLUSIVE);
	if (IS_ERR(sensor->i2c_mux)) {
		dev_warn(dev, "imx219: no i2c-mux GPIO, continuing without it\n");
		sensor->i2c_mux = NULL;
	}

	sensor->vdd = devm_regulator_get(dev, "vdd");
	if (IS_ERR(sensor->vdd)) {
		ret = PTR_ERR(sensor->vdd);
		dev_err_probe(dev, ret, "imx219: failed to get vdd regulator\n");
		goto err_mutex;
	}

	ret = imx219_power_on(sensor);
	if (ret) {
		dev_err(dev, "imx219: power on failed: %d\n", ret);
		goto err_mutex;
	}

	ret = imx219_detect(sensor);
	if (ret) {
		dev_err(dev, "imx219: sensor detect failed: %d\n", ret);
		goto err_power;
	}

	imx219_power_off(sensor);

	v4l2_i2c_subdev_init(&sensor->sd, client, &imx219_subdev_ops);
	sensor->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	sensor->sd.entity.function = MEDIA_ENT_F_CAM_SENSOR;
	sensor->pad.flags = MEDIA_PAD_FL_SOURCE;

	ret = media_entity_pads_init(&sensor->sd.entity, 1, &sensor->pad);
	if (ret) {
		dev_err(dev, "imx219: failed to init media pads: %d\n", ret);
		goto err_mutex;
	}

	ret = imx219_init_controls(sensor);
	if (ret) {
		dev_err(dev, "imx219: failed to init controls: %d\n", ret);
		goto err_media_entity;
	}

	ret = v4l2_async_register_subdev(&sensor->sd);
	if (ret) {
		dev_err(dev, "imx219: failed to register subdev: %d\n", ret);
		goto err_ctrls;
	}

	sensor->miscdev.minor = MISC_DYNAMIC_MINOR;
	sensor->miscdev.fops = &imx219_fops;
	sensor->miscdev.parent = dev;
	if (client->dev.of_node) {
		u32 csi_id;

		if (!of_property_read_u32(client->dev.of_node, "csi-id",
					  &csi_id))
			sensor->miscdev.name = devm_kasprintf(
				dev, GFP_KERNEL, "imx219-%u", csi_id);
		else
			sensor->miscdev.name = devm_kasprintf(
				dev, GFP_KERNEL, "imx219-%02x", client->addr);
	} else {
		sensor->miscdev.name = devm_kasprintf(
			dev, GFP_KERNEL, "imx219-%02x", client->addr);
	}

	if (!sensor->miscdev.name) {
		ret = -ENOMEM;
		goto err_subdev;
	}

	ret = misc_register(&sensor->miscdev);
	if (ret) {
		dev_err(dev, "imx219: failed to register misc device: %d\n",
			ret);
		goto err_subdev;
	}

	global_imx219 = sensor;
	dev_info(dev, "imx219: probe successful, ioctl device /dev/%s\n",
		 sensor->miscdev.name);
	return 0;

err_subdev:
	v4l2_async_unregister_subdev(&sensor->sd);
err_ctrls:
	v4l2_ctrl_handler_free(&sensor->ctrl_handler);
err_media_entity:
	media_entity_cleanup(&sensor->sd.entity);
err_power:
	imx219_power_off(sensor);
err_mutex:
	mutex_destroy(&sensor->lock);
	return ret;
}

static void imx219_remove(struct i2c_client *client)
{
	struct imx219 *sensor = i2c_get_clientdata(client);

	if (global_imx219 == sensor)
		global_imx219 = NULL;

	misc_deregister(&sensor->miscdev);
	v4l2_async_unregister_subdev(&sensor->sd);
	v4l2_ctrl_handler_free(&sensor->ctrl_handler);
	media_entity_cleanup(&sensor->sd.entity);
	imx219_power_off(sensor);
	mutex_destroy(&sensor->lock);
}

static const struct of_device_id imx219_of_match[] = {
	{ .compatible = "sony,imx219" },
	{}
};
MODULE_DEVICE_TABLE(of, imx219_of_match);

static struct i2c_driver imx219_driver = {
	.driver = {
		.name		= "imx219-simple",
		.of_match_table	= of_match_ptr(imx219_of_match),
	},
	.probe		= imx219_probe,
	.remove		= imx219_remove,
};

module_i2c_driver(imx219_driver)

MODULE_DESCRIPTION("Simplified V4L2 subdev driver for IMX219 on K3");
MODULE_LICENSE("GPL v2");
