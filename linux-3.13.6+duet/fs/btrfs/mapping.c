/*
 * Copyright (C) 2014 George Amvrosiadis.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License v2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this program; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 021110-1307, USA.
 */
#include "ctree.h"
#include "volumes.h"
#include "mapping.h"
#include "raid56.h"

#define BTRFS_FS_MAPPING_UNSUPP_RAID \
		(BTRFS_BLOCK_GROUP_RAID0 | BTRFS_BLOCK_GROUP_RAID1 | \
		BTRFS_BLOCK_GROUP_RAID10 | BTRFS_BLOCK_GROUP_RAID5 | \
		BTRFS_BLOCK_GROUP_RAID6)

/*
 * Finds a device using its bdev pointer. If that's NULL, the devid field
 * is used instead
 */
static struct btrfs_device *__find_device(struct btrfs_fs_info *fs_info,
					struct block_device *bdev, u64 devid)
{
	struct btrfs_device *device;
	struct btrfs_fs_devices *cur_devices;

	cur_devices = fs_info->fs_devices;
	while (cur_devices) {
		list_for_each_entry(device, &cur_devices->devices, dev_list) {
			if ((bdev && device->bdev == bdev) ||
			    (!bdev && device->devid == devid))
				return device;
		}
		cur_devices = cur_devices->seed;
	}

	return NULL;
}

/*
 * Looks for a chunk that contains the given physical address.
 * de_ctx must have already been allocated before calling this.
 */
static int __find_dev_extent_by_paddr(struct btrfs_device *device, u64 pofft,
					struct btrfs_dev_extent_ctx *de_ctx)
{
	struct btrfs_dev_extent *de;
	struct btrfs_key key;
	struct btrfs_root *root = device->dev_root;
	struct btrfs_path *path;
	struct extent_buffer *l;
	int ret, slot;

	if (!de_ctx)
		return -EIO;

	if (pofft >= device->total_bytes || device->is_tgtdev_for_dev_replace)
		return -ENXIO;

	path = btrfs_alloc_path();
	if (!path)
		return -ENOMEM;

	/*
	 * Work on commit root. The related disk blocks are static as long as
	 * CoW is applied.
	 */
	path->search_commit_root = 1;
	path->skip_locking = 1;
	path->reada = 2;

	key.objectid = device->devid;
	key.type = BTRFS_DEV_EXTENT_KEY;
	key.offset = pofft;

	ret = btrfs_search_slot_for_read(root, &key, path, 0, 0);
	if (ret != 0) {
		ret = -ENOENT;
		goto out;
	}

	while (1) {
		l = path->nodes[0];
		slot = path->slots[0];
		if (slot >= btrfs_header_nritems(l)) {
			ret = btrfs_next_leaf(root, path);
			if (ret == 0)
				continue;
			else
				goto out;
		}
		btrfs_item_key_to_cpu(l, &key, slot);

		if (key.objectid < device->devid)
			goto next;

		if (key.objectid > device->devid)
			goto out;

		if (btrfs_key_type(&key) != BTRFS_DEV_EXTENT_KEY)
			goto next;

		/* We found a dev extent item on the right device. Make sure
		 * it contains the given paddr */
		de = btrfs_item_ptr(l, slot, struct btrfs_dev_extent);
		de_ctx->pstart = key.offset;
		de_ctx->vstart = btrfs_dev_extent_chunk_offset(l, de);
		de_ctx->len = btrfs_dev_extent_length(l, de);

		if (de_ctx->pstart <= pofft && pofft < (de_ctx->pstart +
								de_ctx->len))
			break;

next:
		path->slots[0]++;
	}

out:
	btrfs_free_path(path);
	return ret < 0 ? ret : 0;
}

static int __find_extent_obj(struct btrfs_root *root, struct btrfs_key *key,
			struct btrfs_file_extent_item *extent)
{
	struct btrfs_path *path;
	int ret = 0;

	path = btrfs_alloc_path();
	if (!path)
		return -ENOMEM;

	/*
	 * Work on commit root. The related disk blocks are static as long as
	 * CoW is applied.
	 */
	path->search_commit_root = 1;
	path->skip_locking = 1;

	ret = btrfs_search_slot(NULL, root, key, path, 0, 0);
	if (ret != 0) {
		ret = -ENOENT;
		goto out;
	}

	extent = btrfs_item_ptr(path->nodes[0], path->slots[0],
				struct btrfs_file_extent_item);

out:
	btrfs_free_path(path);
	return ret;
}

/* The v-range given is assumed to belong in the same device extent */
static int __iterate_range_items(struct btrfs_fs_info *fs_info, u64 vofft,
				u64 vlen, struct btrfs_root *root,
				iterate_ranges_t iterate)
{
	struct btrfs_root *extroot = fs_info->extent_root;
	struct btrfs_key key, ekey;
	struct btrfs_path *path;
	struct extent_buffer *l;
	struct btrfs_extent_item *ei;
	struct btrfs_extent_inline_ref *iref;
	struct btrfs_extent_data_ref *dref;
	struct btrfs_file_extent_item *extent;
	unsigned long ptr, end;
	u64 cur_vlen, cur_vofft;
	u64 ext_size, ext_len;
	int found, slot, ret=0;

	path = btrfs_alloc_path();
	if (!path)
		return -ENOMEM;

	/*
	 * Work on commit root. The related disk blocks are static as
	 * long as CoW is applied. Also do some readahead in case we
	 * need to iterate over multiple extents.
	 */
	path->reada = 2;
	path->search_commit_root = 1;
	path->skip_locking = 1;

	/* Iterate over all extent items of the given v-range */
	cur_vofft = vofft;
	cur_vlen = vlen;
	while (cur_vlen) {
		btrfs_release_path(path);

#ifdef CONFIG_BTRFS_FS_MAPPING_DEBUG
		printk(KERN_DEBUG "__iterate_range_items: vofft = %llu\n",
			cur_vofft);
#endif /* CONFIG_BTRFS_FS_MAPPING_DEBUG */

		/* Search the extent tree for an extent at this v-offset */
		key.objectid = cur_vofft;
		key.type = BTRFS_EXTENT_ITEM_KEY;
		key.offset = (u64)-1;

		ret = btrfs_search_slot_for_read(extroot, &key, path, 0, 0);
		if (ret != 0) {
			ret = -ENOENT;
			goto out;
		}

		l = path->nodes[0];
		slot = path->slots[0];
		btrfs_item_key_to_cpu(l, &key, slot);

#ifdef CONFIG_BTRFS_FS_MAPPING_DEBUG
		printk(KERN_DEBUG "__iterate_range_items processing (%llu %u "
			"%llu)\n", key.objectid, key.type, key.offset);
#endif /* CONFIG_BTRFS_FS_MAPPING_DEBUG */

		/* Check that we got the right type of extent */
		if (key.type != BTRFS_EXTENT_ITEM_KEY &&
		    key.type != BTRFS_METADATA_ITEM_KEY) {
			ret = -ENOENT;
			goto out;
		}

		/* Find the size of the extent */
		if (key.type == BTRFS_METADATA_ITEM_KEY)
			ext_size = extroot->leafsize;
		else
			ext_size = key.offset;

		/* v-offset should be within this extent */
		if (key.objectid + ext_size <= cur_vofft) {
			ret = -ENOENT;
			goto out;
		}

		/* Find the range of this extent that we should touch */
		if (cur_vlen >= ext_size - (cur_vofft - key.objectid))
			ext_len = ext_size - (cur_vofft - key.objectid);
		else
			ext_len = cur_vlen;

		ei = btrfs_item_ptr(l, slot, struct btrfs_extent_item);

		/* We only support data extents */
		if (!(btrfs_extent_flags(l, ei) & BTRFS_EXTENT_FLAG_DATA))
			goto next;

#ifdef CONFIG_BTRFS_FS_MAPPING_DEBUG
		printk(KERN_DEBUG "__iterate_range_items: data extent found\n");
#endif /* CONFIG_BTRFS_FS_MAPPING_DEBUG */

		/* Go over all refs for this extent and check if they belong
		 * to the given root */
		iref = (struct btrfs_extent_inline_ref *)(ei + 1);
		ptr = (unsigned long)iref;
		end = (unsigned long)ei + btrfs_item_size_nr(l, slot);
		found = 0;
		while (ptr < end) {
			int type;
			u64 offset;

			iref = (struct btrfs_extent_inline_ref *)ptr;
			type = btrfs_extent_inline_ref_type(l, iref);
			offset = btrfs_extent_inline_ref_offset(l, iref);

			if (type == BTRFS_EXTENT_DATA_REF_KEY) {
				dref = (struct btrfs_extent_data_ref *)
							(&iref->offset);

				found = 1;
				ekey.objectid =
					btrfs_extent_data_ref_objectid(l,
									dref);
				ekey.type = BTRFS_EXTENT_DATA_KEY;
				ekey.offset = btrfs_extent_data_ref_offset(l,
									dref);

#ifdef CONFIG_BTRFS_FS_MAPPING_DEBUG
				printk(KERN_DEBUG "__iterate_range_items "
					"looking for extent (%llu %u %llu)\n",
					ekey.objectid, ekey.type, ekey.offset);
#endif /* CONFIG_BTRFS_FS_MAPPING_DEBUG */

				/* Try to find the data extent in given tree */
				ret = __find_extent_obj(root, &ekey, extent);
				if (ret)
					goto out;
				else {
					found = 1;
					break;
				}
			}

			ptr += btrfs_extent_inline_ref_size(type);
		}

		if (!found) {
			ret = -EINVAL;
			goto out;
		}

		if (iterate)
			iterate(cur_vofft, ext_len, (void *)extent);

#ifdef CONFIG_BTRFS_FS_MAPPING_DEBUG
		printk(KERN_INFO "Synergy found w/ extent (%llu %u %llu), for "
			"v-range [%llu, %llu]\n", ekey.objectid, ekey.type,
			ekey.offset, cur_vofft, cur_vofft + ext_len);
#endif /* CONFIG_BTRFS_FS_MAPPING_DEBUG */

next:
		btrfs_release_path(path);

		cur_vofft += ext_len;
		cur_vlen -= ext_len;
	}

out:
	btrfs_free_path(path);
	return ret;
}

/*
 * Given a block device, a physical address range, and a root, find all the
 * metadata items that correspond to the range, and call the provided
 * callback for each
 */
int btrfs_physical_to_items(struct btrfs_fs_info *fs_info,
			    struct block_device *bdev, u64 pofft, u64 plen,
			    struct btrfs_root *root, iterate_ranges_t iterate)
{
	struct btrfs_device *dev;
	u64 cur_pofft, cur_plen, vofft, vlen;
	struct btrfs_dev_extent_ctx de_ctx;
	int ret = 0;

	memset(&de_ctx, 0, sizeof(de_ctx));

	dev = __find_device(fs_info, bdev, 0);
	if (!dev) {
		printk(KERN_ERR "btrfs_physical_to_items: device not found\n");
		ret = -ENODEV;
		goto out;
	}

#ifdef CONFIG_BTRFS_FS_MAPPING_DEBUG
	printk(KERN_DEBUG "btrfs_physical_to_items: device id %llu, size %llu.\n",
		dev->devid, dev->total_bytes);
#endif /* CONFIG_BTRFS_FS_MAPPING_DEBUG */

	/* Iterate through every device extent in the given p-range */
	cur_pofft = pofft;
	cur_plen = plen;
	while (cur_plen) {
		ret = __find_dev_extent_by_paddr(dev, cur_pofft, &de_ctx);
		if (ret) {
			printk(KERN_ERR "btrfs_physical_to_items: dev extent not found\n");
			goto out;
		}

#ifdef CONFIG_BTRFS_FS_MAPPING_DEBUG
		printk(KERN_DEBUG "btrfs_physical_to_items: dev extent pstart %llu, "
			"vstart %llu, len %llu\n",
			de_ctx.pstart, de_ctx.vstart, de_ctx.len);
#endif /* CONFIG_BTRFS_FS_MAPPING_DEBUG */

		/* Adjust pofft and plen to this device extent */
		/* TODO: Support RAID schemes */
		vofft = de_ctx.vstart + (cur_pofft - de_ctx.pstart);
		if (cur_plen > de_ctx.pstart + de_ctx.len - cur_pofft)
			vlen = de_ctx.pstart + de_ctx.len - cur_pofft;
		else
			vlen = cur_plen;

		ret = __iterate_range_items(fs_info, vofft, vlen, root,
								iterate);
		if (ret) {
			printk(KERN_DEBUG "btrfs_physical_to_items: item iteration failed\n");
			goto out;
		}

		cur_pofft += vlen;
		cur_plen -= vlen;
	}

out:
	return ret;
}

/*
 * Translates a chunk's virtual range in one or more physical ranges. Note that
 * an extent may not span chunks, but a chunk may span physical devices.
 */
static int __iter_chunk_ranges(struct btrfs_fs_info *fs_info, u64 chunk_offt,
			struct btrfs_chunk *chunk, struct extent_buffer *l,
			u64 vofft, u64 vlen, iterate_ranges_t iterate)
{
	struct btrfs_stripe *stripe;
	struct btrfs_device *device;
	int i, ret=0;
	u64 pofft;
	int num_stripes = btrfs_chunk_num_stripes(l, chunk);

	if (btrfs_chunk_type(l, chunk) & BTRFS_FS_MAPPING_UNSUPP_RAID) {
		/* TODO: Support RAID schemes */
		return -EINVAL;
	} else { /* DUP, or SINGLE */
		/* Process every stripe */
		for (i = 0; i < num_stripes; i++) {
			stripe = btrfs_stripe_nr(chunk, i);

			/* Find the device for this chunk */
			device = __find_device(fs_info, NULL,
					btrfs_stripe_devid_nr(l, chunk, i));

			/* To get the physical offset, just count from the
			 * beginning of the stripe. No need to change the
			 * length, we're still staying in the chunk */
			pofft = btrfs_stripe_offset_nr(l, chunk, i) +
				(vofft - chunk_offt);

			ret = iterate(pofft, vlen, (void *)device->bdev);
			if (ret)
				goto out;
		}
	}

out:
	return ret;
}

/*
 * Finds all the physical ranges in the virtual range [@vofft, @vofft+@vlen].
 * The virtual range is assumed to be contained within one extent (and as
 * a result, the same chunk.
 */
static int __iter_physical_ranges(struct btrfs_fs_info *fs_info, u64 vofft,
					u64 vlen, iterate_ranges_t iterate)
{
	struct btrfs_root *fs_root = fs_info->chunk_root;
	struct btrfs_key key;
	struct btrfs_path *path;
	struct btrfs_chunk *chunk;
	struct extent_buffer *l;
	u64 chunk_len, chunk_offt;
	int ret = 0;

	path = btrfs_alloc_path();
	if (!path)
		return -ENOMEM;

	path->search_commit_root = 1;
	path->skip_locking = 1;

	/* Find the chunk that the logical range belongs in */
	key.objectid = BTRFS_FIRST_CHUNK_TREE_OBJECTID;
	key.type = BTRFS_CHUNK_ITEM_KEY;
	key.offset = vofft;

	ret = btrfs_search_slot_for_read(fs_root, &key, path, 0, 0);
	if (ret != 0)
		goto not_found;

	l = path->nodes[0];
	btrfs_item_key_to_cpu(l, &key, path->slots[0]);

	if (key.objectid != BTRFS_FIRST_CHUNK_TREE_OBJECTID ||
	    key.type != BTRFS_CHUNK_ITEM_KEY)
		goto not_found;

	chunk = btrfs_item_ptr(l, path->slots[0], struct btrfs_chunk);
	chunk_offt = key.offset;
	chunk_len = btrfs_chunk_length(l, chunk);

	if (chunk_offt + chunk_len <= vofft || chunk_offt > vofft)
		goto not_found;

	ret = __iter_chunk_ranges(fs_info, chunk_offt, chunk, l, vofft, vlen,
								iterate);

	btrfs_free_path(path);
	return ret;

not_found:
	btrfs_free_path(path);
	return -ENOENT;
}

/*
 * A bit scarier than btrfs_get_extent, this finds the mapping of a virtual
 * range to a physical one. Since the resulting physical ranges might be
 * discontiguous, we discover each separately, and call the given callback
 * function.
 */
int btrfs_ino_to_physical(struct btrfs_fs_info *fs_info, u64 ino, u64 iofft,
			u64 ilen, iterate_ranges_t iterate)
{
	struct btrfs_root *fs_root = fs_info->fs_root;
	struct btrfs_key key;
	struct btrfs_path *path;
	struct extent_buffer *l;
	struct btrfs_file_extent_item *fi;
	u64 vofft, vlen;
	u64 cur_iofft, cur_ilen;
	int slot, ret = 0;

	path = btrfs_alloc_path();
	if (!path)
		return -ENOMEM;

	/* Do some readahead, in case we need to go over multiple extents */
	path->reada = 2;
	path->search_commit_root = 1;
	path->skip_locking = 1;

	/* Find the first extent data item corresponding to the given i-offset */
	key.objectid = ino;
	key.type = BTRFS_EXTENT_DATA_KEY;
	key.offset = iofft;

	ret = btrfs_search_slot_for_read(fs_root, &key, path, 0, 0);
	if (ret != 0) {
		ret = -ENOENT;
		goto out;
	}

	/* Iterate over all extents corresponding to the given v-range */
	cur_iofft = iofft;
	cur_ilen = ilen;

	while (cur_ilen) {
		l = path->nodes[0];
		slot = path->slots[0];
		if (slot >= btrfs_header_nritems(l)) {
#ifdef CONFIG_BTRFS_FS_MAPPING_DEBUG
			printk(KERN_DEBUG "btrfs_file_to_physical: EOLeaf\n");
#endif /* CONFIG_BTRFS_FS_MAPPING_DEBUG */
			ret = btrfs_next_leaf(fs_root, path);
			if (ret == 0) {
				continue;
			} else {
				ret = -ENOENT;
				goto out;
			}
		}
		btrfs_item_key_to_cpu(l, &key, slot);

#ifdef CONFIG_BTRFS_FS_MAPPING_DEBUG
		printk(KERN_DEBUG "btrfs_file_to_physical processing (%llu, "
			"%u, %llu)\n", key.objectid, key.type, key.offset);
#endif /* CONFIG_BTRFS_FS_MAPPING_DEBUG */

		if (key.objectid != ino || key.type != BTRFS_EXTENT_DATA_KEY) {
			ret = -ENOENT;
			goto out;
		}

		/* TODO: Account for compression */
		fi = btrfs_item_ptr(l, slot, struct btrfs_file_extent_item);
		if (btrfs_file_extent_type(l, fi) == BTRFS_FILE_EXTENT_INLINE) {
			/* TODO: Support inline extents */
			//l_len = btrfs_file_extent_inline_len(l, fi);
			ret = 0;
			goto out;
		}

		/* Check if we found the extent we're looking for */
		if (key.offset + (btrfs_file_extent_num_bytes(l, fi) -
		    btrfs_file_extent_disk_bytenr(l,fi)) <= cur_iofft)
			goto next;

		/* Find the v-length starting from the v-offset */
		vofft = btrfs_file_extent_disk_bytenr(l, fi) +
			btrfs_file_extent_offset(l, fi) +
			(cur_iofft - key.offset);
		vlen = btrfs_file_extent_num_bytes(l, fi) -
			(vofft - btrfs_file_extent_disk_bytenr(l, fi));

		if (cur_ilen >= vlen) {
			cur_ilen -= vlen;
		} else {
			vlen = cur_ilen;
			cur_ilen = 0;
		}

		/* Process this v-range */
		if (__iter_physical_ranges(fs_info, vofft, vlen, iterate)) {
			ret = -ENOENT;
			goto out;
		}

		/* Move on to the next extent */
		cur_iofft += vlen;

next:
		path->slots[0]++;
		cond_resched();
	}

out:
	btrfs_free_path(path);
	return ret;
}