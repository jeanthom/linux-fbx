#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/sched.h>
#include <linux/spi/spi.h>
#include <linux/spi/ssd1327.h>
#include <linux/workqueue.h>
#include <linux/gpio.h>
#include <linux/fb.h>
#include <linux/platform_device.h>
#include <linux/uaccess.h>
#include <linux/vmalloc.h>
#include <linux/backlight.h>

/*
 * commands
 */
#define OPCODE_CONTRAST		0x81

#define OPCODE_SET_COLUMN	0x15
#define OPCODE_SET_ROW		0x75
#define OPCODE_SET_REMAP	0xa0
#define OPCODE_DISPLAY_NORMAL	0xa4
#define OPCODE_DISPLAY_ALL_ON	0xa5
#define OPCODE_DISPLAY_ALL_OFF	0xa6

#define OPCODE_DISPLAY_OFF	0xae
#define OPCODE_DISPLAY_ON	0xaf

#define OPCODE_DEF_GRAY		0xb9

#define SSD1327_MAX_BRIGHTNESS		0x81
#define SSD1327_NOMINAL_BRIGHTNESS	0x64

/*
 * fbinfo
 */
static struct fb_fix_screeninfo ssd1327_fb_fix = {
	.id		= "ssd1327",
	.type		= FB_TYPE_PACKED_PIXELS,
	.visual		= FB_VISUAL_STATIC_PSEUDOCOLOR,
	.xpanstep	= 0,
	.ypanstep	= 1,
	.ywrapstep	= 0,
	.accel		= FB_ACCEL_NONE,
};

static struct fb_var_screeninfo ssd1327_fb_var = {
	.bits_per_pixel	= 8,
	.grayscale	= 1,
	.nonstd		= 1,
	.red.length	= 8,
	.green.length	= 8,
	.blue.length	= 8,
};

/*
 * private data
 */
#define SSD1327_COLS		64
#define SSD1327_ROWS		128
#define GDDRAM_SIZE		SSD1327_COLS * SSD1327_ROWS

struct ssd1327 {
	struct mutex			mutex;

	/* image of display ram */
	u8				gddram[GDDRAM_SIZE];
	u8				old_gddram[GDDRAM_SIZE];

	/* data ram, 8 bits per pixel */
	u8				*vmem;
	unsigned int			vmem_size;

	struct fb_info			*fb;
	struct ssd1327_platform_data	*data;
	struct spi_device		*spi;

	struct backlight_device		*backlight;
	unsigned int			brightness;

	/* watchog timer */
	struct delayed_work		wtd_work;
	unsigned int			wtd_max;
	atomic_t			wtd_count;
};

/*
 * send command to device
 */
static int send_cmd(struct ssd1327 *priv, u8 cmd)
{
	struct ssd1327_platform_data *data;
	int ret;

	data = priv->spi->dev.platform_data;

	mutex_lock(&priv->mutex);
	gpio_set_value(data->data_select_gpio, 0);
	ret = spi_write_then_read(priv->spi, &cmd, 1, NULL, 0);
	mutex_unlock(&priv->mutex);
	return ret;
}

/*
 * send command list to device
 */
static int send_cmds(struct ssd1327 *priv, const u8 *cmd, unsigned int len)
{
	unsigned int i;
	int ret;

	for (i = 0; i < len; i++) {
		ret = send_cmd(priv, cmd[i]);
		if (ret < 0)
			return ret;
	}
	return 0;
}

/*
 * write given data into device gddram
 */
static int write_data(struct ssd1327 *priv, u8 *tx, unsigned int size)
{
	struct ssd1327_platform_data *data;
	int ret;

	data = priv->spi->dev.platform_data;

	mutex_lock(&priv->mutex);
	gpio_set_value(data->data_select_gpio, 1);
	ret = spi_write(priv->spi, tx, size);
	mutex_unlock(&priv->mutex);
	return ret;
}

/*
 * soft reset & initialize ssd1327
 */
static int ssd1327_init(struct ssd1327 *priv)
{
	const u8 init_cmds[] = { OPCODE_DISPLAY_ON,

				 /* set even/odd splitting */
				 OPCODE_SET_REMAP, (1 << 6),
				 OPCODE_CONTRAST, SSD1327_NOMINAL_BRIGHTNESS,
				 OPCODE_DEF_GRAY,
				 OPCODE_DISPLAY_NORMAL,
	};
	int ret;

	/* zero ram */
	ret = write_data(priv, priv->gddram, GDDRAM_SIZE);
	if (ret)
		return ret;

	return send_cmds(priv, init_cmds, sizeof (init_cmds));
}

/*
 * update area
 */
static int ssd1327_fb_update(struct ssd1327 *priv)
{
	unsigned int col, row, w, h, i, count;
	unsigned char *vmem;
	u8 *start;
	u8 ccmds[3] = { OPCODE_SET_COLUMN, 0, 0x3f };
	u8 rcmds[3] = { OPCODE_SET_ROW, 0, 0x7f };
	int toggle, last_toggle_pos, moved;

	w = priv->data->width;
	h = priv->data->height;

	/* backup previous gddram */
	memcpy(priv->old_gddram, priv->gddram, GDDRAM_SIZE);

	vmem = priv->vmem + w * priv->fb->var.yoffset;

	for (row = 0; row < SSD1327_ROWS; row++) {

		if (row >= h)
			break;

		for (col = 0; col < SSD1327_COLS; col++) {
			unsigned int nibble;
			u8 val;

			val = 0;
			for (nibble = 0; nibble < 2; nibble++) {
				unsigned int off, x;
				u8 vval;

				x = col * 2 + nibble;
				if (x >= w)
					break;

				switch (priv->fb->var.rotate) {
				case 0:
				default:
					off = row * w + x;
					break;

				case 180:
					off = w * h - (row * w + x) - 1;
					break;

				case 90:
					off = (w - x - 1) * w + row;
					break;

				case 270:
					off = x * w + (h - row - 1);
					break;
				}

				vval = vmem[off] >> 4;
				val |= vval << (nibble * 4);
			}

			priv->gddram[row * SSD1327_COLS + col] = val;
		}
	}

	/* count consecutive toggled bytes, each column/row address
	 * change adds 6 bytes to send  */
	moved = toggle = 0;
	last_toggle_pos = -INT_MAX;
	count = 0;
	for (i = 0; i < GDDRAM_SIZE; i++) {
		if (priv->gddram[i] ^ priv->old_gddram[i]) {
			/* if crossing column boundary and first
			 * address is not 0, we must send column
			 * command */
			if (moved && ((i % SSD1327_COLS) == 0)) {
				count += 3;
				moved = 0;
			}

			if (!toggle) {
				if (i - last_toggle_pos < 6) {
					unsigned int j;

					/* fake last columns as dirty,
					 * cheaper than repositionning
					 * cursor */
					for (j = last_toggle_pos; j < i; j++)
						priv->old_gddram[j] =
							~priv->gddram[j];

					count += i - last_toggle_pos - 1;
				} else {
					/* send command to change
					 * address & column */
					count += 6;

					/* if we changed first column address
					 * to non 0, remember it */
					if ((i % SSD1327_COLS))
						moved = 1;
					else
						moved = 0;
				}
			}

			toggle = 1;
			count++;

		} else {
			if (toggle)
				last_toggle_pos = i - 1;
			toggle = 0;
		}
	}

	/* force full gddram update if we would send more bytes
	 * using clever update */
	if (count > GDDRAM_SIZE)
		return write_data(priv, priv->gddram, GDDRAM_SIZE);

	moved = toggle = 0;
	count = 0;
	start = NULL;
	for (i = 0; i < GDDRAM_SIZE; i++) {

		if (priv->gddram[i] ^ priv->old_gddram[i]) {
			/* if crossed column boundary and first
			 * address is not 0, we must send command to
			 * reset column*/
			if (moved && ((i % SSD1327_COLS) == 0)) {
				write_data(priv, start, count);
				start += count;
				count = 0;
				ccmds[1] = 0;
				send_cmds(priv, ccmds, 3);
				moved = 0;
			}

			if (!toggle) {
				ccmds[1] = i % SSD1327_COLS;
				rcmds[1] = i / SSD1327_COLS;
				send_cmds(priv, ccmds, 3);
				send_cmds(priv, rcmds, 3);

				/* if we changed first column address
				 * to non 0, remember it */
				if ((i % SSD1327_COLS))
					moved = 1;
				else
					moved = 0;
				start = &priv->gddram[i];
			}

			count++;
			toggle = 1;

		} else {
			if (count) {
				write_data(priv, start, count);
				count = 0;
			}
			toggle = 0;
		}
	}

	if (count)
		write_data(priv, start, count);

	/* reset position */
	ccmds[1] = 0;
	send_cmds(priv, ccmds, 3);
	rcmds[1] = 0;
	send_cmds(priv, rcmds, 3);
	return 0;
}

/*
 * frame buffer fill rect callback
 */
static void ssd1327_fb_fillrect(struct fb_info *info,
				const struct fb_fillrect *rect)
{
	struct ssd1327 *priv = info->par;
	sys_fillrect(info, rect);
	atomic_set(&priv->wtd_count, priv->wtd_max);
	ssd1327_fb_update(priv);
}

/*
 * frame buffer copy area callback
 */
static void ssd1327_fb_copyarea(struct fb_info *info,
				const struct fb_copyarea *area)
{
	struct ssd1327 *priv = info->par;
	sys_copyarea(info, area);
	atomic_set(&priv->wtd_count, priv->wtd_max);
	ssd1327_fb_update(priv);
}

/*
 * frame buffer image blit
 */
static void ssd1327_fb_imageblit(struct fb_info *info,
				 const struct fb_image *image)
{
	struct ssd1327 *priv = info->par;
	sys_imageblit(info, image);
	atomic_set(&priv->wtd_count, priv->wtd_max);
	ssd1327_fb_update(priv);
}

/*
 * frame buffer pan callback
 */
static int ssd1327_fb_pan(struct fb_var_screeninfo *var, struct fb_info *info)
{
	struct ssd1327 *priv = info->par;
	priv->fb->var.xoffset = var->xoffset;
	priv->fb->var.yoffset = var->yoffset;
	atomic_set(&priv->wtd_count, priv->wtd_max);
	ssd1327_fb_update(priv);
	return 0;
}

/*
 * fram buffer set_par callback, set videomode
 */
static int ssd1327_fb_set_par(struct fb_info *info)
{
	struct ssd1327 *priv = info->par;
	/* called after rotate update */
	atomic_set(&priv->wtd_count, priv->wtd_max);
	ssd1327_fb_update(priv);
	return 0;
}

static int ssd1327_fb_check_var(struct fb_var_screeninfo *var,
				struct fb_info *info)
{
	unsigned int rotate;

	rotate = var->rotate;
	if (rotate != 0 && rotate != 90 && rotate != 180 && rotate != 270)
		rotate = 0;
	*var = info->var;
	var->rotate = rotate;
	return 0;
}

/*
 * frame buffer blank callback
 */
static int ssd1327_fb_blank(int blank, struct fb_info *info)
{
	return 0;
}

/*
 * frame buffer write from userspace
 */
static ssize_t ssd1327_fb_write(struct fb_info *info, const char __user *buf,
				size_t count, loff_t *ppos)
{
	struct ssd1327 *priv = info->par;
	unsigned long p = *ppos;
	void *dst;
	int err = 0;
	unsigned long total_size;

	if (info->state != FBINFO_STATE_RUNNING)
		return -EPERM;

	total_size = info->fix.smem_len;

	if (p > total_size)
		return -EFBIG;

	if (count > total_size) {
		err = -EFBIG;
		count = total_size;
	}

	if (count + p > total_size) {
		if (!err)
			err = -ENOSPC;

		count = total_size - p;
	}

	dst = (void __force *)(info->screen_base + p);

	if (copy_from_user(dst, buf, count))
		err = -EFAULT;

	if  (!err)
		*ppos += count;

	atomic_set(&priv->wtd_count, priv->wtd_max);
	ssd1327_fb_update(priv);

	return (err) ? err : count;
}

static struct fb_ops ssd1327_fb_ops = {
	.owner		= THIS_MODULE,
	.fb_write	= ssd1327_fb_write,
	.fb_fillrect	= ssd1327_fb_fillrect,
	.fb_copyarea	= ssd1327_fb_copyarea,
	.fb_imageblit	= ssd1327_fb_imageblit,
	.fb_pan_display	= ssd1327_fb_pan,
	.fb_blank	= ssd1327_fb_blank,
	.fb_check_var	= ssd1327_fb_check_var,
	.fb_set_par	= ssd1327_fb_set_par,
};

/*
 * watchdog timer
 */
static void wtd_work_cb(struct work_struct *t)
{
	struct ssd1327 *priv;
	struct delayed_work *dwork;

	dwork = container_of(t, struct delayed_work, work);
	priv = container_of(dwork, struct ssd1327, wtd_work);

	if (atomic_dec_and_test(&priv->wtd_count)) {
		dev_err(&priv->spi->dev, "watchdog triggered\n");
		memset(priv->vmem, 0, priv->vmem_size);
		ssd1327_fb_update(priv);
	}

	schedule_delayed_work(&priv->wtd_work, HZ);
}

/*
 * backlight control
 */
static int ssd1327_bl_update_status(struct backlight_device *bl)
{
	struct ssd1327 *priv;
	u8 bl_cmds[2];
	int ret;

	priv = bl_get_data(bl);

	bl_cmds[0] = OPCODE_CONTRAST;
	bl_cmds[1] = bl->props.brightness;

	ret = send_cmds(priv, bl_cmds, sizeof (bl_cmds));
	if (ret < 0)
		return ret;
	priv->brightness = bl->props.brightness;
	return 0;
}

static int ssd1327_bl_get_brightness(struct backlight_device *bl)
{
	struct ssd1327 *priv;
	priv = bl_get_data(bl);
	return priv->brightness;
}

static struct backlight_ops ssd1327_bl_ops = {
	.update_status		= ssd1327_bl_update_status,
	.get_brightness		= ssd1327_bl_get_brightness,
};

static const struct backlight_properties ssd1327_bl_props = {
	.power		= FB_BLANK_UNBLANK,
	.fb_blank	= FB_BLANK_UNBLANK,
	.max_brightness	= SSD1327_MAX_BRIGHTNESS,
	.type		= BACKLIGHT_RAW,
};

static int init_backlight(struct ssd1327 *priv)
{
	struct backlight_device *bl;

	bl = backlight_device_register("ssd1327", &priv->spi->dev,
				       priv, &ssd1327_bl_ops,
				       &ssd1327_bl_props);
	if (IS_ERR(bl)) {
		dev_err(&priv->spi->dev, "error %ld on backlight register\n",
			PTR_ERR(bl));
		return PTR_ERR(bl);
	}
	priv->backlight = bl;
	bl->props.brightness = priv->brightness;
	return 0;
}

/*
 * platform device probe callback
 */
static int ssd1327_probe(struct spi_device *spi)
{
	struct ssd1327 *priv;
	struct ssd1327_platform_data *data;
	struct fb_info *fb;
	int ret;

	data = spi->dev.platform_data;
	if (!data) {
		dev_err(&spi->dev, "no screen description\n");
		return -ENODEV;
	}

	/* sanity check on screen size */
	if (data->width > SSD1327_COLS * 2 ||
	    data->height > SSD1327_ROWS) {
		dev_err(&spi->dev, "unsupported screen dimension\n");
		return -ENODEV;
	}

	fb = framebuffer_alloc(sizeof (*priv), &spi->dev);
	if (!fb)
		return -ENOMEM;
	priv = fb->par;
	mutex_init(&priv->mutex);
	priv->spi = spi;
	priv->data = data;
	priv->fb = fb;
	priv->brightness = SSD1327_NOMINAL_BRIGHTNESS;
	priv->wtd_max = data->watchdog;

	/* setup framebuffer */
	fb->fbops = &ssd1327_fb_ops;
	fb->flags = FBINFO_FLAG_DEFAULT | FBINFO_HWACCEL_YPAN;
	fb->var = ssd1327_fb_var;
	fb->fix = ssd1327_fb_fix;

	fb->var.xres = data->width;
	fb->var.yres = data->height;
	fb->var.xres_virtual = data->width;
	fb->var.yres_virtual = data->height * 2;

	/* twice lcd size so we can pan in one direction */
	fb->fix.smem_len = (data->width * data->height) * 2;
	fb->fix.line_length = data->width;
	fb->var.rotate = data->rotate;

	/* allocate video memory */
	priv->vmem_size = PAGE_ALIGN(fb->fix.smem_len);
	priv->vmem = vmalloc(priv->vmem_size);
	if (!priv->vmem) {
		ret = -ENOMEM;
		goto fail;
	}
	memset(priv->vmem, 0, priv->vmem_size);
	fb->screen_base = (char __iomem *)priv->vmem;

	ret = ssd1327_init(priv);
	if (ret)
		goto fail;

	if (init_backlight(priv))
		goto fail;

	/* register frame buffer */
	ret = register_framebuffer(fb);
	if (ret < 0)
		goto fail;

	INIT_DELAYED_WORK(&priv->wtd_work, wtd_work_cb);

	if (priv->wtd_max) {
		atomic_set(&priv->wtd_count, priv->wtd_max);
		schedule_delayed_work(&priv->wtd_work, HZ);
	}

	dev_info(&spi->dev,
		 "fb%d: SSD1327 frame buffer device (%ux%u screen)\n",
		 fb->node, data->width, data->height);

	dev_set_drvdata(&spi->dev, priv);
	return 0;

fail:
	if (priv->vmem)
		vfree(priv->vmem);
	if (priv->backlight)
		backlight_device_unregister(priv->backlight);
	framebuffer_release(fb);
	return ret;
}

/*
 * platform device remove callback
 */
static int ssd1327_remove(struct spi_device *spi)
{
	struct ssd1327 *priv;
	unsigned int i;

	priv = dev_get_drvdata(&spi->dev);
	cancel_delayed_work_sync(&priv->wtd_work);
	unregister_framebuffer(priv->fb);
	for (i = 0; i < priv->vmem_size; i += PAGE_SIZE) {
		struct page *page;
		page = vmalloc_to_page(priv->vmem + i);
		page->mapping = NULL;
	}
	vfree(priv->vmem);
	backlight_device_unregister(priv->backlight);
	framebuffer_release(priv->fb);
	return 0;
}

static struct spi_driver ssd1327_driver = {
	.driver = {
		.name		= "ssd1327",
		.owner		= THIS_MODULE,
	},
	.probe		= ssd1327_probe,
	.remove		= ssd1327_remove,
};

static int __init ssd1327_module_init(void)
{
	return spi_register_driver(&ssd1327_driver);
}

static void __exit ssd1327_module_exit(void)
{
	spi_unregister_driver(&ssd1327_driver);
}

module_init(ssd1327_module_init);
module_exit(ssd1327_module_exit);

MODULE_DESCRIPTION("SSD1327 driver");
MODULE_AUTHOR("Maxime Bizon <mbizon@freebox.fr>");
MODULE_LICENSE("GPL");
