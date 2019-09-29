/*
 *  arch/arm/include/asm/prom.h
 *
 *  Copyright (C) 2009 Canonical Ltd. <jeremy.kerr@canonical.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 */
#ifndef __ASMARM_PROM_H
#define __ASMARM_PROM_H

#ifdef CONFIG_OF

extern const struct machine_desc *setup_machine_fdt(unsigned int dt_phys);
extern unsigned int setup_fdt_machtype(unsigned int machtype,
				       unsigned int atags_pointer);

extern void __init arm_dt_init_cpu_maps(void);

#else /* CONFIG_OF */

static inline const struct machine_desc *setup_machine_fdt(unsigned int dt_phys)
{
	return NULL;
}
static inline phys_addr_t setup_fdt_machtype(unsigned int machtype,
					     unsigned int atags_pointer) {
	return atags_pointer;
}

static inline void arm_dt_init_cpu_maps(void) { }

#endif /* CONFIG_OF */
#endif /* ASMARM_PROM_H */
