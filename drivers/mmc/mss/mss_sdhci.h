/*
 * mss_sdhci.h - SD Host Controller interface driver
 *
 * Copyright (C) 2007 Intel Corporation
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License Version 2 only 
 * for now as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 */

/*
 * derived from linux/drivers/mmc/sdhci.h 
 * Copyright (c) 2005 Pierre Ossman 
 */


/*
 * PCI registers
 */
#define MSS_VDD_150     0
#define MSS_VDD_155     1
#define MSS_VDD_160     2
#define MSS_VDD_165     3
#define MSS_VDD_170     4
#define MSS_VDD_180     5
#define MSS_VDD_190     6
#define MSS_VDD_200     7
#define MSS_VDD_210     8
#define MSS_VDD_220     9
#define MSS_VDD_230     10
#define MSS_VDD_240     11
#define MSS_VDD_250     12
#define MSS_VDD_260     13
#define MSS_VDD_270     14
#define MSS_VDD_280     15
#define MSS_VDD_290     16
#define MSS_VDD_300     17
#define MSS_VDD_310     18
#define MSS_VDD_320     19
#define MSS_VDD_330     20
#define MSS_VDD_340     21
#define MSS_VDD_350     22
#define MSS_VDD_360     23

#define PCI_SDHCI_IFPIO			0x00
#define PCI_SDHCI_IFDMA			0x01
#define PCI_SDHCI_IFVENDOR		0x02

#define PCI_SLOT_INFO			0x40	/* 8 bits */
#define PCI_SLOT_INFO_SLOTS(x)		((x >> 4) & 7)
#define PCI_SLOT_INFO_FIRST_BAR_MASK	0x07

/*
 * Controller registers
 */
#define SDHCI_DMA_ADDRESS	0x00
#define SDHCI_BLOCK_SIZE	0x04
#define SDHCI_MAKE_BLKSZ(dma, blksz) (((dma & 0x7) << 12) | (blksz & 0xFFF))

#define SDHCI_BLOCK_COUNT	0x06
#define SDHCI_ARGUMENT		0x08

#define SDHCI_TRANSFER_MODE	0x0C
#define SDHCI_TRNS_DMA		0x01
#define SDHCI_TRNS_BLK_CNT_EN	0x02
#define SDHCI_TRNS_ACMD12	0x04
#define SDHCI_TRNS_READ		0x10
#define SDHCI_TRNS_MULTI	0x20

#define SDHCI_COMMAND		0x0E
#define SDHCI_CMD_RESP_MASK	0x03
#define SDHCI_CMD_CRC		0x08
#define SDHCI_CMD_INDEX		0x10
#define SDHCI_CMD_DATA		0x20

#define SDHCI_CMD_RESP_NONE	0x00
#define SDHCI_CMD_RESP_LONG	0x01
#define SDHCI_CMD_RESP_SHORT	0x02
#define SDHCI_CMD_RESP_SHORT_BUSY 	0x03

#define SDHCI_MAKE_CMD(c, f) (((c & 0xff) << 8) | (f & 0xff))

#define SDHCI_RESPONSE		0x10
#define SDHCI_BUFFER		0x20
#define SDHCI_PRESENT_STATE	0x24

#define SDHCI_CMD_INHIBIT	0x00000001
#define SDHCI_DATA_INHIBIT	0x00000002
#define SDHCI_DOING_WRITE	0x00000100
#define SDHCI_DOING_READ	0x00000200
#define SDHCI_SPACE_AVAILABLE	0x00000400
#define SDHCI_DATA_AVAILABLE	0x00000800
#define SDHCI_CARD_PRESENT	0x00010000
#define SDHCI_WRITE_PROTECT	0x00080000

#define SDHCI_HOST_CONTROL 	0x28
#define SDHCI_CTRL_LED		0x01
#define SDHCI_CTRL_4BITBUS	0x02
#define SDHCI_CTRL_HISPD	0x04

#define SDHCI_POWER_CONTROL	0x29
#define SDHCI_POWER_ON		0x01
#define SDHCI_POWER_180		0x0A
#define SDHCI_POWER_300		0x0C
#define SDHCI_POWER_330		0x0E

#define SDHCI_BLOCK_GAP_CONTROL	0x2A

#define SDHCI_WALK_UP_CONTROL	0x2B

#define SDHCI_CLOCK_CONTROL	0x2C
#define SDHCI_DIVIDER_SHIFT	8
#define SDHCI_CLOCK_CARD_EN	0x0004
#define SDHCI_CLOCK_INT_STABLE	0x0002
#define SDHCI_CLOCK_INT_EN	0x0001

#define SDHCI_TIMEOUT_CONTROL	0x2E

#define SDHCI_SOFTWARE_RESET	0x2F
#define SDHCI_RESET_ALL		0x01
#define SDHCI_RESET_CMD		0x02
#define SDHCI_RESET_DATA	0x04

#define SDHCI_INT_STATUS	0x30
#define SDHCI_INT_ENABLE	0x34
#define SDHCI_SIGNAL_ENABLE	0x38
#define SDHCI_INT_RESPONSE	0x00000001
#define SDHCI_INT_DATA_END	0x00000002
#define SDHCI_INT_BLK_GAP	0x00000004
#define SDHCI_INT_DMA_END	0x00000008
#define SDHCI_INT_SPACE_AVAIL	0x00000010
#define SDHCI_INT_DATA_AVAIL	0x00000020
#define SDHCI_INT_CARD_INSERT	0x00000040
#define SDHCI_INT_CARD_REMOVE	0x00000080
#define SDHCI_INT_CARD_INT	0x00000100
#define SDHCI_INT_ERROR		0x00008000
#define SDHCI_INT_TIMEOUT	0x00010000
#define SDHCI_INT_CRC		0x00020000
#define SDHCI_INT_END_BIT	0x00040000
#define SDHCI_INT_INDEX		0x00080000
#define SDHCI_INT_DATA_TIMEOUT	0x00100000
#define SDHCI_INT_DATA_CRC	0x00200000
#define SDHCI_INT_DATA_END_BIT	0x00400000
#define SDHCI_INT_BUS_POWER	0x00800000
#define SDHCI_INT_ACMD12ERR	0x01000000

#define SDHCI_INT_NORMAL_MASK	0x00007FFF
#define SDHCI_INT_ERROR_MASK	0xFFFF8000

#define SDHCI_INT_CMD_MASK	(SDHCI_INT_RESPONSE | SDHCI_INT_TIMEOUT | \
		SDHCI_INT_CRC | SDHCI_INT_END_BIT | SDHCI_INT_INDEX)
#define SDHCI_INT_DATA_MASK	(SDHCI_INT_DATA_END | SDHCI_INT_DMA_END | \
		SDHCI_INT_DATA_AVAIL | SDHCI_INT_SPACE_AVAIL | \
		SDHCI_INT_DATA_TIMEOUT | SDHCI_INT_DATA_CRC | \
		SDHCI_INT_DATA_END_BIT)

#define SDHCI_ACMD12_ERR	0x3C

/* 3E-3F reserved */

#define SDHCI_CAPABILITIES	0x40
#define SDHCI_TIMEOUT_CLK_MASK	0x0000003F
#define SDHCI_TIMEOUT_CLK_SHIFT 0
#define SDHCI_TIMEOUT_CLK_UNIT	0x00000080
#define SDHCI_CLOCK_BASE_MASK	0x00003F00
#define SDHCI_CLOCK_BASE_SHIFT	8
#define SDHCI_MAX_BLOCK_MASK	0x00030000
#define SDHCI_MAX_BLOCK_SHIFT  	16
#define SDHCI_CAN_DO_HISPD	0x00200000
#define SDHCI_CAN_DO_DMA	0x00400000
#define SDHCI_CAN_VDD_330	0x01000000
#define SDHCI_CAN_VDD_300	0x02000000
#define SDHCI_CAN_VDD_180	0x04000000

/* 44-47 reserved for more caps */

#define SDHCI_MAX_CURRENT	0x48

/* 4C-4F reserved for more max current */
/* 50-FB reserved */

#define SDHCI_SLOT_INT_STATUS	0xFC

#define SDHCI_HOST_VERSION	0xFE
#define SDHCI_VENDOR_VER_MASK	0xFF00
#define SDHCI_VENDOR_VER_SHIFT	8
#define SDHCI_SPEC_VER_MASK	0x00FF
#define SDHCI_SPEC_VER_SHIFT	0

#define SDHCI_USE_DMA		(1<<0)

struct sdhci_chip;

struct sdhci_host {
	struct sdhci_chip	*chip;
	struct mss_host		*mmc;		/* MMC structure */

	spinlock_t		lock;		/* Mutex */
	int			flags;		/* Host attributes */


	unsigned int	max_clk;		/* Max possible freq (MHz) */
	unsigned int	timeout_clk;		/* Timeout freq (KHz) */
	unsigned int	max_block;		/* Max block size (bytes) */

	unsigned int	clock;			/* Current clock (MHz) */
	unsigned short	power;			/* Current voltage */

	struct mss_ll_request	*mrq;		/* Current request */
	struct mss_cmd	*cmd;			/* Current command */
	struct mss_data	*data;			/* Current data request */
	struct mss_card	*card;

	struct scatterlist	*cur_sg;	/* We're working on this */
	char			*mapped_sg;	/* This is where it's mapped */
	int	num_sg;				/* Entries left */
	int	offset;				/* Offset into current sg */
	int	remain;				/* Bytes left in current */
	int	size;				/* Remaining bytes in transfer */

	char	slot_descr[20];			/* Name for reservations */
	int		irq;			/* Device IRQ */
	int		bar;			/* PCI BAR index */
	unsigned long	addr;			/* Bus address */
	void __iomem *	ioaddr;			/* Mapped address */

	struct tasklet_struct	card_tasklet;	/* Tasklet structures */
	struct tasklet_struct	finish_tasklet;

	struct timer_list	timer;		/* Timer for timeouts */
};

struct sdhci_chip {
	struct pci_dev		*pdev;
	unsigned long		quirks;
	int			num_slots;	/* Slots on controller */
	struct sdhci_host	*hosts[0];	/* Pointers to hosts */
};
