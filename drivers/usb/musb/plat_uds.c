/*****************************************************************
 * Copyright 2005 Mentor Graphics Corporation
 * Copyright (C) 2005-2006 by Texas Instruments
 * Copyright (C) 2006 by Nokia Corporation
 *
 * This file is part of the Inventra Controller Driver for Linux.
 *
 * The Inventra Controller Driver for Linux is free software; you
 * can redistribute it and/or modify it under the terms of the GNU
 * General Public License version 2 as published by the Free Software
 * Foundation.
 *
 * The Inventra Controller Driver for Linux is distributed in
 * the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public
 * License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with The Inventra Controller Driver for Linux ; if not,
 * write to the Free Software Foundation, Inc., 59 Temple Place,
 * Suite 330, Boston, MA  02111-1307  USA
 *
 * ANY DOWNLOAD, USE, REPRODUCTION, MODIFICATION OR DISTRIBUTION
 * OF THIS DRIVER INDICATES YOUR COMPLETE AND UNCONDITIONAL ACCEPTANCE
 * OF THOSE TERMS.THIS DRIVER IS PROVIDED "AS IS" AND MENTOR GRAPHICS
 * MAKES NO WARRANTIES, EXPRESS OR IMPLIED, RELATED TO THIS DRIVER.
 * MENTOR GRAPHICS SPECIFICALLY DISCLAIMS ALL IMPLIED WARRANTIES
 * OF MERCHANTABILITY; FITNESS FOR A PARTICULAR PURPOSE AND
 * NON-INFRINGEMENT.  MENTOR GRAPHICS DOES NOT PROVIDE SUPPORT
 * SERVICES OR UPDATES FOR THIS DRIVER, EVEN IF YOU ARE A MENTOR
 * GRAPHICS SUPPORT CUSTOMER.
 ******************************************************************/

/*
 * Inventra (Multipoint) Dual-Role Controller Driver for Linux.
 *
 * This consists of a Host Controller Driver (HCD) and a peripheral
 * controller driver implementing the "Gadget" API; OTG support is
 * in the works.  These are normal Linux-USB controller drivers which
 * use IRQs and have no dedicated thread.
 *
 * This version of the driver has only been used with products from
 * Texas Instruments.  Those products integrate the Inventra logic
 * with other DMA, IRQ, and bus modules, as well as other logic that
 * needs to be reflected in this driver.
 *
 *
 * NOTE:  the original Mentor code here was pretty much a collection
 * of mechanisms that don't seem to have been fully integrated/working
 * for *any* Linux kernel version.  This version aims at Linux 2.6.now,
 * Key open issues include:
 *
 *  - Lack of host-side transaction scheduling, for all transfer types.
 *    The hardware doesn't do it; instead, software must.
 *
 *    This is not an issue for OTG devices that don't support external
 *    hubs, but for more "normal" USB hosts it's a user issue that the
 *    "multipoint" support doesn't scale in the expected ways.  That
 *    includes DaVinci EVM in a common non-OTG mode.
 *
 *      * Control and bulk use dedicated endpoints, and there's as
 *        yet no mechanism to either (a) reclaim the hardware when
 *        peripherals are NAKing, which gets complicated with bulk
 *        endpoints, or (b) use more than a single bulk endpoint in
 *        each direction.
 *
 *        RESULT:  one device may be perceived as blocking another one.
 *
 *      * Interrupt and isochronous will dynamically allocate endpoint
 *        hardware, but (a) there's no record keeping for bandwidth;
 *        (b) in the common case that few endpoints are available, there
 *        is no mechanism to reuse endpoints to talk to multiple devices.
 *
 *        RESULT:  At one extreme, bandwidth can be overcommitted in
 *        some hardware configurations, no faults will be reported.
 *        At the other extreme, the bandwidth capabilities which do
 *        exist tend to be severely undercommitted.  You can't yet hook
 *        up both a keyboard and a mouse to an external USB hub.
 *
 *      * Host side doesn't understand that hardware endpoints have two
 *        directions, so it uses only half the resources available on
 *        chips like DaVinci or TUSB 6010.
 *
 *		+++	PARTIALLY RESOLVED	+++
 *
 *        RESULT:  On DaVinci (and TUSB 6010), only one external device may
 *        use periodic transfers, other than the hub used to connect it.
 *        (And if it were to understand, there would still be limitations
 *        because of the lack of periodic endpoint scheduling.)
 *
 *  - Host-side doesn't use the HCD framework, even the older version in
 *    the 2.6.10 kernel, which doesn't provide per-endpoint URB queues.
 *
 *		+++	PARTIALLY RESOLVED	+++
 *
 *    RESULT:  code bloat, because it provides its own root hub;
 *    correctness issues.
 *
 *  - Provides its own OTG bits.  These are untested, and many of them
 *    seem to be superfluous code bloat given what usbcore does.  (They
 *    have now been partially removed.)
 */

/*
 * This gets many kinds of configuration information:
 *	- Kconfig for everything user-configurable
 *	- <asm/arch/hdrc_cnf.h> for SOC or family details
 *	- platform_device for addressing, irq, and platform_data
 *	- platform_data is mostly for board-specific informarion
 *
 * Most of the conditional compilation will (someday) vanish.
 */

#include <linux/config.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/list.h>
#include <linux/kobject.h>
#include <linux/platform_device.h>
#include <linux/clk.h>

#include <asm/io.h>

#ifdef	CONFIG_ARM
#include <asm/arch/hardware.h>
#include <asm/arch/memory.h>
#include <asm/mach-types.h>
#endif

#include "musbdefs.h"
// #ifdef CONFIG_USB_MUSB_HDRC_HCD
#define VBUSERR_RETRY_COUNT	2		/* is this too few? */
// #endif


#ifdef CONFIG_ARCH_DAVINCI
#include "davinci.h"
#endif



#if MUSB_DEBUG > 0
unsigned debug = MUSB_DEBUG;
module_param(debug, uint, 0);
MODULE_PARM_DESC(debug, "initial debug message level");

#define MUSB_VERSION_SUFFIX	"/dbg"
#endif

#define DRIVER_AUTHOR "Mentor Graphics Corp. and Texas Instruments"
#define DRIVER_DESC "Inventra Dual-Role USB Controller Driver"

#define MUSB_VERSION_BASE "2.2a/db-0.5.1"

#ifndef MUSB_VERSION_SUFFIX
#define MUSB_VERSION_SUFFIX	""
#endif
#define MUSB_VERSION	MUSB_VERSION_BASE MUSB_VERSION_SUFFIX

#define DRIVER_INFO DRIVER_DESC ", v" MUSB_VERSION

const char musb_driver_name[] = "musb_hdrc";

MODULE_DESCRIPTION(DRIVER_INFO);
MODULE_AUTHOR(DRIVER_AUTHOR);
MODULE_LICENSE("GPL");

/* time (millseconds) to wait before a restart */
#define MUSB_RESTART_TIME        5000

/* how many babbles to allow before giving up */
#define MUSB_MAX_BABBLE_COUNT    10


/*-------------------------------------------------------------------------*/

static inline struct musb *dev_to_musb(struct device *dev)
{
#ifdef CONFIG_USB_MUSB_HDRC_HCD
	/* usbcore insists dev->driver_data is a "struct hcd *" */
	return hcd_to_musb(dev_get_drvdata(dev));
#else
	return dev_get_drvdata(dev);
#endif
}

static void otg_input_changed(struct musb * pThis, u8 devctl, u8 reset,
			u8 connection, u8 suspend)
{
#ifdef CONFIG_USB_MUSB_OTG
	struct otg_machine	*otgm = &pThis->OtgMachine;
	MGC_OtgMachineInputs Inputs;

	/* reading suspend state from Power register does NOT work */
	memset(&Inputs, 0, sizeof(Inputs));

	Inputs.bSession = (devctl & MGC_M_DEVCTL_SESSION) ? TRUE : FALSE;
	Inputs.bSuspend = suspend;
	Inputs.bConnection = connection;
	Inputs.bReset = reset;
	Inputs.bConnectorId = (devctl & MGC_M_DEVCTL_BDEVICE) ? TRUE : FALSE;

	MGC_OtgMachineInputsChanged(otgm, &Inputs);
#endif
}

static void otg_input_changed_X(struct musb * pThis, u8 bVbusError, u8 bConnect)
{
#ifdef CONFIG_USB_MUSB_OTG
	MGC_OtgMachineInputs Inputs;
	void __iomem *pBase = pThis->pRegs;
	u8 devctl = musb_readb(pBase, MGC_O_HDRC_DEVCTL);
	u8 power = musb_readb(pBase, MGC_O_HDRC_POWER);

	DBG(2, "<== power %02x, devctl %02x%s%s\n", power, devctl,
			bConnect ? ", bcon" : "",
			bVbusError ? ", vbus_error" : "");

	/* speculative */
	memset(&Inputs, 0, sizeof(Inputs));
	Inputs.bSession = (devctl & MGC_M_DEVCTL_SESSION) ? TRUE : FALSE;
	Inputs.bConnectorId = (devctl & MGC_M_DEVCTL_BDEVICE) ? TRUE : FALSE;
	Inputs.bReset = (power & MGC_M_POWER_RESET) ? TRUE : FALSE;
	Inputs.bConnection = bConnect;
	Inputs.bVbusError = bVbusError;
	Inputs.bSuspend = (power & MGC_M_POWER_SUSPENDM) ? TRUE : FALSE;
	MGC_OtgMachineInputsChanged(&(pThis->OtgMachine), &Inputs);
#endif				/* CONFIG_USB_MUSB_OTG */
}


/*-------------------------------------------------------------------------*/

#ifndef CONFIG_USB_TUSB6010
/*
 * Load an endpoint's FIFO
 */
void musb_write_fifo(struct musb_hw_ep *hw_ep, u16 wCount, const u8 *pSource)
{
	void __iomem *fifo = hw_ep->fifo;

	prefetch((u8 *)pSource);

	DBG(4, "%cX ep%d fifo %p count %d buf %p\n",
			'T', hw_ep->bLocalEnd, fifo, wCount, pSource);

	/* we can't assume unaligned reads work */
	if (likely((0x01 & (unsigned long) pSource) == 0)) {
		u16	index = 0;

		/* best case is 32bit-aligned source address */
		if ((0x02 & (unsigned long) pSource) == 0) {
			if (wCount >= 4) {
				writesl(fifo, pSource + index, wCount >> 2);
				index += wCount & ~0x03;
			}
			if (wCount & 0x02) {
				musb_writew(fifo, 0, *(u16*)&pSource[index]);
				index += 2;
			}
		} else {
			if (wCount >= 2) {
				writesw(fifo, pSource + index, wCount >> 1);
				index += wCount & ~0x01;
			}
		}
		if (wCount & 0x01)
			musb_writeb(fifo, 0, pSource[index]);
	} else  {
		/* byte aligned */
		writesb(fifo, pSource, wCount);
	}
}

/*
 * Unload an endpoint's FIFO
 */
void musb_read_fifo(struct musb_hw_ep *hw_ep, u16 wCount, u8 *pDest)
{
	void __iomem *fifo = hw_ep->fifo;

	DBG(4, "%cX ep%d fifo %p count %d buf %p\n",
			'R', hw_ep->bLocalEnd, fifo, wCount, pDest);

	/* we can't assume unaligned writes work */
	if (likely((0x01 & (unsigned long) pDest) == 0)) {
		u16	index = 0;

		/* best case is 32bit-aligned destination address */
		if ((0x02 & (unsigned long) pDest) == 0) {
			if (wCount >= 4) {
				readsl(fifo, pDest, wCount >> 2);
				index = wCount & ~0x03;
			}
			if (wCount & 0x02) {
				*(u16*)&pDest[index] = musb_readw(fifo, 0);
				index += 2;
			}
		} else {
			if (wCount >= 2) {
				readsw(fifo, pDest, wCount >> 1);
				index = wCount & ~0x01;
			}
		}
		if (wCount & 0x01)
			pDest[index] = musb_readb(fifo, 0);
	} else  {
		/* byte aligned */
		readsb(fifo, pDest, wCount);
	}
}

#endif	/* normal PIO */


/*-------------------------------------------------------------------------*/

/* for high speed test mode; see USB 2.0 spec 7.1.20 */
static const u8 musb_test_packet[53] = {
	/* implicit SYNC then DATA0 to start */

	/* JKJKJKJK x9 */
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	/* JJKKJJKK x8 */
	0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa,
	/* JJJJKKKK x8 */
	0xee, 0xee, 0xee, 0xee, 0xee, 0xee, 0xee, 0xee,
	/* JJJJJJJKKKKKKK x8 */
	0xfe, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
	/* JJJJJJJK x8 */
	0x7f, 0xbf, 0xdf, 0xef, 0xf7, 0xfb, 0xfd,
	/* JKKKKKKK x10, JK */
	0xfc, 0x7e, 0xbf, 0xdf, 0xef, 0xf7, 0xfb, 0xfd, 0x7e

	/* implicit CRC16 then EOP to end */
};

void musb_load_testpacket(struct musb *musb)
{
	MGC_SelectEnd(musb->pRegs, 0);
	musb_write_fifo(musb->control_ep,
			sizeof(musb_test_packet), musb_test_packet);
	musb_writew(musb->pRegs, MGC_O_HDRC_CSR0, MGC_M_CSR0_TXPKTRDY);
}

/*-------------------------------------------------------------------------*/

/*
 * Interrupt Service Routine to record USB "global" interrupts.
 * Since these do not happen often and signify things of
 * paramount importance, it seems OK to check them individually;
 * the order of the tests is specified in the manual
 *
 * @param pThis instance pointer
 * @param bIntrUSB register contents
 * @param devctl
 * @param power
 */

#define STAGE0_MASK (MGC_M_INTR_RESUME | MGC_M_INTR_SESSREQ \
		| MGC_M_INTR_VBUSERROR | MGC_M_INTR_CONNECT \
		| MGC_M_INTR_RESET )

static irqreturn_t musb_stage0_irq(struct musb * pThis, u8 bIntrUSB,
				u8 devctl, u8 power)
{
	irqreturn_t handled = IRQ_NONE;
#ifdef CONFIG_USB_MUSB_HDRC_HCD
	void __iomem *pBase = pThis->pRegs;
#endif

	DBG(3, "<== Power=%02x, DevCtl=%02x, bIntrUSB=0x%x\n", power, devctl,
		bIntrUSB);

	/* in host mode when a device resume me (from power save)
	 * in device mode when the host resume me; it shold not change
	 * "identity".
	 */
	if (bIntrUSB & MGC_M_INTR_RESUME) {
		handled = IRQ_HANDLED;
		DBG(3, "RESUME\n");
		pThis->is_active = 1;

		if (devctl & MGC_M_DEVCTL_HM) {
#ifdef CONFIG_USB_MUSB_HDRC_HCD
			/* REVISIT:  this is where SRP kicks in, yes?
			 * host responsibility should be to CLEAR the
			 * resume signaling after 50 msec ...
			 */
			MUSB_HST_MODE(pThis);	/* unnecessary */
			power &= ~MGC_M_POWER_SUSPENDM;
			musb_writeb(pBase, MGC_O_HDRC_POWER,
				power | MGC_M_POWER_RESUME);

			/* should now be A_SUSPEND */
			pThis->xceiv.state = OTG_STATE_A_HOST;
#endif
		} else {
#ifdef CONFIG_USB_GADGET_MUSB_HDRC
			MUSB_DEV_MODE(pThis);	/* unnecessary */
#endif
			musb_g_resume(pThis);
		}
	}

#ifdef CONFIG_USB_MUSB_HDRC_HCD
	/* see manual for the order of the tests */
	if (bIntrUSB & MGC_M_INTR_SESSREQ) {
		DBG(1, "SESSION_REQUEST (%d)\n", pThis->xceiv.state);

		/* IRQ arrives from ID pin sense or (later, if VBUS power
		 * is removed) SRP.  responses are time critical:
		 *  - turn on VBUS (with silicon-specific mechanism)
		 *  - go through A_WAIT_VRISE
		 *  - ... to A_WAIT_BCON.
		 * a_wait_vrise_tmout triggers VBUS_ERROR transitions
		 */
		musb_writeb(pBase, MGC_O_HDRC_DEVCTL, MGC_M_DEVCTL_SESSION);
		pThis->bEnd0Stage = MGC_END0_START;
		pThis->xceiv.state = OTG_STATE_A_IDLE;
		MUSB_HST_MODE(pThis);

		handled = IRQ_HANDLED;

#ifdef CONFIG_USB_MUSB_OTG
		{
		MGC_OtgMachineInputs Inputs;
		memset(&Inputs, 0, sizeof(Inputs));
		Inputs.bSession = TRUE;
		Inputs.bConnectorId = FALSE;
		Inputs.bReset = FALSE;
		Inputs.bConnection = FALSE;
		Inputs.bSuspend = FALSE;
		MGC_OtgMachineInputsChanged(&(pThis->OtgMachine), &Inputs);
		}
#endif
	}

	if (bIntrUSB & MGC_M_INTR_VBUSERROR) {

		// MGC_OtgMachineInputsChanged(otgm, &Inputs);
		// ... may need to abort otg timer ...

		DBG(1, "VBUS_ERROR (%02x)\n", devctl);

		/* after hw goes to A_IDLE, try connecting again */
		pThis->xceiv.state = OTG_STATE_A_IDLE;
		if (pThis->vbuserr_retry--)
			musb_writeb(pBase, MGC_O_HDRC_DEVCTL,
					MGC_M_DEVCTL_SESSION);
		return IRQ_HANDLED;
	} else
		pThis->vbuserr_retry = VBUSERR_RETRY_COUNT;

	if (bIntrUSB & MGC_M_INTR_CONNECT) {
		handled = IRQ_HANDLED;
		pThis->is_active = 1;

		pThis->bEnd0Stage = MGC_END0_START;

#ifdef CONFIG_USB_MUSB_OTG
		/* flush endpoints when transitioning from Device Mode */
		if (is_peripheral_active(pThis)) {
			// REVISIT HNP; just force disconnect
		}
		pThis->bDelayPortPowerOff = FALSE;
#endif
		pThis->port1_status &= ~(USB_PORT_STAT_LOW_SPEED
					|USB_PORT_STAT_HIGH_SPEED
					|USB_PORT_STAT_ENABLE
					);
		pThis->port1_status |= USB_PORT_STAT_CONNECTION
					|(USB_PORT_STAT_C_CONNECTION << 16);

		/* high vs full speed is just a guess until after reset */
		if (devctl & MGC_M_DEVCTL_LSDEV)
			pThis->port1_status |= USB_PORT_STAT_LOW_SPEED;

		usb_hcd_poll_rh_status(musb_to_hcd(pThis));

		MUSB_HST_MODE(pThis);

		/* indicate new connection to OTG machine */
		switch (pThis->xceiv.state) {
		case OTG_STATE_B_WAIT_ACON:
			pThis->xceiv.state = OTG_STATE_B_HOST;
			break;
		default:
			DBG(2, "connect in state %d\n", pThis->xceiv.state);
			/* FALLTHROUGH */
		case OTG_STATE_A_WAIT_BCON:
		case OTG_STATE_A_WAIT_VRISE:
			pThis->xceiv.state = OTG_STATE_A_HOST;
			break;
		}
		DBG(1, "CONNECT (host state %d)\n", pThis->xceiv.state);
		otg_input_changed(pThis, devctl, FALSE, TRUE, FALSE);
	}
#endif	/* CONFIG_USB_MUSB_HDRC_HCD */

	/* saved one bit: bus reset and babble share the same bit;
	 * If I am host is a babble! i must be the only one allowed
	 * to reset the bus; when in otg mode it means that I have
	 * to switch to device
	 */
	if (bIntrUSB & MGC_M_INTR_RESET) {
		if (devctl & MGC_M_DEVCTL_HM) {
			DBG(1, "BABBLE\n");

			/* REVISIT it's unclear how to handle this.  Mentor's
			 * code stopped the whole USB host, which is clearly
			 * very wrong.  For now, just expect the hardware is
			 * sane, so babbling devices also trigger a normal
			 * endpoint i/o fault (with automatic recovery).
			 * (A "babble" IRQ seems quite pointless...)
			 */

		} else {
			DBG(1, "BUS RESET\n");

			musb_g_reset(pThis);

			/* reading state from Power register doesn't work */
			otg_input_changed(pThis, devctl, TRUE, FALSE,
						(power & MGC_M_POWER_SUSPENDM)
						? TRUE : FALSE);

			sysfs_notify(&pThis->controller->kobj, NULL, "cable");
		}

		handled = IRQ_HANDLED;
	}

	return handled;
}

/*
 * Interrupt Service Routine to record USB "global" interrupts.
 * Since these do not happen often and signify things of
 * paramount importance, it seems OK to check them individually;
 * the order of the tests is specified in the manual
 *
 * @param pThis instance pointer
 * @param bIntrUSB register contents
 * @param devctl
 * @param power
 */
static irqreturn_t musb_stage2_irq(struct musb * pThis, u8 bIntrUSB,
				u8 devctl, u8 power)
{
	irqreturn_t handled = IRQ_NONE;

#if 0
/* REVISIT ... this would be for multiplexing periodic endpoints, or
 * supporting transfer phasing to prevent exceeding ISO bandwidth
 * limits of a given frame or microframe.
 *
 * It's not needed for peripheral side, which dedicates endpoints;
 * though it _might_ use SOF irqs for other purposes.
 *
 * And it's not currently needed for host side, which also dedicates
 * endpoints, relies on TX/RX interval registers, and isn't claimed
 * to support ISO transfers yet.
 */
	if (bIntrUSB & MGC_M_INTR_SOF) {
		void __iomem *pBase = pThis->pRegs;
		struct musb_hw_ep	*ep;
		u8 bEnd;
		u16 wFrame;

		DBG(6, "START_OF_FRAME\n");
		handled = IRQ_HANDLED;

		/* start any periodic Tx transfers waiting for current frame */
		wFrame = musb_readw(pBase, MGC_O_HDRC_FRAME);
		ep = pThis->aLocalEnd;
		for (bEnd = 1; (bEnd < pThis->bEndCount)
					&& (pThis->wEndMask >= (1 << bEnd));
				bEnd++, ep++) {
			// FIXME handle framecounter wraps (12 bits)
			// eliminate duplicated StartUrb logic
			if (ep->dwWaitFrame >= wFrame) {
				ep->dwWaitFrame = 0;
				printk("SOF --> periodic TX%s on %d\n",
					ep->tx_channel ? " DMA" : "",
					bEnd);
				if (!ep->tx_channel)
					musb_h_tx_start(pThis, bEnd);
				else
					cppi_hostdma_start(pThis, bEnd);
			}
		}		/* end of for loop */
	}
#endif

	if ((bIntrUSB & MGC_M_INTR_DISCONNECT) && !pThis->bIgnoreDisconnect) {
		DBG(1, "DISCONNECT as %s, devctl %02x\n",
				MUSB_MODE(pThis), devctl);
		handled = IRQ_HANDLED;
		pThis->is_active = 0;

		/* need to check it against pThis, because devctl is going
		 * to report ID low as soon as the device gets disconnected
		 */
		if (is_host_active(pThis))
			musb_root_disconnect(pThis);
		else
			musb_g_disconnect(pThis);

		/* REVISIT all OTG state machine transitions */
		otg_input_changed_X(pThis, FALSE, FALSE);

		sysfs_notify(&pThis->controller->kobj, NULL, "cable");
	}

	if (bIntrUSB & MGC_M_INTR_SUSPEND) {
		DBG(1, "SUSPEND, devctl %02x\n", devctl);
		handled = IRQ_HANDLED;

		/* peripheral suspend, may trigger HNP */
		if (!(devctl & MGC_M_DEVCTL_HM)) {
			musb_g_suspend(pThis);
#ifdef CONFIG_USB_GADGET_MUSB_HDRC
			pThis->is_active = is_otg_enabled(pThis)
					&& pThis->xceiv.gadget->b_hnp_enable;
#else
			pThis->is_active = 0;
#endif
			otg_input_changed(pThis, devctl, FALSE, FALSE, TRUE);
		} else
			pThis->is_active = 0;
	}

	return handled;
}

/*-------------------------------------------------------------------------*/

/*
* Program the HDRC to start (enable interrupts, dma, etc.).
*/
void musb_start(struct musb * pThis)
{
	void __iomem *pBase = pThis->pRegs;
	u8 state;

	DBG(2, "<==\n");

	/* TODO: always set ISOUPDATE in POWER (periph mode) and leave it on! */

	/*  Set INT enable registers, enable interrupts */
	musb_writew(pBase, MGC_O_HDRC_INTRTXE, pThis->wEndMask);
	musb_writew(pBase, MGC_O_HDRC_INTRRXE, pThis->wEndMask & 0xfffe);
	musb_writeb(pBase, MGC_O_HDRC_INTRUSBE, 0xf7);

	musb_platform_enable(pThis);

	musb_writeb(pBase, MGC_O_HDRC_TESTMODE, 0);

	/* enable high-speed/low-power and start session */
	musb_writeb(pBase, MGC_O_HDRC_POWER,
		MGC_M_POWER_SOFTCONN | MGC_M_POWER_HSENAB);

	switch (pThis->board_mode) {
	case MUSB_HOST:
	case MUSB_OTG:
		musb_writeb(pBase, MGC_O_HDRC_DEVCTL, MGC_M_DEVCTL_SESSION);
		break;
	case MUSB_PERIPHERAL:
		state = musb_readb(pBase, MGC_O_HDRC_DEVCTL);
		musb_writeb(pBase, MGC_O_HDRC_DEVCTL,
			state & ~MGC_M_DEVCTL_SESSION);
		break;
	}
}


static void musb_generic_disable(struct musb *pThis)
{
	void __iomem	*pBase = pThis->pRegs;
	u16	temp;

	/* disable interrupts */
	musb_writeb(pBase, MGC_O_HDRC_INTRUSBE, 0);
	musb_writew(pBase, MGC_O_HDRC_INTRTX, 0);
	musb_writew(pBase, MGC_O_HDRC_INTRRX, 0);

	/* off */
	musb_writeb(pBase, MGC_O_HDRC_DEVCTL, 0);

	/*  flush pending interrupts */
	temp = musb_readb(pBase, MGC_O_HDRC_INTRUSB);
	temp = musb_readw(pBase, MGC_O_HDRC_INTRTX);
	temp = musb_readw(pBase, MGC_O_HDRC_INTRRX);

}

/*
 * Make the HDRC stop (disable interrupts, etc.);
 * reversible by musb_start
 * called on gadget driver unregister
 * with controller locked, irqs blocked
 * acts as a NOP unless some role activated the hardware
 */
void musb_stop(struct musb * pThis)
{
	/* stop IRQs, timers, ... */
	musb_platform_disable(pThis);
	musb_generic_disable(pThis);
	DBG(3, "HDRC disabled\n");

#ifdef CONFIG_USB_MUSB_OTG
	if (is_otg_enabled(pThis))
		MGC_OtgMachineDestroy(&pThis->OtgMachine);
#endif

	/* FIXME
	 *  - mark host and/or peripheral drivers unusable/inactive
	 *  - disable DMA (and enable it in HdrcStart)
	 *  - make sure we can musb_start() after musb_stop(); with
	 *    OTG mode, gadget driver module rmmod/modprobe cycles that
	 *  - ...
	 */

#ifdef CONFIG_USB_MUSB_HDRC_HCD
	if (is_host_enabled(pThis)) {
		/* REVISIT aren't there some paths where this is wrong?  */
		dev_warn(pThis->controller, "%s, root hub still active\n",
				__FUNCTION__);
	}
#endif
}

static void musb_shutdown(struct platform_device *pdev)
{
	struct musb	*musb = dev_to_musb(&pdev->dev);
	unsigned long	flags;

	spin_lock_irqsave(&musb->Lock, flags);
	musb_platform_disable(musb);
	musb_generic_disable(musb);
	MUSB_ERR_MODE(musb, MUSB_ERR_SHUTDOWN);
	spin_unlock_irqrestore(&musb->Lock, flags);
}


/*-------------------------------------------------------------------------*/

/*
 * The silicon either has hard-wired endpoint configurations, or else
 * "dynamic fifo" sizing.  The driver has support for both, though at this
 * writing only the dynamic sizing is very well tested.   We use normal
 * idioms to so both modes are compile-tested, but dead code elimination
 * leaves only the relevant one in the object file.
 *
 * We don't currently use dynamic fifo setup capability to do anything
 * more than selecting one of a bunch of predefined configurations.
 */
#ifdef MUSB_C_DYNFIFO_DEF
#define	can_dynfifo()	1
#else
#define	can_dynfifo()	0
#endif

static ushort __devinitdata fifo_mode = 2;

/* "modprobe ... fifo_mode=1" etc */
module_param(fifo_mode, ushort, 0);
MODULE_PARM_DESC(fifo_mode, "initial endpoint configuration");


#define DYN_FIFO_SIZE (1<<(MUSB_C_RAM_BITS+2))

enum fifo_style { FIFO_RXTX, FIFO_TX, FIFO_RX } __attribute__ ((packed));
enum buf_mode { BUF_SINGLE, BUF_DOUBLE } __attribute__ ((packed));

struct fifo_cfg {
	u8		hw_ep_num;
	enum fifo_style	style;
	enum buf_mode	mode;
	u16		maxpacket;
};

/*
 * tables defining fifo_mode values.  define more if you like.
 * for host side, make sure both halves of ep1 are set up.
 */

/* mode 0 - fits in 2KB */
static const struct fifo_cfg __devinitdata mode_0_cfg[] = {
{ .hw_ep_num = 1, .style = FIFO_TX,   .maxpacket = 512, },
{ .hw_ep_num = 1, .style = FIFO_RX,   .maxpacket = 512, },
{ .hw_ep_num = 2, .style = FIFO_RXTX, .maxpacket = 512, },
{ .hw_ep_num = 3, .style = FIFO_RXTX, .maxpacket = 256, },
{ .hw_ep_num = 4, .style = FIFO_RXTX, .maxpacket = 256, },
};

/* mode 1 - fits in 4KB */
static const struct fifo_cfg __devinitdata mode_1_cfg[] = {
{ .hw_ep_num = 1, .style = FIFO_TX,   .maxpacket = 512, .mode = BUF_DOUBLE, },
{ .hw_ep_num = 1, .style = FIFO_RX,   .maxpacket = 512, .mode = BUF_DOUBLE, },
{ .hw_ep_num = 2, .style = FIFO_RXTX, .maxpacket = 512, .mode = BUF_DOUBLE, },
{ .hw_ep_num = 3, .style = FIFO_RXTX, .maxpacket = 256, },
{ .hw_ep_num = 4, .style = FIFO_RXTX, .maxpacket = 256, },
};

/* mode 2 - fits in 4KB */
static const struct fifo_cfg __devinitdata mode_2_cfg[] = {
{ .hw_ep_num = 1, .style = FIFO_TX,   .maxpacket = 512, },
{ .hw_ep_num = 1, .style = FIFO_RX,   .maxpacket = 512, },
{ .hw_ep_num = 2, .style = FIFO_TX,   .maxpacket = 512, },
{ .hw_ep_num = 2, .style = FIFO_RX,   .maxpacket = 512, },
{ .hw_ep_num = 3, .style = FIFO_RXTX, .maxpacket = 256, },
{ .hw_ep_num = 4, .style = FIFO_RXTX, .maxpacket = 256, },
};

/* mode 3 - fits in 4KB */
static const struct fifo_cfg __devinitdata mode_3_cfg[] = {
{ .hw_ep_num = 1, .style = FIFO_TX,   .maxpacket = 512, .mode = BUF_DOUBLE, },
{ .hw_ep_num = 1, .style = FIFO_RX,   .maxpacket = 512, .mode = BUF_DOUBLE, },
{ .hw_ep_num = 2, .style = FIFO_TX,   .maxpacket = 512, },
{ .hw_ep_num = 2, .style = FIFO_RX,   .maxpacket = 512, },
{ .hw_ep_num = 3, .style = FIFO_RXTX, .maxpacket = 256, },
{ .hw_ep_num = 4, .style = FIFO_RXTX, .maxpacket = 256, },
};


/*
 * configure a fifo; for non-shared endpoints, this may be called
 * once for a tx fifo and once for an rx fifo.
 *
 * returns negative errno or offset for next fifo.
 */
static int __devinit
fifo_setup(struct musb *musb, struct musb_hw_ep  *hw_ep,
		const struct fifo_cfg *cfg, u16 offset)
{
	void __iomem	*mbase = musb->pRegs;
	int	size = 0;
	u16	maxpacket = cfg->maxpacket;
	u16	c_off = offset >> 3;
	u8	c_size;

	/* expect hw_ep has already been zero-initialized */

	size = ffs(max(maxpacket, (u16) 8)) - 1;
	maxpacket = 1 << size;

	c_size = size - 3;
	if (cfg->mode == BUF_DOUBLE) {
		if ((offset + (maxpacket << 1)) > DYN_FIFO_SIZE)
			return -EMSGSIZE;
		c_size |= MGC_M_FIFOSZ_DPB;
	} else {
		if ((offset + maxpacket) > DYN_FIFO_SIZE)
			return -EMSGSIZE;
	}

	/* configure the FIFO */
	musb_writeb(mbase, MGC_O_HDRC_INDEX, hw_ep->bLocalEnd);

#ifdef CONFIG_USB_MUSB_HDRC_HCD
	/* EP0 reserved endpoint for control, bidirectional;
	 * EP1 reserved for bulk, two unidirection halves.
	 */
	if (hw_ep->bLocalEnd == 1)
		musb->bulk_ep = hw_ep;
	/* REVISIT error check:  be sure ep0 can both rx and tx ... */
#endif
	switch (cfg->style) {
	case FIFO_TX:
		musb_writeb(mbase, MGC_O_HDRC_TXFIFOSZ, c_size);
		musb_writew(mbase, MGC_O_HDRC_TXFIFOADD, c_off);
		hw_ep->tx_double_buffered = !!(c_size & MGC_M_FIFOSZ_DPB);
		hw_ep->wMaxPacketSizeTx = maxpacket;
		break;
	case FIFO_RX:
		musb_writeb(mbase, MGC_O_HDRC_RXFIFOSZ, c_size);
		musb_writew(mbase, MGC_O_HDRC_RXFIFOADD, c_off);
		hw_ep->rx_double_buffered = !!(c_size & MGC_M_FIFOSZ_DPB);
		hw_ep->wMaxPacketSizeRx = maxpacket;
		break;
	case FIFO_RXTX:
		musb_writeb(mbase, MGC_O_HDRC_TXFIFOSZ, c_size);
		musb_writew(mbase, MGC_O_HDRC_TXFIFOADD, c_off);
		hw_ep->rx_double_buffered = !!(c_size & MGC_M_FIFOSZ_DPB);
		hw_ep->wMaxPacketSizeRx = maxpacket;

		musb_writeb(mbase, MGC_O_HDRC_RXFIFOSZ, c_size);
		musb_writew(mbase, MGC_O_HDRC_RXFIFOADD, c_off);
		hw_ep->tx_double_buffered = hw_ep->rx_double_buffered;
		hw_ep->wMaxPacketSizeTx = maxpacket;

		hw_ep->bIsSharedFifo = TRUE;
		break;
	}

	/* NOTE rx and tx endpoint irqs aren't managed separately,
	 * which happens to be ok
	 */
	musb->wEndMask |= (1 << hw_ep->bLocalEnd);

	return offset + (maxpacket << ((c_size & MGC_M_FIFOSZ_DPB) ? 1 : 0));
}

static const struct fifo_cfg __devinitdata ep0_cfg = {
	.style = FIFO_RXTX, .maxpacket = 64,
};

static int __devinit ep_config_from_table(struct musb *musb)
{
	const struct fifo_cfg	*cfg;
	unsigned		n;
	int			offset;
	struct musb_hw_ep	*hw_ep = musb->aLocalEnd;

	switch (fifo_mode) {
	default:
		fifo_mode = 0;
		/* FALLTHROUGH */
	case 0:
		cfg = mode_0_cfg;
		n = ARRAY_SIZE(mode_0_cfg);
		break;
	case 1:
		cfg = mode_1_cfg;
		n = ARRAY_SIZE(mode_1_cfg);
		break;
	case 2:
		cfg = mode_2_cfg;
		n = ARRAY_SIZE(mode_2_cfg);
		break;
	case 3:
		cfg = mode_3_cfg;
		n = ARRAY_SIZE(mode_3_cfg);
		break;
	}

	printk(KERN_DEBUG "%s: setup fifo_mode %d\n",
			musb_driver_name, fifo_mode);


	offset = fifo_setup(musb, hw_ep, &ep0_cfg, 0);
	// assert(offset > 0)

	while (n--) {
		u8	epn = cfg->hw_ep_num;

		if (epn >= MUSB_C_NUM_EPS) {
			pr_debug( "%s: invalid ep %d\n",
					musb_driver_name, epn);
			return -EINVAL;
		}
		offset = fifo_setup(musb, hw_ep + epn, cfg++, offset);
		if (offset < 0) {
			pr_debug( "%s: mem overrun, ep %d\n",
					musb_driver_name, epn);
			return -EINVAL;
		}
		epn++;
		musb->bEndCount = max(epn, musb->bEndCount);
	}

	printk(KERN_DEBUG "%s: %d/%d max ep, %d/%d memory\n",
			musb_driver_name,
			musb->bEndCount, MUSB_C_NUM_EPS * 2 - 1,
			offset, DYN_FIFO_SIZE);

#ifdef CONFIG_USB_MUSB_HDRC_HCD
	if (!musb->bulk_ep) {
		pr_debug( "%s: missing bulk\n", musb_driver_name);
		return -EINVAL;
	}
#endif

	return 0;
}


/*
 * ep_config_from_hw - when MUSB_C_DYNFIFO_DEF is false
 * @param pThis the controller
 */
static int __devinit ep_config_from_hw(struct musb *musb)
{
	u8 bEnd = 0, reg;
	struct musb_hw_ep *pEnd;
	void *pBase = musb->pRegs;

	DBG(2, "<== static silicon ep config\n");

	/* FIXME pick up ep0 maxpacket size */

	for (bEnd = 1; bEnd < MUSB_C_NUM_EPS; bEnd++) {
		MGC_SelectEnd(pBase, bEnd);
		pEnd = musb->aLocalEnd + bEnd;

		/* read from core using indexed model */
		reg = musb_readb(pEnd->regs, 0x10 + MGC_O_HDRC_FIFOSIZE);
		if (!reg) {
			/* 0's returned when no more endpoints */
			break;
		}
		musb->bEndCount++;
		musb->wEndMask |= (1 << bEnd);

		pEnd->wMaxPacketSizeTx = 1 << (reg & 0x0f);

		/* shared TX/RX FIFO? */
		if ((reg & 0xf0) == 0xf0) {
			pEnd->wMaxPacketSizeRx = pEnd->wMaxPacketSizeTx;
			pEnd->bIsSharedFifo = TRUE;
			continue;
		} else {
			pEnd->wMaxPacketSizeRx = 1 << ((reg & 0xf0) >> 4);
			pEnd->bIsSharedFifo = FALSE;
		}

		/* FIXME set up pEnd->{rx,tx}_double_buffered */

#ifdef CONFIG_USB_MUSB_HDRC_HCD
		/* pick an RX/TX endpoint for bulk */
		if (pEnd->wMaxPacketSizeTx < 512
				|| pEnd->wMaxPacketSizeRx < 512)
			continue;

		/* REVISIT:  this algorithm is lazy, we should at least
		 * try to pick a double buffered endpoint.
		 */
		if (musb->bulk_ep)
			continue;
		musb->bulk_ep = pEnd;
#endif
	}

#ifdef CONFIG_USB_MUSB_HDRC_HCD
	if (!musb->bulk_ep) {
		pr_debug( "%s: missing bulk\n", musb_driver_name);
		return -EINVAL;
	}
#endif

	return 0;
}

enum { MUSB_CONTROLLER_MHDRC, MUSB_CONTROLLER_HDRC, };

/* Initialize MUSB (M)HDRC part of the USB hardware subsystem;
 * configure endpoints, or take their config from silicon
 */
static int __devinit musb_core_init(u16 wType, struct musb *pThis)
{
#ifdef MUSB_AHB_ID
	u32 dwData;
#endif
	u8 reg;
	char *type;
	u16 wRelease, wRelMajor, wRelMinor;
	char aInfo[78], aRevision[32], aDate[12];
	void __iomem	*pBase = pThis->pRegs;
	int		status = 0;
	int		i;

	/* log core options (read using indexed model) */
	MGC_SelectEnd(pBase, 0);
	reg = musb_readb(pBase, 0x10 + MGC_O_HDRC_CONFIGDATA);

	strcpy(aInfo, (reg & MGC_M_CONFIGDATA_UTMIDW) ? "UTMI-16" : "UTMI-8");
	if (reg & MGC_M_CONFIGDATA_DYNFIFO) {
		strcat(aInfo, ", dyn FIFOs");
	}
	if (reg & MGC_M_CONFIGDATA_MPRXE) {
		strcat(aInfo, ", bulk combine");
#ifdef C_MP_RX
		pThis->bBulkCombine = TRUE;
#else
		strcat(aInfo, " (X)");		/* no driver support */
#endif
	}
	if (reg & MGC_M_CONFIGDATA_MPTXE) {
		strcat(aInfo, ", bulk split");
#ifdef C_MP_TX
		pThis->bBulkSplit = TRUE;
#else
		strcat(aInfo, " (X)");		/* no driver support */
#endif
	}
	if (reg & MGC_M_CONFIGDATA_HBRXE) {
		strcat(aInfo, ", HB-ISO Rx");
		strcat(aInfo, " (X)");		/* no driver support */
	}
	if (reg & MGC_M_CONFIGDATA_HBTXE) {
		strcat(aInfo, ", HB-ISO Tx");
		strcat(aInfo, " (X)");		/* no driver support */
	}
	if (reg & MGC_M_CONFIGDATA_SOFTCONE) {
		strcat(aInfo, ", SoftConn");
	}

	printk(KERN_DEBUG "%s: ConfigData=0x%02x (%s)\n",
			musb_driver_name, reg, aInfo);

#ifdef MUSB_AHB_ID
	dwData = musb_readl(pBase, 0x404);
	sprintf(aDate, "%04d-%02x-%02x", (dwData & 0xffff),
		(dwData >> 16) & 0xff, (dwData >> 24) & 0xff);
	/* FIXME ID2 and ID3 are unused */
	dwData = musb_readl(pBase, 0x408);
	printk("ID2=%lx\n", (long unsigned)dwData);
	dwData = musb_readl(pBase, 0x40c);
	printk("ID3=%lx\n", (long unsigned)dwData);
	reg = musb_readb(pBase, 0x400);
	wType = ('M' == reg) ? MUSB_CONTROLLER_MHDRC : MUSB_CONTROLLER_HDRC;
#else
	aDate[0] = 0;
#endif
	if (MUSB_CONTROLLER_MHDRC == wType) {
		pThis->bIsMultipoint = 1;
		type = "M";
	} else {
		pThis->bIsMultipoint = 0;
		type = "";
#ifdef CONFIG_USB_MUSB_HDRC_HCD
#ifndef	CONFIG_USB_OTG_BLACKLIST_HUB
		printk(KERN_ERR
			"%s: kernel must blacklist external hubs\n",
			musb_driver_name);
#endif
#endif
	}

	/* log release info */
	wRelease = musb_readw(pBase, MGC_O_HDRC_HWVERS);
	wRelMajor = (wRelease >> 10) & 0x1f;
	wRelMinor = wRelease & 0x3ff;
	snprintf(aRevision, 32, "%d.%d%s", wRelMajor,
		wRelMinor, (wRelease & 0x8000) ? "RC" : "");
	printk(KERN_DEBUG "%s: %sHDRC RTL version %s %s\n",
			musb_driver_name, type, aRevision, aDate);

	/* configure ep0 */
	pThis->aLocalEnd[0].wMaxPacketSizeTx = MGC_END0_FIFOSIZE;
	pThis->aLocalEnd[0].wMaxPacketSizeRx = MGC_END0_FIFOSIZE;

	/* discover endpoint configuration */
	pThis->bEndCount = 1;
	pThis->wEndMask = 1;

	if (reg & MGC_M_CONFIGDATA_DYNFIFO) {
		if (can_dynfifo())
			status = ep_config_from_table(pThis);
		else {
			ERR("reconfigure software for Dynamic FIFOs\n");
			status = -ENODEV;
		}
	} else {
		if (!can_dynfifo())
			status = ep_config_from_hw(pThis);
		else {
			ERR("reconfigure software for static FIFOs\n");
			return -ENODEV;
		}
	}

	if (status < 0)
		return status;

	/* finish init, and print endpoint config */
	for (i = 0; i < pThis->bEndCount; i++) {
		struct musb_hw_ep	*hw_ep = pThis->aLocalEnd + i;

		hw_ep->fifo = MUSB_FIFO_OFFSET(i) + pBase;
#ifdef CONFIG_USB_TUSB6010
		hw_ep->fifo_async = pThis->async + 0x400 + MUSB_FIFO_OFFSET(i);
		hw_ep->fifo_sync = pThis->sync + 0x400 + MUSB_FIFO_OFFSET(i);
		if (i == 0)
			hw_ep->conf = pBase - 0x400 + TUSB_EP0_CONF;
		else
			hw_ep->conf = pBase + 0x400 + (((i - 1) & 0xf) << 2);
#endif

		hw_ep->regs = MGC_END_OFFSET(i, 0) + pBase;
#ifdef CONFIG_USB_MUSB_HDRC_HCD
		hw_ep->target_regs = MGC_BUSCTL_OFFSET(i, 0) + pBase;
		hw_ep->rx_reinit = 1;
		hw_ep->tx_reinit = 1;
#endif

		if (hw_ep->wMaxPacketSizeTx) {
			printk(KERN_DEBUG
				"%s: hw_ep %d%s, %smax %d\n",
				musb_driver_name, i,
				hw_ep->bIsSharedFifo ? "shared" : "tx",
				hw_ep->tx_double_buffered
					? "doublebuffer, " : "",
				hw_ep->wMaxPacketSizeTx);
		}
		if (hw_ep->wMaxPacketSizeRx && !hw_ep->bIsSharedFifo) {
			printk(KERN_DEBUG
				"%s: hw_ep %d%s, %smax %d\n",
				musb_driver_name, i,
				"rx",
				hw_ep->rx_double_buffered
					? "doublebuffer, " : "",
				hw_ep->wMaxPacketSizeRx);
		}
		if (!(hw_ep->wMaxPacketSizeTx || hw_ep->wMaxPacketSizeRx))
			DBG(1, "hw_ep %d not configured\n", i);
	}

	return 0;
}

/*-------------------------------------------------------------------------*/

#ifdef CONFIG_ARCH_OMAP243X

static irqreturn_t generic_interrupt(int irq, void *__hci, struct pt_regs *r)
{
	unsigned long	flags;
	irqreturn_t	retval = IRQ_NONE;
	struct musb	*musb = __hci;

	spin_lock_irqsave(&musb->Lock, flags);

	musb->int_usb = musb_readb(musb->pRegs, MGC_O_HDRC_INTRUSB);
	musb->int_tx = musb_readw(musb->pRegs, MGC_O_HDRC_INTRTX);
	musb->int_rx = musb_readw(musb->pRegs, MGC_O_HDRC_INTRRX);
	musb->int_regs = r;

	if (musb->int_usb || musb->int_tx || musb->int_rx)
		retval = musb_interrupt(musb);

	spin_unlock_irqrestore(&musb->Lock, flags);

	/* REVISIT we sometimes get spurious IRQs on g_ep0
	 * not clear why...
	 */
	if (retval != IRQ_HANDLED)
		DBG(5, "spurious?\n");

	return IRQ_HANDLED;
}

#else
#define generic_interrupt	NULL
#endif

/*
 * handle all the irqs defined by the HDRC core. for now we expect:  other
 * irq sources (phy, dma, etc) will be handled first, musb->int_* values
 * will be assigned, and the irq will already have been acked.
 *
 * called in irq context with spinlock held, irqs blocked
 */
irqreturn_t musb_interrupt(struct musb *musb)
{
	irqreturn_t	retval = IRQ_NONE;
	u8		devctl, power;
	int		ep_num;
	u32		reg;

	devctl = musb_readb(musb->pRegs, MGC_O_HDRC_DEVCTL);
	power = musb_readb(musb->pRegs, MGC_O_HDRC_POWER);

	DBG(4, "** IRQ %s usb%04x tx%04x rx%04x\n",
		(devctl & MGC_M_DEVCTL_HM) ? "host" : "peripheral",
		musb->int_usb, musb->int_tx, musb->int_rx);

	/* ignore requests when in error */
	if (MUSB_IS_ERR(musb)) {
		WARN("irq in error\n");
		musb_platform_disable(musb);
		return IRQ_NONE;
	}

	/* the core can interrupt us for multiple reasons; docs have
	 * a generic interrupt flowchart to follow
	 */
	if (musb->int_usb & STAGE0_MASK)
		retval |= musb_stage0_irq(musb, musb->int_usb,
				devctl, power);
	else
		musb->vbuserr_retry = VBUSERR_RETRY_COUNT;

	/* "stage 1" is handling endpoint irqs */

	/* handle endpoint 0 first */
	if (musb->int_tx & 1) {
		if (devctl & MGC_M_DEVCTL_HM)
			retval |= musb_h_ep0_irq(musb);
		else
			retval |= musb_g_ep0_irq(musb);
	}

	/* RX on endpoints 1-15 */
	reg = musb->int_rx >> 1;
	ep_num = 1;
	while (reg) {
		if (reg & 1) {
			// MGC_SelectEnd(musb->pRegs, ep_num);
			/* REVISIT just retval = ep->rx_irq(...) */
			retval = IRQ_HANDLED;
			if (devctl & MGC_M_DEVCTL_HM)
				musb_host_rx(musb, ep_num);
			else
				musb_g_rx(musb, ep_num);
		}

		reg >>= 1;
		ep_num++;
	}

	/* TX on endpoints 1-15 */
	reg = musb->int_tx >> 1;
	ep_num = 1;
	while (reg) {
		if (reg & 1) {
			// MGC_SelectEnd(musb->pRegs, ep_num);
			/* REVISIT just retval |= ep->tx_irq(...) */
			retval = IRQ_HANDLED;
			if (devctl & MGC_M_DEVCTL_HM)
				musb_host_tx(musb, ep_num);
			else
				musb_g_tx(musb, ep_num);
		}
		reg >>= 1;
		ep_num++;
	}

	/* finish handling "global" interrupts after handling fifos */
	if (musb->int_usb)
		retval |= musb_stage2_irq(musb,
				musb->int_usb, devctl, power);

	return retval;
}


#ifndef CONFIG_USB_INVENTRA_FIFO
static int __devinitdata use_dma = is_dma_capable();

/* "modprobe ... use_dma=0" etc */
module_param(use_dma, bool, 0);
MODULE_PARM_DESC(use_dma, "enable/disable use of DMA");

void musb_dma_completion(struct musb *musb, u8 bLocalEnd, u8 bTransmit)
{
	u8	devctl = musb_readb(musb->pRegs, MGC_O_HDRC_DEVCTL);

	/* called with controller lock already held */

	if (!bLocalEnd) {
#if !(defined(CONFIG_USB_TI_CPPI_DMA) || defined(CONFIG_USB_TUSB_OMAP_DMA))
		/* endpoint 0 */
		if (devctl & MGC_M_DEVCTL_HM)
			musb_h_ep0_irq(musb);
		else
			musb_g_ep0_irq(musb);
#endif
	} else {
		/* endpoints 1..15 */
		if (bTransmit) {
			if (devctl & MGC_M_DEVCTL_HM)
				musb_host_tx(musb, bLocalEnd);
			else
				musb_g_tx(musb, bLocalEnd);
		} else {
			/* receive */
			if (devctl & MGC_M_DEVCTL_HM)
				musb_host_rx(musb, bLocalEnd);
			else
				musb_g_rx(musb, bLocalEnd);
		}
	}
}

#else
#define use_dma			is_dma_capable()
#endif

/*-------------------------------------------------------------------------*/

#ifdef CONFIG_SYSFS

static ssize_t
musb_mode_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct musb *musb = dev_to_musb(dev);
	unsigned long flags;
	int ret = -EINVAL;

	spin_lock_irqsave(&musb->Lock, flags);
	switch (musb->board_mode) {
	case MUSB_HOST:
		ret = sprintf(buf, "host\n");
		break;
	case MUSB_PERIPHERAL:
		ret = sprintf(buf, "peripheral\n");
		break;
	case MUSB_OTG:
		ret = sprintf(buf, "otg\n");
		break;
	}
	spin_unlock_irqrestore(&musb->Lock, flags);

	return ret;
}
static DEVICE_ATTR(mode, S_IRUGO, musb_mode_show, NULL);

static ssize_t
musb_cable_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct musb *musb = dev_to_musb(dev);
	char *v1= "", *v2 = "?";
	unsigned long flags;
	int vbus;

	spin_lock_irqsave(&musb->Lock, flags);
#ifdef CONFIG_USB_TUSB6010
	/* REVISIT: connect-A != connect-B ... */
	vbus = musb_platform_get_vbus_status(musb);
	if (vbus)
		v2 = "connected";
	else
		v2 = "disconnected";
#else
	/* NOTE: board-specific issues, like too-big capacitors keeping
	 * VBUS high for a long time after power has been removed, can
	 * cause temporary false indications of a connection.
	 */
	vbus = musb_readb(musb->pRegs, MGC_O_HDRC_DEVCTL);
	if (vbus & 0x10) {
		/* REVISIT retest on real OTG hardware */
		switch (musb->board_mode) {
		case MUSB_HOST:
			v2 = "A";
			break;
		case MUSB_PERIPHERAL:
			v2 = "B";
			break;
		case MUSB_OTG:
			v1 = "Mini-";
			v2 = (vbus & MGC_M_DEVCTL_BDEVICE) ? "A" : "B";
			break;
		}
	} else	/* VBUS level below A-Valid */
		v2 = "disconnected";
#endif
	musb_platform_try_idle(musb);
	spin_unlock_irqrestore(&musb->Lock, flags);

	return sprintf(buf, "%s%s\n", v1, v2);
}
static DEVICE_ATTR(cable, S_IRUGO, musb_cable_show, NULL);

#endif

/* --------------------------------------------------------------------------
 * Init support
 */

static struct musb *__devinit
allocate_instance(struct device *dev, void __iomem *mbase)
{
	struct musb		*musb;
	struct musb_hw_ep	*ep;
	int			epnum;
#ifdef CONFIG_USB_MUSB_HDRC_HCD
	struct usb_hcd	*hcd;

	hcd = usb_create_hcd(&musb_hc_driver, dev, dev->bus_id);
	if (!hcd)
		return NULL;
	/* usbcore sets dev->driver_data to hcd, and sometimes uses that... */

	musb = hcd_to_musb(hcd);
	INIT_LIST_HEAD(&musb->control);
	INIT_LIST_HEAD(&musb->in_bulk);
	INIT_LIST_HEAD(&musb->out_bulk);

	hcd->uses_new_polling = 1;

#else
	musb = kzalloc(sizeof *musb, GFP_KERNEL);
	if (!musb)
		return NULL;
	dev_set_drvdata(dev, musb);

#endif

	musb->pRegs = mbase;
	musb->ctrl_base = mbase;
	musb->nIrq = -ENODEV;
	for (epnum = 0, ep = musb->aLocalEnd;
			epnum < MUSB_C_NUM_EPS;
			epnum++, ep++) {

		ep->musb = musb;
		ep->bLocalEnd = epnum;
	}

	musb->controller = dev;
	return musb;
}

static void musb_free(struct musb *musb)
{
	/* this has multiple entry modes. it handles fault cleanup after
	 * probe(), where things may be partially set up, as well as rmmod
	 * cleanup after everything's been de-activated.
	 */

#ifdef CONFIG_SYSFS
	device_remove_file(musb->controller, &dev_attr_mode);
	device_remove_file(musb->controller, &dev_attr_cable);
#endif

#ifdef CONFIG_USB_GADGET_MUSB_HDRC
	musb_gadget_cleanup(musb);
#endif

	if (musb->nIrq >= 0)
		free_irq(musb->nIrq, musb);
	if (is_dma_capable() && musb->pDmaController) {
		struct dma_controller	*c = musb->pDmaController;

//
		(void) c->stop(c->pPrivateData);
		dma_controller_factory.destroy(c);
	}
	musb_platform_exit(musb);
	if (musb->clock) {
		clk_disable(musb->clock);
		clk_put(musb->clock);
	}

#ifdef CONFIG_USB_MUSB_HDRC_HCD
	usb_put_hcd(musb_to_hcd(musb));
#else
	kfree(musb);
#endif
}

/*
 * Perform generic per-controller initialization.
 *
 * @pDevice: the controller (already clocked, etc)
 * @nIrq: irq
 * @pRegs: virtual address of controller registers,
 *	not yet corrected for platform-specific offsets
 */
static int __devinit
musb_init_controller(struct device *dev, int nIrq, void __iomem *ctrl)
{
	int			status;
	struct musb		*pThis;
	struct musb_hdrc_platform_data *plat = dev->platform_data;

	/* The driver might handle more features than the board; OK.
	 * Fail when the board needs a feature that's not enabled.
	 */
	if (!plat) {
		dev_dbg(dev, "no platform_data?\n");
		return -ENODEV;
	}
	switch (plat->mode) {
	case MUSB_HOST:
#ifdef CONFIG_USB_MUSB_HDRC_HCD
		break;
#else
		goto bad_config;
#endif
	case MUSB_PERIPHERAL:
#ifdef CONFIG_USB_GADGET_MUSB_HDRC
		break;
#else
		goto bad_config;
#endif
	case MUSB_OTG:
#ifdef CONFIG_USB_MUSB_OTG
		break;
#else
	bad_config:
#endif
	default:
		dev_dbg(dev, "incompatible Kconfig role setting\n");
		return -EINVAL;
	}

	/* allocate */
	pThis = allocate_instance(dev, ctrl);
	if (!pThis)
		return -ENOMEM;

	spin_lock_init(&pThis->Lock);
	pThis->board_mode = plat->mode;
	pThis->board_set_power = plat->set_power;

	/* assume vbus is off */

	/* platform adjusts pThis->pRegs and pThis->isr if needed,
	 * and activates clocks
	 */
	pThis->isr = generic_interrupt;
	status = musb_platform_init(pThis);

	if (status < 0)
		goto fail;
	if (!pThis->isr) {
		status = -ENODEV;
		goto fail2;
	}

#ifndef CONFIG_USB_INVENTRA_FIFO
	if (use_dma && dev->dma_mask) {
		struct dma_controller	*c;

		c = dma_controller_factory.create(pThis, pThis->pRegs);
		pThis->pDmaController = c;
		if (c)
			(void) c->start(c->pPrivateData);
	}
#endif
	/* ideally this would be abstracted in platform setup */
	if (!is_dma_capable() || !pThis->pDmaController)
		dev->dma_mask = NULL;

	/* be sure interrupts are disabled before connecting ISR */
	musb_platform_disable(pThis);

	/* setup musb parts of the core (especially endpoints) */
	status = musb_core_init(plat->multipoint
			? MUSB_CONTROLLER_MHDRC
			: MUSB_CONTROLLER_HDRC, pThis);
	if (status < 0)
		goto fail2;

	/* attach to the IRQ */
	if (request_irq (nIrq, pThis->isr, 0, dev->bus_id, pThis)) {
		dev_err(dev, "request_irq %d failed!\n", nIrq);
		status = -ENODEV;
		goto fail2;
	}
	pThis->nIrq = nIrq;
	device_init_wakeup(dev, 1);

	pr_info("%s: USB %s mode controller at %p using %s, IRQ %d\n",
			musb_driver_name,
			({char *s;
			switch (pThis->board_mode) {
			case MUSB_HOST:		s = "Host"; break;
			case MUSB_PERIPHERAL:	s = "Peripheral"; break;
			default:		s = "OTG"; break;
			}; s; }),
			ctrl,
			(is_dma_capable() && pThis->pDmaController)
				? "DMA" : "PIO",
			pThis->nIrq);

#ifdef CONFIG_USB_MUSB_HDRC_HCD
	/* host side needs more setup, except for no-host modes */
	if (pThis->board_mode != MUSB_PERIPHERAL) {
		struct usb_hcd	*hcd = musb_to_hcd(pThis);

		if (pThis->board_mode == MUSB_OTG)
			hcd->self.otg_port = 1;
		pThis->xceiv.host = &hcd->self;
		hcd->power_budget = 2 * (plat->power ? : 250);
	}
#endif				/* CONFIG_USB_MUSB_HDRC_HCD */

#ifdef CONFIG_USB_MUSB_OTG
	/* if present, this gets used even on non-otg boards */
	MGC_OtgMachineInit(&pThis->OtgMachine, pThis);
#endif

	/* For the host-only role, we can activate right away.
	 * Otherwise, wait till the gadget driver hooks up.
	 *
	 * REVISIT switch to compile-time is_role_host() etc
	 * to get rid of #ifdeffery
	 */
	switch (pThis->board_mode) {
#ifdef CONFIG_USB_MUSB_HDRC_HCD
	case MUSB_HOST:
		MUSB_HST_MODE(pThis);
		pThis->xceiv.state = OTG_STATE_A_IDLE;
		status = usb_add_hcd(musb_to_hcd(pThis), -1, 0);

		DBG(1, "%s mode, status %d, devctl %02x %c\n",
			"HOST", status,
			musb_readb(pThis->pRegs, MGC_O_HDRC_DEVCTL),
			(musb_readb(pThis->pRegs, MGC_O_HDRC_DEVCTL)
					& MGC_M_DEVCTL_BDEVICE
				? 'B' : 'A'));
		break;
#endif
#ifdef CONFIG_USB_GADGET_MUSB_HDRC
	case MUSB_PERIPHERAL:
		MUSB_DEV_MODE(pThis);
		status = musb_gadget_setup(pThis);

		DBG(1, "%s mode, status %d, dev%02x\n",
			"PERIPHERAL", status,
			musb_readb(pThis->pRegs, MGC_O_HDRC_DEVCTL));
		break;
#endif
#ifdef CONFIG_USB_MUSB_OTG
	case MUSB_OTG:
		MUSB_OTG_MODE(pThis);
		status = musb_gadget_setup(pThis);

		DBG(1, "%s mode, status %d, dev%02x\n",
			"OTG", status,
			musb_readb(pThis->pRegs, MGC_O_HDRC_DEVCTL));
#endif
		break;
	}

	if (status == 0)
		musb_debug_create("driver/musb_hdrc", pThis);
	else {
fail:
		device_init_wakeup(dev, 0);
		musb_free(pThis);
		return status;
	}

#ifdef CONFIG_SYSFS
	device_create_file(dev, &dev_attr_mode);
	device_create_file(dev, &dev_attr_cable);
#endif

	return status;

fail2:
	musb_platform_exit(pThis);
	goto fail;
}

/*-------------------------------------------------------------------------*/

/* all implementations (PCI bridge to FPGA, VLYNQ, etc) should just
 * bridge to a platform device; this driver then suffices.
 */

static int __devinit musb_probe(struct platform_device *pdev)
{
	struct device	*dev = &pdev->dev;
	int		irq = platform_get_irq(pdev, 0);
	struct resource	*iomem;
	void __iomem	*base;

	iomem = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!iomem || irq == 0)
		return -ENODEV;

	base = ioremap(iomem->start, iomem->end - iomem->start + 1);
	if (!base) {
		dev_err(dev, "ioremap failed\n");
		return -ENOMEM;
	}

	return musb_init_controller(dev, irq, base);
}

static int __devexit musb_remove(struct platform_device *pdev)
{
	struct musb	*musb = dev_to_musb(&pdev->dev);

	/* this gets called on rmmod.
	 *  - Host mode: host may still be active
	 *  - Peripheral mode: peripheral is deactivated (or never-activated)
	 *  - OTG mode: both roles are deactivated (or never-activated)
	 */
	musb_shutdown(pdev);
	musb_debug_delete("driver/musb_hdrc", musb);
#ifdef CONFIG_USB_MUSB_HDRC_HCD
	if (musb->board_mode == MUSB_HOST)
		usb_remove_hcd(musb_to_hcd(musb));
#endif
	musb_free(musb);
	device_init_wakeup(&pdev->dev, 0);
	return 0;
}

#ifdef	CONFIG_PM

static int musb_suspend(struct platform_device *pdev, pm_message_t message)
{
	unsigned long	flags;
	struct musb	*musb = dev_to_musb(&pdev->dev);

	if (!musb->clock)
		return 0;

	spin_lock_irqsave(&musb->Lock, flags);

	if (is_peripheral_active(musb)) {
		/* FIXME force disconnect unless we know USB will wake
		 * the system up quickly enough to respond ...
		 */
	} else if (is_host_active(musb)) {
		/* we know all the children are suspended; sometimes
		 * they will even be wakeup-enabled.
		 */
	}

	clk_disable(musb->clock);
	spin_unlock_irqrestore(&musb->Lock, flags);
	return 0;
}

static int musb_resume(struct platform_device *pdev)
{
	unsigned long	flags;
	struct musb	*musb = dev_to_musb(&pdev->dev);

	if (!musb->clock)
		return 0;

	spin_lock_irqsave(&musb->Lock, flags);
	clk_enable(musb->clock);
	/* for static cmos like DaVinci, register values were preserved
	 * unless for some reason the whole soc powered down and we're
	 * not treating that as a whole-system restart (e.g. swsusp)
	 */
	spin_unlock_irqrestore(&musb->Lock, flags);
	return 0;
}

#else
#define	musb_suspend	NULL
#define	musb_resume	NULL
#endif

static struct platform_driver musb_driver = {
	.driver = {
		.name		= (char *)musb_driver_name,
		.bus		= &platform_bus_type,
		.owner		= THIS_MODULE,
	},
	.probe		= musb_probe,
	.remove		= __devexit_p(musb_remove),
	.shutdown	= musb_shutdown,
	.suspend	= musb_suspend,
	.resume		= musb_resume,
};

/*-------------------------------------------------------------------------*/

static int __init musb_init(void)
{
#ifdef CONFIG_USB_MUSB_HDRC_HCD
	if (usb_disabled())
		return 0;
#endif

	pr_info("%s: version " MUSB_VERSION ", "
#ifdef CONFIG_USB_INVENTRA_FIFO
		"pio"
#elif defined(CONFIG_USB_TI_CPPI_DMA)
		"cppi-dma"
#elif defined(CONFIG_USB_INVENTRA_DMA)
		"musb-dma"
#elif defined(CONFIG_USB_TUSB_OMAP_DMA)
		"tusb-omap-dma"
#else
		"?dma?"
#endif
		", "
#ifdef CONFIG_USB_MUSB_OTG
		"otg (peripheral+host)"
#elif defined(CONFIG_USB_GADGET_MUSB_HDRC)
		"peripheral"
#elif defined(CONFIG_USB_MUSB_HDRC_HCD)
		"host"
#endif
		", debug=%d\n",
		musb_driver_name, debug);
	return platform_driver_register(&musb_driver);
}

/* make us init after usbcore and before usb
 * gadget and host-side drivers start to register
 */
subsys_initcall(musb_init);

static void __exit musb_cleanup(void)
{
	platform_driver_unregister(&musb_driver);
}
module_exit(musb_cleanup);