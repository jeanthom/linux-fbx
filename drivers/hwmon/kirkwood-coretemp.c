/*
 * kirkwood-coretemp.c for kirkwood-coretemp
 * Created by <nschichan@freebox.fr> on Wed Jul 11 19:59:27 2012
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/io.h>
#include <linux/hwmon.h>
#include <linux/hwmon-sysfs.h>
#include <linux/platform_device.h>
#include <linux/err.h>

#define PFX "kirkwood-coretemp: "

struct kirkwood_coretemp_priv {
	void __iomem *reg;
	struct device *hwmon_dev;
	struct attribute_group attrs;
};

static int show_kirkwood_coretemp(struct device *dev,
				  struct device_attribute *devattr,
				  char *buf)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct kirkwood_coretemp_priv *priv = platform_get_drvdata(pdev);
	u32 reg;
	u32 raw_temp;
	s32 temp_out;

	reg = readl(priv->reg);

	/*
	 * TermTValid shall be set.
	 */
	if ((reg & (1 << 9)) == 0)
		return -EIO;

	raw_temp = (reg >> 10) & 0x1ff;

	/*
	 * out temperature = (322 - raw) / 1.3625
	 *
	 * can't use float here, so be creative.
	 *
	 * we also have to avoid 32bit integer overflow (hence the
	 * 1000000 / 1363 division instead of 10000000 / 13625)
	 */
	temp_out = (322 - raw_temp);
	temp_out = (temp_out * 1000000) / 1363;

	return sprintf(buf, "%i\n", temp_out);
}

static SENSOR_DEVICE_ATTR(temp1_input, S_IRUGO, show_kirkwood_coretemp,
			  NULL, 0);

static int show_name(struct device *dev, struct device_attribute *devattr,
		     char *buf)
{
	return sprintf(buf, "%s\n", kobject_name(&dev->kobj));
}

static DEVICE_ATTR(name, S_IRUGO, show_name, NULL);

static struct attribute *kirkwood_coretemp_attr[] = {
	&sensor_dev_attr_temp1_input.dev_attr.attr,
	&dev_attr_name.attr,
	NULL,
};

static int kirkwood_coretemp_probe(struct platform_device *pdev)
{
	struct kirkwood_coretemp_priv *priv;
	struct resource *resource;
	int err = 0;

	dev_dbg(&pdev->dev, "probe.\n");

	resource = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!resource)
		return -ENXIO;

	priv = kzalloc(sizeof (*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->reg = ioremap(resource->start, resource_size(resource));
	if (!priv->reg) {
		dev_err(&pdev->dev, "unable to ioremap registers.\n");
		err = -ENOMEM;
		goto err_free_priv;
	}

	priv->attrs.attrs = kirkwood_coretemp_attr;
	err = sysfs_create_group(&pdev->dev.kobj, &priv->attrs);
	if (err) {
		dev_err(&pdev->dev, "unable to greate sysfs group.\n");
		goto err_iounmap;
	}

	platform_set_drvdata(pdev, priv);

	priv->hwmon_dev = hwmon_device_register(&pdev->dev);
	if (IS_ERR(priv->hwmon_dev)) {
		dev_err(&pdev->dev, "unable to register hwmon device.\n");
		err = PTR_ERR(priv->hwmon_dev);
		goto err_sysfs_remove_group;
	}


	return 0;

err_sysfs_remove_group:
	sysfs_remove_group(&pdev->dev.kobj, &priv->attrs);
err_iounmap:
	iounmap(priv->reg);
err_free_priv:
	kfree(priv);
	return err;
}

static int kirkwood_coretemp_remove(struct platform_device *pdev)
{
	struct kirkwood_coretemp_priv *priv = platform_get_drvdata(pdev);

	dev_dbg(&pdev->dev, "remove.\n");
	hwmon_device_unregister(priv->hwmon_dev);
	sysfs_remove_group(&pdev->dev.kobj, &priv->attrs);
	iounmap(priv->reg);
	kfree(priv);

	return 0;
}

static struct platform_driver kirkwood_coretemp_driver = {
	.probe		= kirkwood_coretemp_probe,
	.remove		= kirkwood_coretemp_remove,
	.driver		= {
		.name	= "kirkwood-coretemp",
	}
};

static int __init kirkwood_coretemp_init(void)
{
	int err;

	err = platform_driver_register(&kirkwood_coretemp_driver);
	if (err) {
		printk(KERN_ERR PFX "unable to register platform driver.\n");
		return err;
	}

	return 0;
}

static void __exit kirkwood_coretemp_exit(void)
{
	platform_driver_unregister(&kirkwood_coretemp_driver);
}

module_init(kirkwood_coretemp_init);
module_exit(kirkwood_coretemp_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Nicolas Schichan <nschichan@freebox.fr>");
