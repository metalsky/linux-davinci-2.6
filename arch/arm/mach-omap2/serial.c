/*
 * arch/arm/mach-omap/omap2/serial.c
 *
 * OMAP2 serial support.
 *
 * Copyright (C) 2005 Nokia Corporation
 * Author: Paul Mundt <paul.mundt@nokia.com>
 *
 * Based off of arch/arm/mach-omap/omap1/serial.c
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License. See the file "COPYING" in the main directory of this archive
 * for more details.
 */
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/serial_8250.h>
#include <linux/serial_reg.h>

#include <asm/io.h>

#include <asm/arch/common.h>
#include <asm/arch/board.h>

static struct plat_serial8250_port serial_platform_data[] = {
	{
		.membase	= (char *)IO_ADDRESS(OMAP_UART1_BASE),
		.mapbase	= (unsigned long)OMAP_UART1_BASE,
		.irq		= 72,
		.flags		= UPF_BOOT_AUTOCONF,
		.iotype		= UPIO_MEM,
		.regshift	= 2,
		.uartclk	= OMAP16XX_BASE_BAUD * 16,
	}, {
		.membase	= (char *)IO_ADDRESS(OMAP_UART2_BASE),
		.mapbase	= (unsigned long)OMAP_UART2_BASE,
		.irq		= 73,
		.flags		= UPF_BOOT_AUTOCONF,
		.iotype		= UPIO_MEM,
		.regshift	= 2,
		.uartclk	= OMAP16XX_BASE_BAUD * 16,
	}, {
		.membase	= (char *)IO_ADDRESS(OMAP_UART3_BASE),
		.mapbase	= (unsigned long)OMAP_UART3_BASE,
		.irq		= 74,
		.flags		= UPF_BOOT_AUTOCONF,
		.iotype		= UPIO_MEM,
		.regshift	= 2,
		.uartclk	= OMAP16XX_BASE_BAUD * 16,
	}, {
		.flags		= 0
	}
};

static inline unsigned int serial_read_reg(struct plat_serial8250_port *up,
					   int offset)
{
	offset <<= up->regshift;
	return (unsigned int)__raw_readb(up->membase + offset);
}

static inline void serial_write_reg(struct plat_serial8250_port *p, int offset,
				    int value)
{
	offset <<= p->regshift;
	__raw_writeb(value, (unsigned long)(p->membase + offset));
}

/*
 * Internal UARTs need to be initialized for the 8250 autoconfig to work
 * properly. Note that the TX watermark initialization may not be needed
 * once the 8250.c watermark handling code is merged.
 */
static inline void __init omap_serial_reset(struct plat_serial8250_port *p)
{
	serial_write_reg(p, UART_OMAP_MDR1, 0x07);
	serial_write_reg(p, UART_OMAP_SCR, 0x08);
	serial_write_reg(p, UART_OMAP_MDR1, 0x00);
	serial_write_reg(p, UART_OMAP_SYSC, 0x01);
}

void __init omap_serial_init()
{
	int i;
	const struct omap_uart_config *info;

	/*
	 * Make sure the serial ports are muxed on at this point.
	 * You have to mux them off in device drivers later on
	 * if not needed.
	 */

	info = omap_get_config(OMAP_TAG_UART,
			       struct omap_uart_config);

	if (info == NULL)
		return;

	for (i = 0; i < OMAP_MAX_NR_PORTS; i++) {
		struct plat_serial8250_port *p = serial_platform_data + i;

		if (!(info->enabled_uarts & (1 << i))) {
			p->membase = 0;
			p->mapbase = 0;
			continue;
		}

		omap_serial_reset(p);
	}
}

static struct platform_device serial_device = {
	.name			= "serial8250",
	.id			= 0,
	.dev			= {
		.platform_data	= serial_platform_data,
	},
};

static int __init omap_init(void)
{
	return platform_device_register(&serial_device);
}
arch_initcall(omap_init);