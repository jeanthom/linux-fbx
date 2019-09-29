#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/input-polldev.h>
#include <linux/smsc_cap1066.h>
#include <linux/slab.h>
#include <linux/gpio.h>

#define PFX		"cap1066: "

/*
 * list of registers
 */
#define SMSC_REG_MAIN_CTRL		0x00
#define SMSC_REG_BTN_STATUS1		0x03
#define SMSC_REG_DATA_SENSITIVITY	0x1f
#define SMSC_REG_CFG			0x20
#define SMSC_REG_SENS_CFG		0x22
#define SMSC_REG_MTOUCH_CFG_REG		0x2a
#define SMSC_REG_CFG2			0x44
#define SMSC_REG_LED_OUT_TYPE		0x71
#define SMSC_REG_LED_LINK		0x72
#define SMSC_REG_LED_OUTPUT_CTL		0x74
#define SMSC_REG_LED_BEHAVIOUR1		0x81
#define SMSC_REG_LED_BEHAVIOUR2		0x82
#define SMSC_REG_LED_DIRECT_DCYCLE	0x93
#define SMSC_REG_LED_DIRECT_RAMP_RATE	0x94
#define SMSC_REG_LED_OFF_DELAY		0x95
#define SMSC_REG_DID			0xfd
#define SMSC_REG_VID			0xfe

/*
 * used in cap1066_init_hw and during priv initialization.
 */
#define DEFAULT_DUTY_CYCLE_MIN		0x4
#define DEFAULT_DUTY_CYCLE_MAX		0xf
#define DEFAULT_RAMP_TIME_FALL		0x1
#define DEFAULT_RAMP_TIME_RISE		0x2

/*
 * vendor id / device id
 */
#define SMSC_CAP1066_VID	0x5d
#define SMSC_CAP1066_DID	0x41
#define SMSC_CAP1166_DID	0x51

static const unsigned short normal_i2c[] = { 0x28, I2C_CLIENT_END };

static const struct i2c_device_id cap1066_id[] = {
	{ "cap1066", 0 },
	{ }
};

/*
 * private context
 */
static unsigned short default_map[CAP1066_MAX_BTNS] = {
	BTN_0,
	BTN_1,
	BTN_2,
	BTN_3,
	BTN_4,
	BTN_5,
};

struct led_btn_name
{
	int code;
	const char *name;
};

/*
 * whenever possible symlinks will be created from led_btn_X to
 * led_key_y, depending on user provided keymap. add entries here as
 * you see fit.
 */
static const struct led_btn_name led_btn_names[] = {
	{ KEY_UP, "led_key_up", },
	{ KEY_DOWN, "led_key_down", },
	{ KEY_LEFT, "led_key_left", },
	{ KEY_RIGHT, "led_key_right", },
	{ KEY_ENTER, "led_key_enter", },
};

enum {
	E_SMSC_CAP1066_LED_MODE_AUTO,
	E_SMSC_CAP1066_LED_MODE_ON,
	E_SMSC_CAP1066_LED_MODE_OFF,
};

struct cap1066_led_dev
{
	struct cap1066_priv	*parent_priv;
	struct device		dev;
	int			led_mode;
	int			led_index;
	const char		*btn_link;
};

struct cap1066_priv {
	struct input_polled_dev *poll_dev;
	struct i2c_client	*client;
	unsigned short		keymap[CAP1066_MAX_BTNS];
	struct cap1066_led_dev	*led_devices[CAP1066_MAX_BTNS];

	u8			duty_cycle_min;
	u8			duty_cycle_max;
	u8			raw_ramp_time_fall;
	u8			raw_ramp_time_rise;

	bool			has_irq_gpio;
	unsigned int		irq_gpio;
};

static const char *get_keycode_btn_name(int key_code)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(led_btn_names); ++i) {
		if (key_code == led_btn_names[i].code)
			return led_btn_names[i].name;
	}
	return NULL;
}

/*
 * single register read
 */
static int cap1066_read_reg(struct i2c_client *client, u8 reg, u8 *val)
{
	int ret;

	ret = i2c_smbus_read_byte_data(client, reg);
	if (ret < 0) {
		printk(KERN_ERR PFX "read failed: %d\n", ret);
		return ret;
	}

	*val = (u8)ret;
	return 0;
}

/*
 * single register write
 */
static int cap1066_write_reg(struct i2c_client *client, u8 reg, u8 val)
{
	int ret;

	ret = i2c_smbus_write_byte_data(client, reg, val);
	if (ret < 0) {
		printk(KERN_ERR PFX "write failed: %d\n", ret);
		return ret;
	}
	return 0;
}

/*
 * called when an smbus device is detected, make sure it's a cap1066
 */
static int cap1066_detect(struct i2c_client *client,
			  struct i2c_board_info *info)

{
	int ret;
	u8 vid, did;
	const char *name = NULL;

	ret = cap1066_read_reg(client, SMSC_REG_VID, &vid);
	if (ret)
		return ret;

	ret = cap1066_read_reg(client, SMSC_REG_DID, &did);
	if (ret)
		return ret;

	if (vid != SMSC_CAP1066_VID)
		goto no_dev;

	switch (did) {
	case SMSC_CAP1066_DID:
		name = "cap1066";
		break;
	case SMSC_CAP1166_DID:
		name = "cap1166";
		break;
	default:
		goto no_dev;
	}

	printk(KERN_INFO PFX "detected SMSC %s chip\n", name);
	if (info)
		strlcpy(info->type, name, I2C_NAME_SIZE);
	return 0;

no_dev:
	printk(KERN_ERR PFX "bad vid/did: 0x%04x/0x%04x\n", vid, did);
	return -ENODEV;
}

/*
 * reset registers value
 */
static int cap1066_init_hw(struct i2c_client *client)
{
	unsigned int i;
	u8 did;
	int ret;

	static const u8 init_regs[] = {
		/* power on */
		SMSC_REG_MAIN_CTRL, 0x0,

		/* default sensitivity */
		SMSC_REG_DATA_SENSITIVITY, 0x2f,

		/* max duration */
		SMSC_REG_SENS_CFG, 0xf4,

		/* default configuration */
		SMSC_REG_CFG, 0x38,

		/* open drain output on all gpios */
		SMSC_REG_LED_OUT_TYPE, 0x00,

		/* link leds with sensors */
		SMSC_REG_LED_LINK, 0x3f,

		/* setup direct mode */
		SMSC_REG_LED_BEHAVIOUR1, 0x00,
		SMSC_REG_LED_BEHAVIOUR2, 0x00,

		/* set led duty cycle min/max to 10% => 100% */
		SMSC_REG_LED_DIRECT_DCYCLE,
			(DEFAULT_DUTY_CYCLE_MAX << 4) |
			(DEFAULT_DUTY_CYCLE_MIN),

		/* set ramp rate time to 500ms/250ms */
		SMSC_REG_LED_DIRECT_RAMP_RATE,
			(DEFAULT_RAMP_TIME_RISE << 3) |
			(DEFAULT_RAMP_TIME_FALL),
	};

	static const u8 init_cap11_regs[] = {
		/* default configuration2 */
		SMSC_REG_CFG2, 0x44,
	};

	for (i = 0; i < ARRAY_SIZE(init_regs); i += 2) {
		int ret;

		ret = cap1066_write_reg(client,
					init_regs[i], init_regs[i + 1]);
		if (ret)
			return ret;
	}

	ret = cap1066_read_reg(client, SMSC_REG_DID, &did);
	if (ret)
		return ret;

	if (did != SMSC_CAP1166_DID)
		return 0;

	for (i = 0; i < ARRAY_SIZE(init_cap11_regs); i += 2) {
		int ret;

		ret = cap1066_write_reg(client,
					init_cap11_regs[i],
					init_cap11_regs[i + 1]);
		if (ret)
			return ret;
	}

	return 0;
}

/*
 * input core poll callback
 */
static void cap1066_input_poll(struct input_polled_dev *dev)
{
	struct cap1066_priv *priv = dev->private;
	struct input_dev *input = dev->input;
	unsigned int i;
	int ret;
	u8 stat;

	if (priv->has_irq_gpio) {
		if (gpio_get_value(priv->irq_gpio))
			return;
	}

	/* clear interrupt flag */
	cap1066_write_reg(priv->client, SMSC_REG_MAIN_CTRL, 0);

	ret = cap1066_read_reg(priv->client, SMSC_REG_BTN_STATUS1, &stat);
	if (ret) {
		printk(KERN_ERR PFX "unable to read status\n");
		return;
	}

	for (i = 0; i < CAP1066_MAX_BTNS; i++)
		input_report_key(input, priv->keymap[i],
				 (stat & (1 << i)) ? 1 : 0);
	input_sync(input);
}

#define to_cap1066_led_dev(Dev)	container_of(Dev, struct cap1066_led_dev, dev)

/*
 * called when all sysfs references to the cap1066_led_dev are gone.
 */
static void cap1066_led_dev_release(struct device *dev)
{
	struct cap1066_led_dev *led_dev;

	led_dev = to_cap1066_led_dev(dev);
	kfree(led_dev);
}

static int is_white(int c)
{
	return c == '\0' || c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

/*
 * store a new control value for a given cap1066_led_dev:
 * - auto means that the led is linked to the capacitive keys
 * - on means that the led is always on
 * - off means that the led is always off
 */
static ssize_t store_control(struct device *dev,
			     struct device_attribute *attr,
			     const char *buf, size_t count)
{
	struct cap1066_led_dev *led_dev;
	int new_mode;
	int read_len = 0;
	const char *end;
	int key;
	u8 reg;
	struct i2c_client *client;
	static const char *valid_keys[] = {
		[E_SMSC_CAP1066_LED_MODE_AUTO] = "auto",
		[E_SMSC_CAP1066_LED_MODE_ON] = "on",
		[E_SMSC_CAP1066_LED_MODE_OFF] = "off",
	};


	led_dev = to_cap1066_led_dev(dev);
	client = led_dev->parent_priv->client;

	if (is_white(*buf))
		/*
		 * eat white spaces silently, upper layer will call us
		 * again.
		 */
		return 1;

	for (key = 0; key < ARRAY_SIZE(valid_keys); ++key) {
		if (count < strlen(valid_keys[key]))
			continue ;
		if (!strncmp(buf, valid_keys[key], strlen(valid_keys[key]))) {
			break;
		}
	}

	if (key == ARRAY_SIZE(valid_keys)) {
		/*
		 * end of valid_keys array reached and nothing valid
		 * was recognized.
		 */
		printk(KERN_ERR PFX "invalid control value.\n");
		return -EINVAL;
	}
	new_mode = key;
	read_len = strlen(valid_keys[key]);

	/*
	 * check that no garbage is present at end of input.
	 */
	end = buf + read_len;
	if (end < buf + count && !is_white(*end)) {
		/*
		 * garbage at end of input.
		 */
		printk(KERN_ERR PFX "garbage at end of value for led "
		       "control.\n");
		return -EINVAL;
	}

	if (new_mode == led_dev->led_mode)
		return read_len;

	if (new_mode == E_SMSC_CAP1066_LED_MODE_AUTO) {
		cap1066_read_reg(client, SMSC_REG_LED_LINK, &reg);
		reg |= (1 << led_dev->led_index);
		cap1066_write_reg(client, SMSC_REG_LED_LINK, reg);
	} else {
		cap1066_read_reg(client, SMSC_REG_LED_LINK, &reg);
		reg &= ~(1 << led_dev->led_index);
		cap1066_write_reg(client, SMSC_REG_LED_LINK, reg);

		cap1066_read_reg(client, SMSC_REG_LED_OUTPUT_CTL, &reg);
		if (new_mode == E_SMSC_CAP1066_LED_MODE_ON)
			reg |= (1 << led_dev->led_index);
		else
			reg &= ~(1 << led_dev->led_index);
		cap1066_write_reg(client, SMSC_REG_LED_OUTPUT_CTL, reg);
	}
	led_dev->led_mode = new_mode;

	return read_len;
}

static ssize_t show_control(struct device *dev,
			    struct device_attribute *attr, char *buf)
{
	struct cap1066_led_dev *led_dev;
	const char *str;

	led_dev = to_cap1066_led_dev(dev);
	switch (led_dev->led_mode) {
	case E_SMSC_CAP1066_LED_MODE_AUTO:
		str = "auto";
		break;

	case E_SMSC_CAP1066_LED_MODE_ON:
		str = "on";
		break;

	case E_SMSC_CAP1066_LED_MODE_OFF:
		str = "off";
		break;

	default:
		str = "invalid";
		break;
	}

	return sprintf(buf, "%s\n", str);
}

static DEVICE_ATTR(control, S_IWUSR | S_IRUSR, show_control, store_control);

static struct device_attribute *cap1066_led_dev_attrs[] = {
	&dev_attr_control,
};

/*
 * helper used to create all attributes given in the attr array.
 *
 * if something goes wrong during creation, remove attributes that
 * have already been created.
 */
static int create_sysfs_files(struct device *dev,
			      struct device_attribute **attrs,
			      size_t count)
{
	int created;
	int error = 0;

	for (created = 0; created < count; ++created) {
		error = device_create_file(dev, attrs[created]);
		if (error)
			break;
	}

	if (!error)
		/*
		 * no errors, can return.
		 */
		return 0;

	/*
	 * errors during creation, remove already created
	 * files.
	 */
	while (--created >= 0)
		device_remove_file(dev, attrs[created]);

	return error;
}

/*
 * create a led device. This will create a new directory in the sysfs
 * base of the parent. a symlink will be created if a button name is
 * found via get_keycode_btn_name().
 */
static struct cap1066_led_dev *cap1066_create_led_dev(struct device *parent,
						      struct cap1066_priv *priv,
						      int index, int key_code)
{
	struct cap1066_led_dev *dev;
	int error = 0;

	dev = kzalloc(sizeof (*dev), GFP_KERNEL);
	if (!dev)
		return NULL;

	dev->led_index = index;
	dev->parent_priv = priv;
	dev_set_name(&dev->dev, "led_btn_%i", index);
	dev->dev.release = cap1066_led_dev_release;
	dev->dev.parent = parent;
	if (device_register(&dev->dev) < 0) {
		kfree(dev);
		return NULL;
	}

	/*
	 * create sysfs attributes.
	 */
	error = create_sysfs_files(&dev->dev, cap1066_led_dev_attrs,
				   ARRAY_SIZE(cap1066_led_dev_attrs));
	if (error) {
		device_unregister(&dev->dev);
		return NULL;
	}

	/*
	 * create sysfs symlinks to friendly names, wherever possible.
	 */
	dev->btn_link = get_keycode_btn_name(key_code);
	if (dev->btn_link) {
		error = sysfs_create_link(&parent->kobj, &dev->dev.kobj,
					  dev->btn_link);
		if (error)
			dev->btn_link = NULL;
	}
	return dev;
}

static void cap1066_remove_led_dev(struct cap1066_led_dev *dev)
{
	int i;

	if (dev->btn_link)
		sysfs_remove_link(&dev->dev.parent->kobj, dev->btn_link);

	for (i = 0; i < ARRAY_SIZE(cap1066_led_dev_attrs); ++i)
		device_remove_file(&dev->dev, cap1066_led_dev_attrs[i]);
	device_unregister(&dev->dev);

	/*
	 * dev->release() kfree the cap1066_led_dev struct
	 */
}

/*
 * helper to exctract an unsigned long from the buffer given in
 * parameter.
 *
 * first store buf in a zero terminated string and strtoul() it.
 */
static int get_ulong(const char *buf, size_t count, unsigned long *ret)
{
	char local_buf[32];
	unsigned long val;
	const char *end;

	strncpy(local_buf, buf, min(count , sizeof (local_buf)));
	local_buf[min(count, sizeof (local_buf) - 1)] = 0;

	val = simple_strtoul(local_buf, (char**)&end, 0);
	if (!is_white(*end))
		/*
		 * garbage after end of input.
		 */
		return -EINVAL;

	*ret = val;

	return 0;
}

/*
 * duty cycle sysfs callbacks: things may not work as expected if
 * duty_cycle_min is >= duty_cycle_max.
 *
 * values that can be written in duty_cycle_max/duty_cycle_min
 * attributes can be on the range [0, 16 [.
 *
 * 0 means the lowest possible pwm duty cycle.
 * 1 means the highest possible pwm duty cycle.
 */

static ssize_t store_duty_cycle_min(struct device *dev,
				    struct device_attribute *attr,
				    const char *buf, size_t count)
{
	unsigned long val;
	struct i2c_client *client;
	struct cap1066_priv *priv;
	int error;
	u8 reg;

	client = to_i2c_client(dev);
	priv = i2c_get_clientdata(client);

	if (is_white(*buf))
		return 1;

	error = get_ulong(buf, count, &val);
	if (error)
		return error;

	if (val > 0xf)
		return -ERANGE;

	cap1066_read_reg(client, SMSC_REG_LED_DIRECT_DCYCLE, &reg);
	reg &= ~0xf;
	reg |= val;
	cap1066_write_reg(client, SMSC_REG_LED_DIRECT_DCYCLE, reg);

	priv->duty_cycle_min = val;

	pr_debug(PFX "store_duty_cycle_min: reg = 0x%02x\n", reg);
	return count;
}

static ssize_t show_duty_cycle_min(struct device *dev,
				   struct device_attribute *attr,
				   char *buf)
{
	struct i2c_client *client;
	struct cap1066_priv *priv;

	client = to_i2c_client(dev);
	priv = i2c_get_clientdata(client);

	return sprintf(buf, "%u\n", priv->duty_cycle_min);
}

static ssize_t store_duty_cycle_max(struct device *dev,
				    struct device_attribute *attr,
				    const char *buf, size_t count)
{
	unsigned long val;
	struct i2c_client *client;
	struct cap1066_priv *priv;
	int error;
	u8 reg;

	client = to_i2c_client(dev);
	priv = i2c_get_clientdata(client);

	if (is_white(*buf))
		return 1;

	error = get_ulong(buf, count, &val);
	if (error)
		return error;

	if (val > 0xf)
		return -ERANGE;

	cap1066_read_reg(client, SMSC_REG_LED_DIRECT_DCYCLE, &reg);
	reg &= ~0xf0;
	reg |= val << 4;
	cap1066_write_reg(client, SMSC_REG_LED_DIRECT_DCYCLE, reg);

	priv->duty_cycle_max = val;

	pr_debug(PFX "store_duty_cycle_max: reg = 0x%02x\n", reg);
	return count;
}

static ssize_t show_duty_cycle_max(struct device *dev,
				   struct device_attribute *attr,
				   char *buf)
{
	struct i2c_client *client;
	struct cap1066_priv *priv;

	client = to_i2c_client(dev);
	priv = i2c_get_clientdata(client);

	return sprintf(buf, "%u\n", priv->duty_cycle_max);
}

/*
 * convert millisecond value to a "raw" value ready to be written to
 * the register.
 */
static u8 msec_to_raw_ramp_time(unsigned long msec)
{
	u8 ret;

	if (msec <= 1500)
		/*
		 * register handles 250 msec increments if below 1500
		 * msec.
		 */
		ret = msec / 250;
	else
		/*
		 * there is no 1750 msec step, and 2000 msec is
		 * encoded as 0x7.
		 */
		ret = 0x7;

	return ret;
}

/*
 * convert raw register value to a millisecond value.
 */
static unsigned long raw_ramp_time_to_msec(u8 raw)
{
	unsigned long ret;

	if (raw < 7)
		ret = 250 * raw;
	else
		ret = 2000;

	return ret;
}

/*
 * ramp time sysfs callbacks. delays are not reliable if programmed
 * want time is higher than 1000 msec.
 *
 * values that can be written are on the range [0, 2000] and are given
 * in milliseconds. Values higher than 2000 are clamped to 2000. shown
 * values are rounded up to the next value supported by the hardware.
 */

static ssize_t store_ramp_time_rise(struct device *dev,
				    struct device_attribute *attr,
				    const char *buf, size_t count)
{
	unsigned long val;
	u8 raw_val;
	struct i2c_client *client;
	struct cap1066_priv *priv;
	int error;
	u8 reg;

	client = to_i2c_client(dev);
	priv = i2c_get_clientdata(client);

	if (is_white(*buf))
		return 1;

	error = get_ulong(buf, count, &val);
	if (error)
		return error;

	raw_val = msec_to_raw_ramp_time(val);

	cap1066_read_reg(client, SMSC_REG_LED_DIRECT_RAMP_RATE, &reg);
	reg &= ~(0x7 << 3);
	reg |= raw_val << 3;
	cap1066_write_reg(client, SMSC_REG_LED_DIRECT_RAMP_RATE, reg);

	priv->raw_ramp_time_rise = raw_val;

	pr_debug(PFX "store_ramp_time_rise: reg = %02x\n", reg);
	return count;
}

static ssize_t show_ramp_time_rise(struct device *dev,
				   struct device_attribute *attr,
				   char *buf)
{
	struct i2c_client *client;
	struct cap1066_priv *priv;
	unsigned long msec;

	client = to_i2c_client(dev);
	priv = i2c_get_clientdata(client);

	msec = raw_ramp_time_to_msec(priv->raw_ramp_time_rise);

	return sprintf(buf, "%lu\n", msec);
}

static ssize_t store_ramp_time_fall(struct device *dev,
				    struct device_attribute *attr,
				    const char *buf, size_t count)
{
	unsigned long val;
	u8 raw_val;
	struct i2c_client *client;
	struct cap1066_priv *priv;
	int error;
	u8 reg;

	client = to_i2c_client(dev);
	priv = i2c_get_clientdata(client);

	if (is_white(*buf))
		return 1;

	error = get_ulong(buf, count, &val);
	if (error)
		return error;

	raw_val = msec_to_raw_ramp_time(val);

	cap1066_read_reg(client, SMSC_REG_LED_DIRECT_RAMP_RATE, &reg);
	reg &= ~0x7;
	reg |= raw_val;
	cap1066_write_reg(client, SMSC_REG_LED_DIRECT_RAMP_RATE, reg);

	priv->raw_ramp_time_fall = raw_val;

	pr_debug(PFX "store_ramp_time_rise: reg = %02x\n", reg);
	return count;
}

static ssize_t show_ramp_time_fall(struct device *dev,
				   struct device_attribute *attr,
				   char *buf)
{
	struct i2c_client *client;
	struct cap1066_priv *priv;
	unsigned long msec;

	client = to_i2c_client(dev);
	priv = i2c_get_clientdata(client);

	msec = raw_ramp_time_to_msec(priv->raw_ramp_time_fall);

	return sprintf(buf, "%lu\n", msec);
}

#define MTOUCH_ENABLE		(1 << 7)
#define MTOUCH_COUNT_MASK	(3 << 2)
#define MTOUCH_COUNT_SHIFT	(2)

/*
 * touch limit handling: the hardware can report at most 1 to 4 key
 * press event or no limit at all.
 *
 * Accepted values in touch_limit attribte:
 * 0 -> no limit
 * [1, 4] -> limit to the indicated count
 * [4, +inf [ -> invalid
 */
static ssize_t store_touch_limit(struct device *dev,
				 struct device_attribute *attr, const char *buf,
				 size_t count)
{
	struct i2c_client *client;
	unsigned long limit;
	int err;
	u8 mtouch_reg;

	client = to_i2c_client(dev);

	if (is_white(*buf))
		return 1;

	err = get_ulong(buf, count, &limit);
	if (err)
		return err;

	if (limit > 4)
		return -EINVAL;

	if (limit == 0) {
		mtouch_reg = 0;
	} else {
		mtouch_reg = MTOUCH_ENABLE |
			((limit - 1) << MTOUCH_COUNT_SHIFT);
	}
	cap1066_write_reg(client, SMSC_REG_MTOUCH_CFG_REG, mtouch_reg);

	return count;
}

static ssize_t show_touch_limit(struct device *dev,
				struct device_attribute *attr,
				char *buf)
{
	struct i2c_client *client;
	u8 mtouch_reg;

	client = to_i2c_client(dev);

	cap1066_read_reg(client, SMSC_REG_MTOUCH_CFG_REG, &mtouch_reg);

	if (mtouch_reg & MTOUCH_ENABLE) {
		u8 count = (mtouch_reg & MTOUCH_COUNT_MASK) >>
			MTOUCH_COUNT_SHIFT;
		return sprintf(buf, "%d\n", count + 1);
	} else {
		return sprintf(buf, "0\n");
	}
}

static DEVICE_ATTR(duty_cycle_min, S_IRUSR | S_IWUSR, show_duty_cycle_min,
		   store_duty_cycle_min);

static DEVICE_ATTR(duty_cycle_max, S_IRUSR | S_IWUSR, show_duty_cycle_max,
		   store_duty_cycle_max);

static DEVICE_ATTR(ramp_time_rise, S_IRUSR | S_IWUSR, show_ramp_time_rise,
		   store_ramp_time_rise);

static DEVICE_ATTR(ramp_time_fall, S_IRUSR | S_IWUSR, show_ramp_time_fall,
		   store_ramp_time_fall);

static DEVICE_ATTR(touch_limit, S_IWUSR | S_IRUSR, show_touch_limit,
		   store_touch_limit);

static struct device_attribute *cap1066_base_attributes[] = {
	&dev_attr_duty_cycle_min,
	&dev_attr_duty_cycle_max,
	&dev_attr_ramp_time_rise,
	&dev_attr_ramp_time_fall,
	&dev_attr_touch_limit,
};


/*
 * i2c core probe callback, called after sucessful detect
 */
static int cap1066_probe(struct i2c_client *client,
			 const struct i2c_device_id *id)
{
	struct input_polled_dev *poll_dev;
	struct input_dev *input;
	struct cap1066_priv *priv;
	int ret, i;

	ret = cap1066_detect(client, NULL);
	if (ret)
		dev_warn(&client->dev, "unknown cap1x66 device.\n");

	/* initialize controller */
	ret = cap1066_init_hw(client);
	if (ret)
		return ret;

	/* allocate context */
	priv = kzalloc(sizeof (*priv), GFP_KERNEL);
	poll_dev = input_allocate_polled_device();
	if (!priv || !poll_dev) {
		ret = -ENOMEM;
		goto out_fail;
	}

	/*
	 * must match what has been setup in cap1066_init_hw().
	 */
	priv->duty_cycle_min = DEFAULT_DUTY_CYCLE_MIN;
	priv->duty_cycle_max = DEFAULT_DUTY_CYCLE_MAX;
	priv->raw_ramp_time_rise = DEFAULT_RAMP_TIME_RISE;
	priv->raw_ramp_time_fall = DEFAULT_RAMP_TIME_FALL;

	if (client->dev.platform_data) {
		struct smsc_cap1066_pdata *pdata;

		pdata = client->dev.platform_data;
		memcpy(priv->keymap, pdata->key_map, sizeof (pdata->key_map));
		priv->has_irq_gpio = pdata->has_irq_gpio;
		priv->irq_gpio = pdata->irq_gpio;
	} else
		memcpy(priv->keymap, default_map, sizeof (default_map));

	if (priv->has_irq_gpio)
		gpio_direction_input(priv->irq_gpio);

	priv->poll_dev = poll_dev;
	priv->client = client;

	poll_dev->private = priv;
	poll_dev->poll = cap1066_input_poll;
	poll_dev->poll_interval = 50 /* ms */;

	input = poll_dev->input;
	input->name = "smsc_cap1066";
	input->phys = "smsc_cap1066/input0";
	input->id.bustype = BUS_I2C;
	input->dev.parent = &client->dev;

	input->keycode = priv->keymap;
	input->keycodemax = ARRAY_SIZE(priv->keymap);
	input->keycodesize = sizeof (unsigned short);

	set_bit(EV_REP, input->evbit);
	set_bit(EV_KEY, input->evbit);
	for (i = 0; i < ARRAY_SIZE(priv->keymap); i++)
		set_bit(priv->keymap[i], input->keybit);

	i2c_set_clientdata(client, priv);

	ret = input_register_polled_device(poll_dev);
	if (ret)
		goto out_fail;

	for (i = 0; i < ARRAY_SIZE(priv->keymap); ++i) {
		if (!priv->keymap[i])
			continue;
		priv->led_devices[i] =
			cap1066_create_led_dev(&client->dev,
					       priv, i, priv->keymap[i]);
	}

	if (create_sysfs_files(&client->dev, cap1066_base_attributes,
			       ARRAY_SIZE(cap1066_base_attributes)) < 0)
		goto out_free_led_devs;

	return 0;

out_free_led_devs:
	for (i = 0; i < ARRAY_SIZE(priv->keymap); ++i)
		if (priv->led_devices[i])
			cap1066_remove_led_dev(priv->led_devices[i]);
out_fail:
	input_free_polled_device(poll_dev);
	kfree(priv);
	i2c_set_clientdata(client, NULL);
	return ret;
}

/*
 * i2c core remove callback
 */
static int cap1066_remove(struct i2c_client *client)
{
	int i;
	struct cap1066_priv *priv = i2c_get_clientdata(client);

	for (i = 0; i < ARRAY_SIZE(cap1066_base_attributes); ++i)
		device_remove_file(&client->dev, cap1066_base_attributes[i]);

	for (i = 0; i < ARRAY_SIZE(priv->keymap); ++i) {
		if (priv->led_devices[i])
			cap1066_remove_led_dev(priv->led_devices[i]);
	}

	input_unregister_polled_device(priv->poll_dev);
	input_free_polled_device(priv->poll_dev);
	kfree(priv);

	return 0;
}

static struct i2c_driver cap1066_driver = {
	.driver = {
		.name	= "cap1066",
	},
	.probe		= cap1066_probe,
	.remove		= cap1066_remove,
	.id_table	= cap1066_id,

	.detect		= cap1066_detect,
	.class		= I2C_CLASS_HWMON,
	.address_list	= normal_i2c,
};

static int __init cap1066_init(void)
{
	return i2c_add_driver(&cap1066_driver);
}

static void __exit cap1066_exit(void)
{
	i2c_del_driver(&cap1066_driver);
}


MODULE_AUTHOR("Maxime Bizon <mbizon@freebox.fr>");
MODULE_DESCRIPTION("SMSC CAP1066 driver");
MODULE_LICENSE("GPL");

module_init(cap1066_init);
module_exit(cap1066_exit);
