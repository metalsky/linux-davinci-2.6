/*
 * linux/arch/arm/mach-omap/omap2/board-h4.c
 *
 * Copyright (C) 2005 Nokia Corporation
 * Author: Paul Mundt <paul.mundt@nokia.com>
 *
 * Modified from mach-omap/omap1/board-generic.c
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/device.h>

#include <asm/hardware.h>
#include <asm/mach-types.h>
#include <asm/mach/arch.h>
#include <asm/mach/map.h>

#include <asm/arch/gpio.h>
#include <asm/arch/mux.h>
#include <asm/arch/usb.h>
#include <asm/arch/board.h>
#include <asm/arch/common.h>

#include <asm/io.h>
#include <asm/delay.h>

static struct resource h4_smc91x_resources[] = {
	[0] = {
		.start  = OMAP24XX_ETHR_START,          /* Physical */
		.end    = OMAP24XX_ETHR_START + 0xf,
		.flags  = IORESOURCE_MEM,
	},
	[1] = {
		.start  = OMAP_GPIO_IRQ(OMAP24XX_ETHR_GPIO_IRQ),
		.end    = OMAP_GPIO_IRQ(OMAP24XX_ETHR_GPIO_IRQ),
		.flags  = IORESOURCE_IRQ,
	},
};

static struct platform_device h4_smc91x_device = {
	.name		= "smc91x",
	.id		= -1,
	.num_resources	= ARRAY_SIZE(h4_smc91x_resources),
	.resource	= h4_smc91x_resources,
};

static struct platform_device *h4_devices[] __initdata = {
	&h4_smc91x_device,
};

static inline void __init h4_init_smc91x(void)
{
	u32 l;

	if (omap_request_gpio(OMAP24XX_ETHR_GPIO_IRQ) < 0) {
		printk(KERN_ERR "Failed to request GPIO#%d for smc91x IRQ\n",
			OMAP24XX_ETHR_GPIO_IRQ);
		return;
	}
	set_irq_type(OMAP_GPIO_IRQ(OMAP24XX_ETHR_GPIO_IRQ),
				 IRQT_FALLING);
	/* Set pin M15 to mux mode 3 -> GPIO#92, disable PU/PD */
	/* FIXME: change this using omap2 mux api when it's ready */
	l = __raw_readl(0xd8000000 + 0x108);
	l &= ~(((1 << 5) - 1) << 16);
	l |= 3 << 16;
	__raw_writel(l, 0xd8000000 + 0x108);
}

static void __init omap_h4_init_irq(void)
{
	omap_init_irq();
	omap_gpio_init();
	h4_init_smc91x();
}

static struct omap_uart_config h4_uart_config __initdata = {
	.enabled_uarts = ((1 << 0) | (1 << 1) | (1 << 2)),
};

static struct omap_mmc_config h4_mmc_config __initdata = {
	.mmc [0] = {
		.enabled 	= 1,
		.wire4		= 1,
		.wp_pin		= -1,
		.power_pin	= -1,
		.switch_pin	= -1,
	},
};

static struct omap_board_config_kernel h4_config[] = {
	{ OMAP_TAG_UART,	&h4_uart_config },
	{ OMAP_TAG_MMC,		&h4_mmc_config },
};

static void __init omap_h4_init(void)
{
	/*
	 * Make sure the serial ports are muxed on at this point.
	 * You have to mux them off in device drivers later on
	 * if not needed.
	 */
	platform_add_devices(h4_devices, ARRAY_SIZE(h4_devices));
	omap_board_config = h4_config;
	omap_board_config_size = ARRAY_SIZE(h4_config);
	omap_serial_init();
}

static void __init omap_h4_map_io(void)
{
	omap_map_common_io();
}

MACHINE_START(OMAP_H4, "OMAP2420 H4 board")
	/* Maintainer: Paul Mundt <paul.mundt@nokia.com> */
	.phys_ram	= 0x80000000,
	.phys_io	= 0x48000000,
	.io_pg_offst	= ((0xd8000000) >> 18) & 0xfffc,
	.boot_params	= 0x80000100,
	.map_io		= omap_h4_map_io,
	.init_irq	= omap_h4_init_irq,
	.init_machine	= omap_h4_init,
	.timer		= &omap_timer,
MACHINE_END