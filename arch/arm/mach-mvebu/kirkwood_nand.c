/*
 * kirkwood_nand.c for fbxgw
 * Created by <nschichan@freebox.fr> on Mon Jan 26 17:08:40 2015
 */

#include <linux/kernel.h>
#include <linux/mtd/nand.h>
#include <linux/platform_device.h>
#include <linux/platform_data/mtd-orion_nand.h>

#include "mach/kirkwood_nand.h"

static struct resource kirkwood_nand_resource = {
	.flags		= IORESOURCE_MEM,
	.start		= KIRKWOOD_NAND_MEM_PHYS_BASE,
	.end		= KIRKWOOD_NAND_MEM_PHYS_BASE +
				KIRKWOOD_NAND_MEM_SIZE - 1,
};

static struct orion_nand_data kirkwood_nand_data = {
	.cle		= 0,
	.ale		= 1,
	.width		= 8,
};

static struct platform_device kirkwood_nand_flash = {
	.name		= "orion_nand",
	.id		= -1,
	.dev		= {
		.platform_data	= &kirkwood_nand_data,
	},
	.resource	= &kirkwood_nand_resource,
	.num_resources	= 1,
};

void __init kirkwood_nand_init_ecc(struct mtd_partition *parts, int nr_parts,
				  int chip_delay, struct kirkwood_nand_ecc *ecc)
{
	if (!ecc) {
		kirkwood_nand_data.ecc = NAND_ECC_SOFT;
		kirkwood_nand_data.bch_ecc_size =
			kirkwood_nand_data.bch_ecc_bytes = 0;
	} else {
		kirkwood_nand_data.ecc = ecc->ecc;
		kirkwood_nand_data.bch_ecc_bytes = ecc->bch_ecc_bytes;
		kirkwood_nand_data.bch_ecc_size = ecc->bch_ecc_size;
	}
	kirkwood_nand_data.parts = parts;
	kirkwood_nand_data.nr_parts = nr_parts;
	kirkwood_nand_data.chip_delay = chip_delay;
	platform_device_register(&kirkwood_nand_flash);
}
