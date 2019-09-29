ccflags-$(CONFIG_ARCH_MULTIPLATFORM) := -I$(srctree)/$(src)/include \
	-I$(srctree)/arch/arm/plat-orion/include

AFLAGS_coherency_ll.o		:= -Wa,-march=armv7-a
CFLAGS_pmsu.o			:= -march=armv7-a

obj-$(CONFIG_MACH_MVEBU_ANY)	 += system-controller.o mvebu-soc-id.o

ifeq ($(CONFIG_MACH_MVEBU_V7),y)
obj-y				 += cpu-reset.o board-v7.o coherency.o coherency_ll.o pmsu.o pmsu_ll.o pm.o pm-board.o
obj-$(CONFIG_SMP)		 += platsmp.o headsmp.o platsmp-a9.o headsmp-a9.o
endif

obj-$(CONFIG_MACH_DOVE)		 += dove.o
obj-$(CONFIG_MACH_KIRKWOOD)	 += kirkwood.o kirkwood-pm.o
obj-$(CONFIG_FBXGW_COMMON)	 += fbxgw-common.o fbxgw-switch.o \
				 kirkwood_nand.o fbxgw-pcie.o
obj-$(CONFIG_MACH_FBXGW2R)	 += fbxgw2r-setup.o
obj-$(CONFIG_MACH_FBXGW1R)	 += fbxgw1r-setup.o

ifdef CONFIG_MACHTYPE_FDT

quiet_cmd_GENFDTS      = GEN     $@
      cmd_GENFDTS      = arch/arm/tools/gen-fdt-s $2 > $@ || rm -f $@

define add-fdt
obj-$$(CONFIG_MACH_$2)		 += fdt-$1.o
arch/arm/mach-mvebu/fdt-$1.S: arch/arm/boot/dts/$1.dtb
	$$(call cmd,GENFDTS,$2)
arch/arm/boot/dts/$1.dtb: arch/arm/boot/dts/$1.dts
	$$(call cmd,dtc)
endef

$(eval $(call add-fdt,fbxgw1r,FBXGW1R))
$(eval $(call add-fdt,fbxgw2r,FBXGW2R))

endif

