#include <linux/init.h>
#include <linux/module.h>
#include <linux/interrupt.h>
#include <linux/delay.h>
#include <linux/platform_device.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/spi/spi.h>
#include <linux/spi/orion_tdm_spi.h>
#include <linux/gpio.h>
#include <linux/clk.h>
#include <asm/unaligned.h>

#define DRIVER_NAME			"orion_tdm_spi"

/*
 * registers
 */
#define PCM_CTRL_REG			0x0000
#define PCM_DAA_CSS_CTRL_MASK		(1 << 15)

#define SPI_CLK_PRESCALE_REG		0x3100
#define SCLOCK_LOW_SHIFT		0
#define SCLOCK_LOW_MASK			(0xff << SCLOCK_LOW_SHIFT)
#define SCLOCK_HIGH_SHIFT		8
#define SCLOCK_HIGH_MASK		(0xff << SCLOCK_HIGH_SHIFT)

#define SPI_GLOBAL_CTRL_REG		0x3104
#define CODEC_ENABLE_MASK		(1 << 0)

#define SPI_CTRL_REG			0x3108
#define SPI_STAT_MASK			(1 << 10)

#define SPI_CODEC_ACCESS_L_REG		0x3130
#define ACCESS_BYTE0_SHIFT		0
#define ACCESS_BYTE1_SHIFT		8

#define SPI_CODEC_ACCESS_H_REG		0x3134
#define ACCESS_BYTE2_SHIFT		0
#define ACCESS_BYTE3_SHIFT		8

#define SPI_REG_ACCESS_CTRL_REG		0x3138
#define BYTES_TO_XFER_MASK		0x3
#define SPI_LSB_MSB_MASK		(1 << 2)
#define SPI_RD_WR_MASK			(1 << 3)
#define SPI_BYTES_TO_READ_SHIFT		4
#define SPI_LO_SPEED_CLK_MASK		(1 << 5)
#define SPI_READ_CS_HOLD_SHIFT		6

#define SPI_READ_DATA_REG		0x313c

#define SPI_REG_ACCESS_CTRL1_REG	0x3140
#define SPI_WRITE_CS_HOLD_SHIFT		0

#define SPI_OUT_EN_CTRL_REG		0x4000
#define SPI_OUT_EN_DISABLE_MASK		(1 << 0)


struct orion_tdm_spi {
	struct spi_master		*master;
	void __iomem			*base;

	unsigned int			max_speed;
	unsigned int			min_speed;

	/* current configured speed/divs for each CS, drivers uses "low"
	 * for CS0 and "high" for CS1 */
	unsigned int			speeds[2];
	u32				divs[2];

	unsigned int			current_cs;
	struct orion_tdm_spi_info	*spi_info;
	struct clk			*clk;
};

MODULE_ALIAS("platform:" DRIVER_NAME);

static inline u32 spi_readl(struct orion_tdm_spi *priv, u32 reg)
{
	u32 val;

	val = readl(priv->base + reg);
/* 	printk("spi_readl: readl at %08x => 0x%08x\n", */
/* 	       priv->base + reg, val); */
	return val;
}

static inline void spi_writel(struct orion_tdm_spi *priv, u32 val, u32 reg)
{
/* 	printk("spi_writl: writl at %08x <= 0x%08x\n", */
/* 	       priv->base + reg, val); */
	writel(val, priv->base + reg);
}

static int spi_baudrate_set(struct orion_tdm_spi *priv,
			    unsigned int cs, unsigned int speed)
{
	u32 tclk_hz;
	u32 div, val;

	if (priv->speeds[cs] == speed)
		return 0;

	tclk_hz = clk_get_rate(priv->clk);

	/* find divider, the supported values are: 2...254 (even only) */
	div = DIV_ROUND_UP(tclk_hz, speed);
	div = roundup(div, 2);

	if (div > 254)
		return 1;

	if (div < 2)
		div = 2;

	/* don't reprogram div if not needed */
	if (priv->divs[cs] == div) {
		priv->speeds[cs] = speed;
		return 0;
	}

	/* Convert the rate to SPI clock divisor value.	*/
	val = spi_readl(priv, SPI_CLK_PRESCALE_REG);
	if (cs) {
		val &= ~SCLOCK_HIGH_MASK;
		val |= div << SCLOCK_HIGH_SHIFT;
	} else {
		val &= ~SCLOCK_LOW_MASK;
		val |= div << SCLOCK_LOW_SHIFT;
	}
	spi_writel(priv, val, SPI_CLK_PRESCALE_REG);

	priv->speeds[cs] = speed;
	priv->divs[cs] = div;
	return 0;
}

static void spi_set_cs(struct orion_tdm_spi *priv, struct spi_device *spi,
		       int active)
{
	struct orion_tdm_spi_info *spi_info;
	int gpio;

	spi_info = priv->spi_info;

	/* set correct cs in hardware */
	if (spi->chip_select != priv->current_cs) {
		u32 val;

		val = spi_readl(priv, PCM_CTRL_REG);
		if (spi->chip_select)
			val |= PCM_DAA_CSS_CTRL_MASK;
		else
			val &= ~PCM_DAA_CSS_CTRL_MASK;
		spi_writel(priv, val, PCM_CTRL_REG);
		priv->current_cs = spi->chip_select;
	}

	/* if not using gpio, hardware moves cs for us */
	gpio = spi_info->cs_use_gpio[priv->current_cs];
	if (gpio == -1)
		return;

	gpio_set_value(gpio, 1 - active);
}

static int do_spi_poll(struct orion_tdm_spi *priv)
{
	unsigned int loop;
	u32 val;

	for (loop = 0; loop < 1000; loop++) {
		val = spi_readl(priv, SPI_CTRL_REG);
		if (!(val & SPI_STAT_MASK))
			return 0;
	}
	return 1;
}

static int do_write_read(struct orion_tdm_spi *priv, struct spi_device *spi,
			 const u8 *tx, unsigned int tx_len,
			 u8 *rx, unsigned int rx_len)
{
	u32 val;

	if (do_spi_poll(priv)) {
		dev_err(&spi->dev, "spi_poll timed out\n");
		return 1;
	}

	val = tx[0];
	if (tx_len > 1)
		val |= tx[1] << 8;
	spi_writel(priv, val, SPI_CODEC_ACCESS_L_REG);

	if (tx_len > 2) {
		val = tx[2];
		if (tx_len > 3)
			val |= tx[3] << 8;
		spi_writel(priv, val, SPI_CODEC_ACCESS_H_REG);
	}


	val = tx_len - 1;
	if (rx_len)
		val |= SPI_RD_WR_MASK;
	if (rx_len > 1)
		val |= (1 << SPI_BYTES_TO_READ_SHIFT);
	if (spi->chip_select) {
		/* note: bit set to 1 => use high speed */
		val |= SPI_LO_SPEED_CLK_MASK;
	}
	spi_writel(priv, val, SPI_REG_ACCESS_CTRL_REG);

	val = spi_readl(priv, SPI_CTRL_REG);
	val |= SPI_STAT_MASK;
	spi_writel(priv, val, SPI_CTRL_REG);

	if (do_spi_poll(priv)) {
		dev_err(&spi->dev, "spi_poll timed out\n");
		return 1;
	}

	if (rx_len) {
		val = spi_readl(priv, SPI_READ_DATA_REG);
		rx[0] = val & 0xff;
		if (rx_len > 1)
			rx[1] = (val >> 8) & 0xff;
	}

	return 0;
}

static int orion_tdm_spi_setup(struct spi_device *spi)
{
	struct orion_tdm_spi *priv;

	priv = spi_master_get_devdata(spi->master);

	if (spi->bits_per_word == 0)
		spi->bits_per_word = 8;

	if (spi->bits_per_word != 8) {
		dev_err(&spi->dev, "setup: unsupported transfer width %u\n",
			spi->bits_per_word);
		return -EINVAL;
	}

	if ((spi->max_speed_hz == 0) ||
	    (spi->max_speed_hz > priv->max_speed))
		spi->max_speed_hz = priv->max_speed;

	if (spi->max_speed_hz < priv->min_speed) {
		dev_err(&spi->dev, "setup: requested speed too low %d Hz\n",
			spi->max_speed_hz);
		return -EINVAL;
	}

	return 0;
}

static int orion_tdm_spi_transfer(struct spi_device *spi,
				  struct spi_message *m)
{
	struct orion_tdm_spi *priv;
	struct spi_transfer *t;
	int want_write, cs_active;

	m->actual_length = 0;
	m->status = 0;

/* 	printk("orion_tdm_spi_transfer for device speed %u\n", */
/* 		spi->max_speed_hz); */

	/* reject invalid messages and transfers */
	if (list_empty(&m->transfers) || !m->complete)
		return -EINVAL;

	priv = spi_master_get_devdata(spi->master);

	/*
	 * first pass for sanity check
	 *
	 * hardware  is  dumb,  and   can't  read/write  at  the  same
	 * time. Worst, you have to write at least one byte to be able
	 * to read, and you can't not read more than two bytes.
	 */
	want_write = 0;

	list_for_each_entry(t, &m->transfers, transfer_list) {

		if (!t->len)
			continue;

		if (t->tx_buf && t->rx_buf) {
			dev_err(&spi->dev,
				"message rejected : "
				"full duplex transfer not supported\n");
			m->status = -ENOTSUPP;
			goto msg_done;
		}

		if (!t->tx_buf && !t->rx_buf && t->len) {
			dev_err(&spi->dev,
				"message rejected : "
				"invalid transfer data buffers\n");
			m->status = -EINVAL;
			goto msg_done;
		}

		/* check if forced transfer width is valid */
		if (t->bits_per_word && t->bits_per_word != 8) {
			dev_err(&spi->dev,
				"message rejected : "
				"invalid transfer bits_per_word (%d bits)\n",
				t->bits_per_word);
			m->status = -EINVAL;
			goto msg_done;
		}

		/* check if forced transfer speed if ok */
		if (t->speed_hz && t->speed_hz < priv->min_speed) {
			dev_err(&spi->dev,
				"message rejected : "
				"device min speed (%d Hz) exceeds "
				"required transfer speed (%d Hz)\n",
				priv->min_speed, t->speed_hz);
			m->status = -EINVAL;
			goto msg_done;
		}

		if (t->tx_buf)
			want_write += t->len;
		if (t->rx_buf) {
			if (t->len > 2) {
				dev_err(&spi->dev,
					"message rejected : "
					"marvell dumb spi can't read "
					"more than 2 bytes\n");
				m->status = -EINVAL;
				goto msg_done;

			}

			if (want_write)
				want_write = 0;
			else {
				dev_err(&spi->dev,
					"message rejected : "
					"marvell dumb spi can't read "
					"without write first\n");
				m->status = -EINVAL;
				goto msg_done;
			}
		}
	}

/* 	printk("SPI CS\n"); */
	spi_set_cs(priv, spi, 0);
	cs_active = 0;

	/* do the actual transfer, we need to coalesce write and read
	 * transfer */
	list_for_each_entry(t, &m->transfers, transfer_list) {
		unsigned int i, speed;

		if (!t->len)
			continue;

		/* get and configure speed for this transfer */
		if (t->speed_hz)
			speed = t->speed_hz;
		else
			speed = spi->max_speed_hz;

		if (spi_baudrate_set(priv, spi->chip_select, speed)) {
			m->status = -EINVAL;
			goto msg_done;
		}

		/* write always one byte, if this is the last byte to
		 * transfer, lookahead next transfer and read if
		 * needed */
		BUG_ON(!t->tx_buf);

		for (i = 0; i < t->len;) {
			struct spi_transfer *nt;
			u8 *rx;
			const u8 *tx;
			unsigned int rx_len, tx_remain;

			rx = NULL;
			nt = NULL;
			rx_len = 0;

			tx_remain = t->len - i;
			if (tx_remain == 1) {
				struct list_head *e;

				/* last byte to write, check if next
				 * transfer is a read and coalesce */
				e = t->transfer_list.next;
				if (e != &m->transfers) {
					nt = list_entry(e, struct spi_transfer,
							transfer_list);
					if (nt->rx_buf) {
						rx = nt->rx_buf;
						rx_len = nt->len;
					} else
						nt = NULL;
				}
			}

			/* we can write 4 bytes at a time if not
			 * reading */
			if (tx_remain > 4)
				tx_remain = 4;
			else {
				/* make sure we leave at least one
				 * byte in case we need to coalesce
				 * with next read */
				if (tx_remain > 1)
					tx_remain--;
			}
			tx = t->tx_buf + i;

			if (!cs_active) {
				spi_set_cs(priv, spi, 1);
				cs_active = 1;
			}

			if (do_write_read(priv, spi, tx, tx_remain,
					  rx, rx_len)) {
				m->status = -EIO;
				goto msg_done;
			}

			if (t->cs_change) {
				spi_set_cs(priv, spi, 0);
				cs_active = 0;
			}

			m->actual_length += tx_remain + rx_len;

			/* skip next transfer if we coalesced it */
			if (nt) {
				t = nt;
				break;
			}

			i += tx_remain;
		}

		if (t->delay_usecs)
			udelay(t->delay_usecs);
	}

	if (cs_active)
		spi_set_cs(priv, spi, 0);

msg_done:
	if (m->complete)
		m->complete(m->context);
	return m->status;
}

static int orion_tdm_spi_info_from_of(struct device *dev,
				      struct orion_tdm_spi_info **out)
{
	int err;
	uint32_t arr[2];

	err = of_property_read_u32_array(dev->of_node, "fbx,cs-gpios", arr,
					 ARRAY_SIZE(arr));
	if (err)
		return err;

	*out = devm_kzalloc(dev, sizeof (**out), GFP_KERNEL);
	if (!*out)
		return -ENOMEM;

	(*out)->cs_use_gpio[0] = arr[0];
	(*out)->cs_use_gpio[1] = arr[1];

	dev_dbg(dev, "cs gpio[0] = %d\n", (*out)->cs_use_gpio[0]);
	dev_dbg(dev, "cs gpio[1] = %d\n", (*out)->cs_use_gpio[1]);
	return 0;
}

static int __init orion_tdm_spi_probe(struct platform_device *pdev)
{
	struct spi_master *master;
	struct orion_tdm_spi *priv;
	struct resource *r;
	struct orion_tdm_spi_info *spi_info;
	unsigned int tclk_hz;
	int status = 0;
	u32 val;

	spi_info = pdev->dev.platform_data;

	if (!spi_info && !pdev->dev.of_node)
		return -ENODEV;

	if (!spi_info) {
		int err = orion_tdm_spi_info_from_of(&pdev->dev, &spi_info);

		if (err)
			return err;
	}

	master = spi_alloc_master(&pdev->dev, sizeof (*priv));
	if (master == NULL) {
		dev_dbg(&pdev->dev, "master allocation failed\n");
		return -ENOMEM;
	}

	if (pdev->id != -1)
		master->bus_num = pdev->id;

	master->setup = orion_tdm_spi_setup;
	master->transfer = orion_tdm_spi_transfer;
	master->num_chipselect = 2;
	master->mode_bits = 0;

	dev_set_drvdata(&pdev->dev, master);

	priv = spi_master_get_devdata(master);
	priv->master = master;
	priv->spi_info = spi_info;

	priv->clk = devm_clk_get(&pdev->dev, NULL);
	if (IS_ERR(priv->clk)) {
		dev_err(&pdev->dev, "no associated clk\n");
		status = PTR_ERR(priv->clk);
		goto out;
	}

	clk_prepare_enable(priv->clk);
	tclk_hz = clk_get_rate(priv->clk);
	priv->max_speed = DIV_ROUND_UP(tclk_hz, 4);
	priv->min_speed = DIV_ROUND_UP(tclk_hz, 254);
	priv->current_cs = ~0;

	r = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (r == NULL) {
		status = -ENODEV;
		goto out_clk;
	}
	priv->base = devm_ioremap_resource(&pdev->dev, r);
	if (!priv->base) {
		status = -ENOMEM;
		goto out_clk;
	}

	/* configure TDM SPI */
	val = spi_readl(priv, SPI_OUT_EN_CTRL_REG);
	val &= ~SPI_OUT_EN_DISABLE_MASK;
	spi_writel(priv, val, SPI_OUT_EN_CTRL_REG);

	val = spi_readl(priv, SPI_GLOBAL_CTRL_REG);
	val |= CODEC_ENABLE_MASK;
	spi_writel(priv, val, SPI_GLOBAL_CTRL_REG);

	master->dev.of_node = pdev->dev.of_node;
	status = spi_register_master(master);
	if (status < 0)
		goto out_clk;

	return status;

out_clk:
	clk_disable_unprepare(priv->clk);

out:
	spi_master_put(master);
	return status;
}

static int __exit orion_tdm_spi_remove(struct platform_device *pdev)
{
	struct spi_master *master;
	struct orion_tdm_spi *priv;

	master = dev_get_drvdata(&pdev->dev);
	priv = spi_master_get_devdata(master);

	clk_disable_unprepare(priv->clk);
	spi_unregister_master(master);

	return 0;
}

static const struct of_device_id orion_tdm_spi_match_table[] = {
	{ .compatible = "marvell,orion-tdm-spi", .data = NULL },
	{},
};

static struct platform_driver orion_tdm_spi_driver = {
	.driver = {
		.name	= DRIVER_NAME,
		.owner	= THIS_MODULE,
		.of_match_table = orion_tdm_spi_match_table,
	},
	.remove		= __exit_p(orion_tdm_spi_remove),
	.probe		= orion_tdm_spi_probe,
};

module_platform_driver(orion_tdm_spi_driver);

MODULE_DESCRIPTION("Orion TDM SPI driver");
MODULE_AUTHOR("Maxime Bizon <mbizon@freebox.fr>");
MODULE_LICENSE("GPL");
