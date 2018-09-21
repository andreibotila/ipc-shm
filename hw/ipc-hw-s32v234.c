/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Copyright (C) 2018 NXP Semiconductors
 */
#include <linux/types.h>
#include <linux/module.h>
#include <linux/io.h>

#include "ipc-hw.h"

/* Hardware IP Block Base Addresses - TODO: get them from device tree */
#define MSCM_BASE    0x40081000ul /* Miscellaneous System Control Module */

/* S32V234 Specific Definitions */
#define DEFAULT_SHM_IRQ_ID    3u /* MSCM interrupt 3 - GIC irq 35 */
#define DEFAULT_REMOTE_CPU    0u /* M4 core */

/* Device tree MSCM node: compatible property (search key) */
#define DT_MSCM_NODE_COMP "fsl,s32v234-mscm"

/* S32V234 Processor IDs */
enum s32v234_processor_idx {
	M4 = 0, /* ARM Cortex-M4 core */
	A53 = 1, /* ARM Cortex-A53 cluster */
};

/**
 * struct mscm_regs - MSCM Peripheral Register Structure
 *
 * @cpxtype:        Processor x Type Register, offset 0x0
 * @cpxnum:         Processor x Number Register, offset 0x4
 * @cpxmaster:      Processor x Master Number Register, offset 0x8
 * @cpxcount:       Processor x Count Register, offset 0xC
 * @cpxcfg[n]:      Processor x Configuration n Register, offset 0x10 + 4*n
 * @cp0type:        Processor 0 Type Register, offset 0x20
 * @cp0num:         Processor 0 Number Register, offset 0x24
 * @cp0master:      Processor 0 Master Number Register, offset 0x28
 * @cp0count:       Processor 0 Count Register, offset 0x2C
 * @cp0cfg[n]:      Processor 0 Configuration n Register, offset 0x30 + 4*n
 * @cp1type:        Processor 1 Type Register, offset 0x40
 * @cp1num:         Processor 1 Number Register, offset 0x44
 * @cp1master:      Processor 1 Master Number Register, offset 0x48
 * @cp1count:       Processor 1 Count Register, offset 0x4C
 * @cp1cfg[n]:      Processor 1 Configuration n Register, offset 0x50 + 4*n
 * @ocmdr[n]:       On-Chip Memory Descriptor Register n, offset 0x400 + 4*n
 * @tcmdr0:         Generic Tightly Coupled Memory Descriptor Register,
 *                  offset 0x480
 * @cpce0:          Core Parity Checking Enable Register 0, offset 0x500
 * @ircp0ir:        Interrupt Router CP0 Interrupt Register, offset 0x800
 * @ircp1ir:        Interrupt Router CP1 Interrupt Register, offset 0x804
 * @ircpgir:        Interrupt Router CPU Generate Interrupt Register,
 *                  offset 0x820
 * @irsprc[n]:      Interrupt Router Shared Peripheral Routing Control
 *                  Register n, offset 0x880 + 2*n
 * @ipcge:          Interconnect Parity Checking Global Enable Register,
 *                  offset 0xD00
 * @ipce[n]:        Interconnect Parity Checking Enable Register n,
 *                  offset 0xD10 + 4*n
 * @ipcgie:         Interconnect Parity Checking Global Injection Enable
 *                  Register, offset 0xD40
 * @ipcie[n]:       Interconnect Parity Checking Injection Enable Register n,
 *                  offset 0xD50 + 4*n
 */
struct mscm_regs {
	volatile const uint32_t cpxtype;
	volatile const uint32_t cpxnum;
	volatile const uint32_t cpxmaster;
	volatile const uint32_t cpxcount;
	volatile const uint32_t cpxcfg[4];
	volatile const uint32_t cp0type;
	volatile const uint32_t cp0num;
	volatile const uint32_t cp0master;
	volatile const uint32_t cp0count;
	volatile const uint32_t cp0cfg[4];
	volatile const uint32_t cp1type;
	volatile const uint32_t cp1num;
	volatile const uint32_t cp1master;
	volatile const uint32_t cp1count;
	volatile const uint32_t cp1cfg[4];
	uint8_t reserved00[928]; /* 0x3A8 */
	volatile uint32_t ocmdr[4];
	uint8_t reserved01[112]; /* 0x70 */
	volatile uint32_t tcmdr0;
	uint8_t reserved02[124]; /* 0x7C */
	volatile uint32_t cpce0;
	uint8_t reserved03[764]; /* 0x2FC */
	volatile uint32_t ircp0ir;
	volatile uint32_t ircp1ir;
	uint8_t reserved04[24]; /* 0x18 */
	volatile uint32_t ircpgir;
	uint8_t reserved05[92]; /* 0x5C */
	volatile uint16_t irsprc[175];
	uint8_t reserved06[800]; /* 0x320 */
	volatile uint32_t ipcge;
	uint8_t reserved07[12]; /* 0xC */
	volatile uint32_t ipce[4];
	uint8_t reserved08[32]; /* 0x20 */
	volatile uint32_t ipcgie;
	uint8_t reserved09[12]; /* 0xC */
	volatile uint32_t ipcie[4];
};

/* MSCM Hardware Register Bit Fields Definitions */

#define MSCM_IRCPxIR_INT(n)    (1u << n) /* Interrupt Router CPx Interrupt n */

#define MSCM_IRCPGIR_TLF_MASK      0x03000000ul /* Target List Field */
#define MSCM_IRCPGIR_CPUTL_MASK    0x000F0000ul /* CPU Target List */
#define MSCM_IRCPGIR_INTID_MASK    0x00000003ul /* Interrupt MSCM ID */

#define MSCM_IRCPGIR_TLF(n)      ((n << 24u) & MSCM_IRCPGIR_TLF_MASK)
#define MSCM_IRCPGIR_CPUTL(n)    (((1u << n) << 16u) & MSCM_IRCPGIR_CPUTL_MASK)
#define MSCM_IRCPGIR_INTID(n)    ((n <<  0u) & MSCM_IRCPGIR_INTID_MASK)

#define MSCM_IRCPGIR_TLF_CPUTL    0u /* CPUTL field used to direct interrupts */
#define MSCM_IRCPGIR_TLF_OTHER    1u /* Interrupt directed to all other cores */
#define MSCM_IRCPGIR_TLF_SELF     2u /* Interrupt directed to requesting core */

#define MSCM_IRSPRCn_RO    0x8000u /* Interrupt Routing Control Lock Bit */

#define MSCM_IRSPRCn_CPxE(n)    (1u << n) /* Interrupt Steering Enable fo CPx */

/* S32V234 Platform Specific Implementation  */

/**
 * struct ipc_hw_priv - ipc shm private interrupt and core data
 *
 * @inter_cpu_irq:    inter-core interrupt reserved for shm driver
 * @remote_cpu:       remote cpu to trigger the interrupt on
 * @mscm:	      pointer to memory-mapped hardware peripheral MSCM
 */
static struct ipc_hw_priv {
	int inter_cpu_irq;
	int remote_cpu;
	struct mscm_regs *mscm;
} priv;

/**
 * ipc_shm_hw_get_dt_comp() - device tree compatible getter
 *
 * Return: MSCM compatible value string for current platform
 */
char *ipc_hw_get_dt_comp(void)
{
	return DT_MSCM_NODE_COMP;
}

/**
 * ipc_shm_hw_get_dt_irq() - device tree compatible getter
 *
 * Return: device tree index of the MSCM interrupt used
 */
int ipc_hw_get_dt_irq(void)
{
	return priv.inter_cpu_irq;
}

/**
 * ipc_shm_hw_init() - map MSCM IP block to proper address
 *
 * If the value -1 is passed for either inter_cpu_irq or remote-cpu
 * the default value defined for the selected platform will be used instead
 *
 * Return: 0 for success, -ENOMEM for invalid interrupt ID or
 * -EINVAL for invalid inter core interrupt or invalid remote cpu
 */
int ipc_hw_init(const struct ipc_shm_cfg *cfg)
{
	if (cfg->inter_cpu_irq < -1 || cfg->inter_cpu_irq > 3
		|| cfg->remote_cpu != M4) {
		return -EINVAL;
	}

	if (cfg->inter_cpu_irq == PLATFORM_DEFAULT) {
		priv.inter_cpu_irq = DEFAULT_SHM_IRQ_ID;
	} else {
		priv.inter_cpu_irq = cfg->inter_cpu_irq;
	}

	if (cfg->remote_cpu == PLATFORM_DEFAULT) {
		priv.remote_cpu = DEFAULT_REMOTE_CPU;
	} else {
		priv.remote_cpu = cfg->remote_cpu;
	}

	/* map MSCM hardware peripheral block */
	priv.mscm = (struct mscm_regs *) ioremap_nocache(
		(phys_addr_t)MSCM_BASE, sizeof(struct mscm_regs));

	if (!priv.mscm) {
		return -ENOMEM;
	}

	return 0;
}

/**
 * ipc_shm_hw_free() - unmap MSCM IP block
 */
void ipc_hw_free(void)
{
	/* unmap MSCM hardware peripheral block */
	iounmap(priv.mscm);
}

/**
 * ipc_shm_hw_irq_enable() - enable notifications from remote
 *
 * Return: 0 for success
 */
int ipc_hw_irq_enable(void)
{
	uint16_t irsprc_mask;

	/* enable MSCM core-to-core interrupt routing */
	irsprc_mask = readw_relaxed(&priv.mscm->irsprc[priv.inter_cpu_irq]);
	writew_relaxed(irsprc_mask | MSCM_IRSPRCn_CPxE(A53),
			&priv.mscm->irsprc[priv.inter_cpu_irq]);

	return 0;
}

/**
 * ipc_shm_hw_irq_disable() - disable notifications from remote
 *
 * Return: 0 for success
 */
int ipc_hw_irq_disable(void)
{
	uint16_t irsprc_mask;

	/* disable MSCM core-to-core interrupt routing */
	irsprc_mask = readw_relaxed(&priv.mscm->irsprc[priv.inter_cpu_irq]);
	writew_relaxed(irsprc_mask & ~MSCM_IRSPRCn_CPxE(A53),
			&priv.mscm->irsprc[priv.inter_cpu_irq]);

	return 0;
}

/**
 * ipc_shm_hw_irq_notify() - notify remote that data is available
 *
 * Return: 0 for success
 */
int ipc_hw_irq_notify(void)
{
	/* trigger MSCM core-to-core interrupt */
	writel_relaxed(MSCM_IRCPGIR_TLF(MSCM_IRCPGIR_TLF_CPUTL) |
			MSCM_IRCPGIR_CPUTL(priv.remote_cpu) |
			MSCM_IRCPGIR_INTID(priv.inter_cpu_irq),
			&priv.mscm->ircpgir);

	return 0;
}

/**
 * ipc_shm_hw_irq_clear() - clear available data notification
 *
 * Return: 0 for success
 */
int ipc_hw_irq_clear(void)
{
	/* clear MSCM core-to-core interrupt */
	writel_relaxed(MSCM_IRCPxIR_INT(priv.inter_cpu_irq),
		       &priv.mscm->ircp1ir);

	return 0;
}
