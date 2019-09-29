/*
 * orion_nand.h for fbxgw
 * Created by <nschichan@freebox.fr> on Mon Jan 26 16:56:29 2015
 */

#ifndef __MACH_ORION_NAND_H
# define __MACH_ORION_NAND_H

# define KIRKWOOD_NAND_MEM_PHYS_BASE	0xf4000000
# define KIRKWOOD_NAND_MEM_SIZE		SZ_1K

# define  NAND_PHYS_BASE		(0xf1010400)
# define   NAND_RD_PARAM_OFF		0x0018
# define   NAND_WR_PARAM_OFF		0x001C
# define   NAND_FLASH_CTL_OFF		0x0070

struct kirkwood_nand_ecc {
	u8 ecc;
	u16 bch_ecc_size;
	u16 bch_ecc_bytes;
};

void kirkwood_nand_init_ecc(struct mtd_partition *parts, int nr_parts,
			    int delay, struct kirkwood_nand_ecc *ecc);

#endif /*! __MACH_ORION_NAND_H */
