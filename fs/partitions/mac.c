/*
 *  fs/partitions/mac.c
 *
 *  Code extracted from drivers/block/genhd.c
 *  Copyright (C) 1991-1998  Linus Torvalds
 *  Re-organised Feb 1998 Russell King
 */

#include <linux/ctype.h>
#include "check.h"
#include "mac.h"

#ifdef CONFIG_PPC_PMAC
#include <asm/machdep.h>
extern void note_bootable_part(dev_t dev, int part, int goodness);
#endif

/*
 * Code to understand MacOS partition tables.
 */

static inline void mac_fix_string(char *stg, int len)
{
	int i;

	for (i = len - 1; i >= 0 && stg[i] == ' '; i--)
		stg[i] = 0;
}

int mac_partition(struct parsed_partitions *state, struct block_device *bdev)
{
	Sector sect;
	unsigned char *data;
	int slot, blocks_in_map;
	unsigned secsize;
#ifdef CONFIG_PPC_PMAC
	int found_root = 0;
	int found_root_goodness = 0;
#endif
	struct mac_partition *part;
	struct mac_driver_desc *md;

	/* Get 0th block and look at the first partition map entry. */
	md = (struct mac_driver_desc *) read_dev_sector(bdev, 0, &sect);
	if (!md)
		return -1;
	if (be16_to_cpu(md->signature) != MAC_DRIVER_MAGIC) {
		put_dev_sector(sect);
		return 0;
	}
	secsize = be16_to_cpu(md->block_size);
	put_dev_sector(sect);
	data = read_dev_sector(bdev, secsize/512, &sect);
	if (!data)
		return -1;
	part = (struct mac_partition *) (data + secsize%512);
	if (be16_to_cpu(part->signature) != MAC_PARTITION_MAGIC) {
		put_dev_sector(sect);
		return 0;		/* not a MacOS disk */
	}
	printk(" [mac]");
	blocks_in_map = be32_to_cpu(part->map_count);
	if (blocks_in_map < 0 || blocks_in_map >= 256) {
		put_dev_sector(sect);
		return 0;
	}
	for (slot = 1; slot <= blocks_in_map; ++slot) {
		int pos = slot * secsize;
		put_dev_sector(sect);
		data = read_dev_sector(bdev, pos/512, &sect);
		if (!data)
			return -1;
		part = (struct mac_partition *) (data + pos%512);
		if (be16_to_cpu(part->signature) != MAC_PARTITION_MAGIC)
			break;
		put_partition(state, slot,
			be32_to_cpu(part->start_block) * (secsize/512),
			be32_to_cpu(part->block_count) * (secsize/512));

		if (!strnicmp(part->type, "Linux_RAID", 10))
			state->parts[slot].flags = 1;
#ifdef CONFIG_PPC_PMAC
		/*
		 * If this is the first bootable partition, tell the
		 * setup code, in case it wants to make this the root.
		 */
		if (machine_is(powermac)) {
			int goodness = 0;

			mac_fix_string(part->processor, 16);
			mac_fix_string(part->name, 32);
			mac_fix_string(part->type, 32);					
		    
			if ((be32_to_cpu(part->status) & MAC_STATUS_BOOTABLE)
			    && strcasecmp(part->processor, "powerpc") == 0)
				goodness++;

			if (strcasecmp(part->type, "Apple_UNIX_SVR2") == 0
			    || (strnicmp(part->type, "Linux", 5) == 0
			        && strcasecmp(part->type, "Linux_swap") != 0)) {
				int i, l;

				goodness++;
				l = strlen(part->name);
				if (strcmp(part->name, "/") == 0)
					goodness++;
				for (i = 0; i <= l - 4; ++i) {
					if (strnicmp(part->name + i, "root",
						     4) == 0) {
						goodness += 2;
						break;
					}
				}
				if (strnicmp(part->name, "swap", 4) == 0)
					goodness--;
			}

			if (goodness > found_root_goodness) {
				found_root = slot;
				found_root_goodness = goodness;
			}
		}
#endif /* CONFIG_PPC_PMAC */
	}
#ifdef CONFIG_PPC_PMAC
	if (found_root_goodness)
		note_bootable_part(bdev->bd_dev, found_root, found_root_goodness);
#endif

	put_dev_sector(sect);
	printk("\n");
	return 1;
}
