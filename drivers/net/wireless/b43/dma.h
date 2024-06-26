#ifndef B43_DMA_H_
#define B43_DMA_H_

#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>
#include <linux/linkage.h>
#include <asm/atomic.h>

#include "b43.h"

/* DMA-Interrupt reasons. */
#define B43_DMAIRQ_FATALMASK	((1 << 10) | (1 << 11) | (1 << 12) \
					 | (1 << 14) | (1 << 15))
#define B43_DMAIRQ_NONFATALMASK	(1 << 13)
#define B43_DMAIRQ_RX_DONE		(1 << 16)

/*** 32-bit DMA Engine. ***/

/* 32-bit DMA controller registers. */
#define B43_DMA32_TXCTL				0x00
#define		B43_DMA32_TXENABLE			0x00000001
#define		B43_DMA32_TXSUSPEND			0x00000002
#define		B43_DMA32_TXLOOPBACK		0x00000004
#define		B43_DMA32_TXFLUSH			0x00000010
#define		B43_DMA32_TXADDREXT_MASK		0x00030000
#define		B43_DMA32_TXADDREXT_SHIFT		16
#define B43_DMA32_TXRING				0x04
#define B43_DMA32_TXINDEX				0x08
#define B43_DMA32_TXSTATUS				0x0C
#define		B43_DMA32_TXDPTR			0x00000FFF
#define		B43_DMA32_TXSTATE			0x0000F000
#define			B43_DMA32_TXSTAT_DISABLED	0x00000000
#define			B43_DMA32_TXSTAT_ACTIVE	0x00001000
#define			B43_DMA32_TXSTAT_IDLEWAIT	0x00002000
#define			B43_DMA32_TXSTAT_STOPPED	0x00003000
#define			B43_DMA32_TXSTAT_SUSP	0x00004000
#define		B43_DMA32_TXERROR			0x000F0000
#define			B43_DMA32_TXERR_NOERR	0x00000000
#define			B43_DMA32_TXERR_PROT	0x00010000
#define			B43_DMA32_TXERR_UNDERRUN	0x00020000
#define			B43_DMA32_TXERR_BUFREAD	0x00030000
#define			B43_DMA32_TXERR_DESCREAD	0x00040000
#define		B43_DMA32_TXACTIVE			0xFFF00000
#define B43_DMA32_RXCTL				0x10
#define		B43_DMA32_RXENABLE			0x00000001
#define		B43_DMA32_RXFROFF_MASK		0x000000FE
#define		B43_DMA32_RXFROFF_SHIFT		1
#define		B43_DMA32_RXDIRECTFIFO		0x00000100
#define		B43_DMA32_RXADDREXT_MASK		0x00030000
#define		B43_DMA32_RXADDREXT_SHIFT		16
#define B43_DMA32_RXRING				0x14
#define B43_DMA32_RXINDEX				0x18
#define B43_DMA32_RXSTATUS				0x1C
#define		B43_DMA32_RXDPTR			0x00000FFF
#define		B43_DMA32_RXSTATE			0x0000F000
#define			B43_DMA32_RXSTAT_DISABLED	0x00000000
#define			B43_DMA32_RXSTAT_ACTIVE	0x00001000
#define			B43_DMA32_RXSTAT_IDLEWAIT	0x00002000
#define			B43_DMA32_RXSTAT_STOPPED	0x00003000
#define		B43_DMA32_RXERROR			0x000F0000
#define			B43_DMA32_RXERR_NOERR	0x00000000
#define			B43_DMA32_RXERR_PROT	0x00010000
#define			B43_DMA32_RXERR_OVERFLOW	0x00020000
#define			B43_DMA32_RXERR_BUFWRITE	0x00030000
#define			B43_DMA32_RXERR_DESCREAD	0x00040000
#define		B43_DMA32_RXACTIVE			0xFFF00000

/* 32-bit DMA descriptor. */
struct b43_dmadesc32 {
	__le32 control;
	__le32 address;
} __attribute__ ((__packed__));
#define B43_DMA32_DCTL_BYTECNT		0x00001FFF
#define B43_DMA32_DCTL_ADDREXT_MASK		0x00030000
#define B43_DMA32_DCTL_ADDREXT_SHIFT	16
#define B43_DMA32_DCTL_DTABLEEND		0x10000000
#define B43_DMA32_DCTL_IRQ			0x20000000
#define B43_DMA32_DCTL_FRAMEEND		0x40000000
#define B43_DMA32_DCTL_FRAMESTART		0x80000000

/*** 64-bit DMA Engine. ***/

/* 64-bit DMA controller registers. */
#define B43_DMA64_TXCTL				0x00
#define		B43_DMA64_TXENABLE			0x00000001
#define		B43_DMA64_TXSUSPEND			0x00000002
#define		B43_DMA64_TXLOOPBACK		0x00000004
#define		B43_DMA64_TXFLUSH			0x00000010
#define		B43_DMA64_TXADDREXT_MASK		0x00030000
#define		B43_DMA64_TXADDREXT_SHIFT		16
#define B43_DMA64_TXINDEX				0x04
#define B43_DMA64_TXRINGLO				0x08
#define B43_DMA64_TXRINGHI				0x0C
#define B43_DMA64_TXSTATUS				0x10
#define		B43_DMA64_TXSTATDPTR		0x00001FFF
#define		B43_DMA64_TXSTAT			0xF0000000
#define			B43_DMA64_TXSTAT_DISABLED	0x00000000
#define			B43_DMA64_TXSTAT_ACTIVE	0x10000000
#define			B43_DMA64_TXSTAT_IDLEWAIT	0x20000000
#define			B43_DMA64_TXSTAT_STOPPED	0x30000000
#define			B43_DMA64_TXSTAT_SUSP	0x40000000
#define B43_DMA64_TXERROR				0x14
#define		B43_DMA64_TXERRDPTR			0x0001FFFF
#define		B43_DMA64_TXERR			0xF0000000
#define			B43_DMA64_TXERR_NOERR	0x00000000
#define			B43_DMA64_TXERR_PROT	0x10000000
#define			B43_DMA64_TXERR_UNDERRUN	0x20000000
#define			B43_DMA64_TXERR_TRANSFER	0x30000000
#define			B43_DMA64_TXERR_DESCREAD	0x40000000
#define			B43_DMA64_TXERR_CORE	0x50000000
#define B43_DMA64_RXCTL				0x20
#define		B43_DMA64_RXENABLE			0x00000001
#define		B43_DMA64_RXFROFF_MASK		0x000000FE
#define		B43_DMA64_RXFROFF_SHIFT		1
#define		B43_DMA64_RXDIRECTFIFO		0x00000100
#define		B43_DMA64_RXADDREXT_MASK		0x00030000
#define		B43_DMA64_RXADDREXT_SHIFT		16
#define B43_DMA64_RXINDEX				0x24
#define B43_DMA64_RXRINGLO				0x28
#define B43_DMA64_RXRINGHI				0x2C
#define B43_DMA64_RXSTATUS				0x30
#define		B43_DMA64_RXSTATDPTR		0x00001FFF
#define		B43_DMA64_RXSTAT			0xF0000000
#define			B43_DMA64_RXSTAT_DISABLED	0x00000000
#define			B43_DMA64_RXSTAT_ACTIVE	0x10000000
#define			B43_DMA64_RXSTAT_IDLEWAIT	0x20000000
#define			B43_DMA64_RXSTAT_STOPPED	0x30000000
#define			B43_DMA64_RXSTAT_SUSP	0x40000000
#define B43_DMA64_RXERROR				0x34
#define		B43_DMA64_RXERRDPTR			0x0001FFFF
#define		B43_DMA64_RXERR			0xF0000000
#define			B43_DMA64_RXERR_NOERR	0x00000000
#define			B43_DMA64_RXERR_PROT	0x10000000
#define			B43_DMA64_RXERR_UNDERRUN	0x20000000
#define			B43_DMA64_RXERR_TRANSFER	0x30000000
#define			B43_DMA64_RXERR_DESCREAD	0x40000000
#define			B43_DMA64_RXERR_CORE	0x50000000

/* 64-bit DMA descriptor. */
struct b43_dmadesc64 {
	__le32 control0;
	__le32 control1;
	__le32 address_low;
	__le32 address_high;
} __attribute__ ((__packed__));
#define B43_DMA64_DCTL0_DTABLEEND		0x10000000
#define B43_DMA64_DCTL0_IRQ			0x20000000
#define B43_DMA64_DCTL0_FRAMEEND		0x40000000
#define B43_DMA64_DCTL0_FRAMESTART		0x80000000
#define B43_DMA64_DCTL1_BYTECNT		0x00001FFF
#define B43_DMA64_DCTL1_ADDREXT_MASK	0x00030000
#define B43_DMA64_DCTL1_ADDREXT_SHIFT	16

struct b43_dmadesc_generic {
	union {
		struct b43_dmadesc32 dma32;
		struct b43_dmadesc64 dma64;
	} __attribute__ ((__packed__));
} __attribute__ ((__packed__));

/* Misc DMA constants */
#define B43_DMA_RINGMEMSIZE		PAGE_SIZE
#define B43_DMA0_RX_FRAMEOFFSET	30
#define B43_DMA3_RX_FRAMEOFFSET	0

/* DMA engine tuning knobs */
#define B43_TXRING_SLOTS		128
#define B43_RXRING_SLOTS		64
#define B43_DMA0_RX_BUFFERSIZE		(B43_DMA0_RX_FRAMEOFFSET + IEEE80211_MAX_FRAME_LEN)
#define B43_DMA3_RX_BUFFERSIZE	16

#ifdef CONFIG_B43_DMA

struct sk_buff;
struct b43_private;
struct b43_txstatus;

struct b43_dmadesc_meta {
	/* The kernel DMA-able buffer. */
	struct sk_buff *skb;
	/* DMA base bus-address of the descriptor buffer. */
	dma_addr_t dmaaddr;
	/* ieee80211 TX status. Only used once per 802.11 frag. */
	bool is_last_fragment;
	struct ieee80211_tx_status txstat;
};

struct b43_dmaring;

/* Lowlevel DMA operations that differ between 32bit and 64bit DMA. */
struct b43_dma_ops {
	struct b43_dmadesc_generic *(*idx2desc) (struct b43_dmaring * ring,
						 int slot,
						 struct b43_dmadesc_meta **
						 meta);
	void (*fill_descriptor) (struct b43_dmaring * ring,
				 struct b43_dmadesc_generic * desc,
				 dma_addr_t dmaaddr, u16 bufsize, int start,
				 int end, int irq);
	void (*poke_tx) (struct b43_dmaring * ring, int slot);
	void (*tx_suspend) (struct b43_dmaring * ring);
	void (*tx_resume) (struct b43_dmaring * ring);
	int (*get_current_rxslot) (struct b43_dmaring * ring);
	void (*set_current_rxslot) (struct b43_dmaring * ring, int slot);
};

struct b43_dmaring {
	/* Lowlevel DMA ops. */
	const struct b43_dma_ops *ops;
	/* Kernel virtual base address of the ring memory. */
	void *descbase;
	/* Meta data about all descriptors. */
	struct b43_dmadesc_meta *meta;
	/* Cache of TX headers for each slot.
	 * This is to avoid an allocation on each TX.
	 * This is NULL for an RX ring.
	 */
	u8 *txhdr_cache;
	/* (Unadjusted) DMA base bus-address of the ring memory. */
	dma_addr_t dmabase;
	/* Number of descriptor slots in the ring. */
	int nr_slots;
	/* Number of used descriptor slots. */
	int used_slots;
	/* Currently used slot in the ring. */
	int current_slot;
	/* Total number of packets sent. Statistics only. */
	unsigned int nr_tx_packets;
	/* Frameoffset in octets. */
	u32 frameoffset;
	/* Descriptor buffer size. */
	u16 rx_buffersize;
	/* The MMIO base register of the DMA controller. */
	u16 mmio_base;
	/* DMA controller index number (0-5). */
	int index;
	/* Boolean. Is this a TX ring? */
	bool tx;
	/* Boolean. 64bit DMA if true, 32bit DMA otherwise. */
	bool dma64;
	/* Boolean. Is this ring stopped at ieee80211 level? */
	bool stopped;
	/* Lock, only used for TX. */
	spinlock_t lock;
	struct b43_wldev *dev;
#ifdef CONFIG_B43_DEBUG
	/* Maximum number of used slots. */
	int max_used_slots;
	/* Last time we injected a ring overflow. */
	unsigned long last_injected_overflow;
#endif				/* CONFIG_B43_DEBUG */
};

static inline u32 b43_dma_read(struct b43_dmaring *ring, u16 offset)
{
	return b43_read32(ring->dev, ring->mmio_base + offset);
}

static inline
    void b43_dma_write(struct b43_dmaring *ring, u16 offset, u32 value)
{
	b43_write32(ring->dev, ring->mmio_base + offset, value);
}

int b43_dma_init(struct b43_wldev *dev);
void b43_dma_free(struct b43_wldev *dev);

int b43_dmacontroller_rx_reset(struct b43_wldev *dev,
			       u16 dmacontroller_mmio_base, int dma64);
int b43_dmacontroller_tx_reset(struct b43_wldev *dev,
			       u16 dmacontroller_mmio_base, int dma64);

u16 b43_dmacontroller_base(int dma64bit, int dmacontroller_idx);

void b43_dma_tx_suspend(struct b43_wldev *dev);
void b43_dma_tx_resume(struct b43_wldev *dev);

void b43_dma_get_tx_stats(struct b43_wldev *dev,
			  struct ieee80211_tx_queue_stats *stats);

int b43_dma_tx(struct b43_wldev *dev,
	       struct sk_buff *skb, struct ieee80211_tx_control *ctl);
void b43_dma_handle_txstatus(struct b43_wldev *dev,
			     const struct b43_txstatus *status);

void b43_dma_rx(struct b43_dmaring *ring);

#else /* CONFIG_B43_DMA */

static inline int b43_dma_init(struct b43_wldev *dev)
{
	return 0;
}
static inline void b43_dma_free(struct b43_wldev *dev)
{
}
static inline
    int b43_dmacontroller_rx_reset(struct b43_wldev *dev,
				   u16 dmacontroller_mmio_base, int dma64)
{
	return 0;
}
static inline
    int b43_dmacontroller_tx_reset(struct b43_wldev *dev,
				   u16 dmacontroller_mmio_base, int dma64)
{
	return 0;
}
static inline
    void b43_dma_get_tx_stats(struct b43_wldev *dev,
			      struct ieee80211_tx_queue_stats *stats)
{
}
static inline
    int b43_dma_tx(struct b43_wldev *dev,
		   struct sk_buff *skb, struct ieee80211_tx_control *ctl)
{
	return 0;
}
static inline
    void b43_dma_handle_txstatus(struct b43_wldev *dev,
				 const struct b43_txstatus *status)
{
}
static inline void b43_dma_rx(struct b43_dmaring *ring)
{
}
static inline void b43_dma_tx_suspend(struct b43_wldev *dev)
{
}
static inline void b43_dma_tx_resume(struct b43_wldev *dev)
{
}

#endif /* CONFIG_B43_DMA */
#endif /* B43_DMA_H_ */
