/*
 * Copyright 2012 (C), Jason Cooper <jason@lakedaemon.net>
 *
 * arch/arm/mach-mvebu/kirkwood.c
 *
 * Flattened Device Tree board initialization
 *
 * This file is licensed under the terms of the GNU General Public
 * License version 2.  This program is licensed "as is" without any
 * warranty of any kind, whether express or implied.
 */

#include <linux/clk.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/mbus.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_net.h>
#include <linux/of_platform.h>
#include <linux/slab.h>
#include <linux/dma-mapping.h>
#include <asm/hardware/cache-feroceon-l2.h>
#include <asm/mach/arch.h>
#include <asm/mach/map.h>
#include <asm/mach-types.h>
#include "mvebu-soc-id.h"
#include "kirkwood.h"
#include "kirkwood-pm.h"
#include "common.h"
#include "board.h"

#include "fbxgw-common.h"

static struct resource kirkwood_cpufreq_resources[] = {
	[0] = {
		.start  = CPU_CONTROL_PHYS,
		.end    = CPU_CONTROL_PHYS + 3,
		.flags  = IORESOURCE_MEM,
	},
};

static struct platform_device kirkwood_cpufreq_device = {
	.name		= "kirkwood-cpufreq",
	.id		= -1,
	.num_resources	= ARRAY_SIZE(kirkwood_cpufreq_resources),
	.resource	= kirkwood_cpufreq_resources,
};

static void __init kirkwood_cpufreq_init(void)
{
	platform_device_register(&kirkwood_cpufreq_device);
}

static struct resource kirkwood_cpuidle_resource[] = {
	{
		.flags	= IORESOURCE_MEM,
		.start	= DDR_OPERATION_BASE,
		.end	= DDR_OPERATION_BASE + 3,
	},
};

static struct platform_device kirkwood_cpuidle = {
	.name		= "kirkwood_cpuidle",
	.id		= -1,
	.resource	= kirkwood_cpuidle_resource,
	.num_resources	= 1,
};

static void __init kirkwood_cpuidle_init(void)
{
	platform_device_register(&kirkwood_cpuidle);
}

static struct resource kirkwood_coretemp_resources[] = {
	[0] = {
		.start  = TEMP_PHYS_BASE,
		.end    = TEMP_PHYS_BASE + sizeof (u32) - 1,
		.flags  = IORESOURCE_MEM,
	},
};
struct platform_device kirkwood_coretemp_device = {
	.name	    = "kirkwood-coretemp",
	.id	    = -1,
	.num_resources = ARRAY_SIZE(kirkwood_coretemp_resources),
	.resource = kirkwood_coretemp_resources,
};

static void __init kirkwood_coretemp_init(void)
{
	u32 dev, rev;

	if (mvebu_get_soc_id(&dev, &rev) < 0)
		return;
	if (dev != 0x6282)
		return;

	platform_device_register(&kirkwood_coretemp_device);
}

#define MV643XX_ETH_MAC_ADDR_LOW	0x0414
#define MV643XX_ETH_MAC_ADDR_HIGH	0x0418

static void __init kirkwood_dt_eth_fixup(void)
{
	struct device_node *np;

	/*
	 * The ethernet interfaces forget the MAC address assigned by u-boot
	 * if the clocks are turned off. Usually, u-boot on kirkwood boards
	 * has no DT support to properly set local-mac-address property.
	 * As a workaround, we get the MAC address from mv643xx_eth registers
	 * and update the port device node if no valid MAC address is set.
	 */
	for_each_compatible_node(np, NULL, "marvell,kirkwood-eth-port") {
		struct device_node *pnp = of_get_parent(np);
		struct clk *clk;
		struct property *pmac;
		void __iomem *io;
		u8 *macaddr;
		u32 reg;

		if (!pnp)
			continue;

		/* skip disabled nodes or nodes with valid MAC address*/
		if (!of_device_is_available(pnp) || of_get_mac_address(np))
			goto eth_fixup_skip;

		clk = of_clk_get(pnp, 0);
		if (IS_ERR(clk))
			goto eth_fixup_skip;

		io = of_iomap(pnp, 0);
		if (!io)
			goto eth_fixup_no_map;

		/* ensure port clock is not gated to not hang CPU */
		clk_prepare_enable(clk);

		/* store MAC address register contents in local-mac-address */
		pr_err(FW_INFO "%s: local-mac-address is not set\n",
		       np->full_name);

		pmac = kzalloc(sizeof(*pmac) + 6, GFP_KERNEL);
		if (!pmac)
			goto eth_fixup_no_mem;

		pmac->value = pmac + 1;
		pmac->length = 6;
		pmac->name = kstrdup("local-mac-address", GFP_KERNEL);
		if (!pmac->name) {
			kfree(pmac);
			goto eth_fixup_no_mem;
		}

		macaddr = pmac->value;
		reg = readl(io + MV643XX_ETH_MAC_ADDR_HIGH);
		macaddr[0] = (reg >> 24) & 0xff;
		macaddr[1] = (reg >> 16) & 0xff;
		macaddr[2] = (reg >> 8) & 0xff;
		macaddr[3] = reg & 0xff;

		reg = readl(io + MV643XX_ETH_MAC_ADDR_LOW);
		macaddr[4] = (reg >> 8) & 0xff;
		macaddr[5] = reg & 0xff;

		of_update_property(np, pmac);

eth_fixup_no_mem:
		iounmap(io);
		clk_disable_unprepare(clk);
eth_fixup_no_map:
		clk_put(clk);
eth_fixup_skip:
		of_node_put(pnp);
	}
}

/*
 * Disable propagation of mbus errors to the CPU local bus, as this
 * causes mbus errors (which can occur for example for PCI aborts) to
 * throw CPU aborts, which we're not set up to deal with.
 */
void kirkwood_disable_mbus_error_propagation(void)
{
	void __iomem *cpu_config;

	cpu_config = ioremap(CPU_CONFIG_PHYS, 4);
	writel(readl(cpu_config) & ~CPU_CONFIG_ERROR_PROP, cpu_config);
}

/*
 * See errata. Without this work around:
 *    "The risk is that the PCIe will not work properly."
 */
static void kirkwood_fe_misc_120(void)
{
	u32 dev, rev;
	void __iomem *reg;

	if (mvebu_get_soc_id(&dev, &rev)) {
		pr_warn("unable to get soc id when trying to apply "
			"FE-MISC-120\n");
		return ;
	}

	if (dev != 0x6282)
		/* not applicable on this device */
		return ;

	reg = ioremap(FE_MISC_120_REG, 4);
	if (!reg) {
		pr_warn("unable to ioremap %08x when trying to apply "
			"FE-MISC-120.\n", FE_MISC_120_REG);
		return ;
	}

	writel(readl(reg) | (3 << 25), reg);
	iounmap(reg);
}

/*
 * map the IMMR space using iotable_init, like in the old times ...
 * As it is going to be mapped using a single 1M section, this can't
 * be bad wrt TLB pressure.
 */
static struct map_desc __initdata kirkwood_io_desc[] = {
	{
		.virtual	= 0xfec00000,
		.pfn		= __phys_to_pfn(0xf1000000),
		.length		= SZ_1M,
		.type		= MT_DEVICE,
	},
};
static void __init kirkwood_map_io(void)
{
	iotable_init(kirkwood_io_desc, ARRAY_SIZE(kirkwood_io_desc));
}

static struct of_dev_auxdata auxdata[] __initdata = {
	OF_DEV_AUXDATA("marvell,kirkwood-audio", 0xf10a0000,
		       "mvebu-audio", NULL),
	{ /* sentinel */ }
};

static void __init kirkwood_init_early(void)
{
	init_dma_coherent_pool_size(SZ_1M);
}

static void __init kirkwood_dt_init(void)
{
	kirkwood_disable_mbus_error_propagation();
	kirkwood_fe_misc_120();
	kirkwood_coretemp_init();

	BUG_ON(mvebu_mbus_dt_init(false));

#ifdef CONFIG_CACHE_FEROCEON_L2
	feroceon_of_init();
#endif
	kirkwood_cpufreq_init();
	kirkwood_cpuidle_init();

	kirkwood_pm_init();
	kirkwood_dt_eth_fixup();

	if (of_machine_is_compatible("lacie,netxbig"))
		netxbig_init();

	of_platform_populate(NULL, of_default_bus_match_table, auxdata, NULL);


#ifdef CONFIG_FBXGW_COMMON
	/*
	 * run pinctrl and mvebu driver init there to control
	 * order. we need both pinctrl and gpio controllers up and
	 * running for fbxgw1r/fbxgw2r_init() functions.
	 *
	 * I'm not proud of this.
	 */
	{
		extern int kirkwood_pinctrl_driver_init(void);
		extern int mvebu_gpio_driver_init(void);

		kirkwood_pinctrl_driver_init();
		mvebu_gpio_driver_init();
	}

	if (of_machine_is_compatible("freebox,fbxgw1r"))
		fbxgw1r_init();

	if (of_machine_is_compatible("freebox,fbxgw2r"))
		fbxgw2r_init();
#endif
}

static const char * const kirkwood_dt_board_compat[] __initconst = {
	"marvell,kirkwood",
	NULL
};

DT_MACHINE_START(KIRKWOOD_DT, "Marvell Kirkwood (Flattened Device Tree)")
	/* Maintainer: Jason Cooper <jason@lakedaemon.net> */
	.init_machine	= kirkwood_dt_init,
	.restart	= mvebu_restart,
	.dt_compat	= kirkwood_dt_board_compat,
MACHINE_END

#ifdef CONFIG_MACH_FBXGW1R
static const char * const fbxgw1r_dt_board_compat[] = {
	"freebox,fbxgw1r",
	NULL,
};

MACHINE_START(FBXGW1R, "Freebox FBXGW1R (In-kernel Flattened Device Tree)")
	.init_machine	= kirkwood_dt_init,
	.restart = mvebu_restart,
	.init_early = kirkwood_init_early,
#ifdef CONFIG_PSTORE_RAM
	.reserve = fbxgw_reserve_crash_zone,
#endif
	.map_io = kirkwood_map_io,
	.dt_compat = fbxgw1r_dt_board_compat,
MACHINE_END

FDT_DESC(FBXGW1R);
#endif

#ifdef CONFIG_MACH_FBXGW2R
static const char * const fbxgw2r_dt_board_compat[] = {
	"freebox,fbxgw2r",
	NULL,
};

MACHINE_START(FBXGW2R, "Freebox FBXGW2R (In-kernel Flattened Device Tree)")
	.init_machine	= kirkwood_dt_init,
	.restart	= mvebu_restart,
	.init_early = kirkwood_init_early,
#ifdef CONFIG_PSTORE_RAM
	.reserve	= fbxgw_reserve_crash_zone,
#endif
	.map_io		= kirkwood_map_io,
	.dt_compat	= fbxgw2r_dt_board_compat,
MACHINE_END

FDT_DESC(FBXGW2R);
#endif
