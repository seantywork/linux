// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2007 Oracle.  All rights reserved.
 */

#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/pagemap.h>
#include <linux/writeback.h>
#include <linux/blkdev.h>
#include <linux/sort.h>
#include <linux/rcupdate.h>
#include <linux/kthread.h>
#include <linux/slab.h>
#include <linux/ratelimit.h>
#include <linux/percpu_counter.h>
#include <linux/lockdep.h>
#include <linux/crc32c.h>
#include "ctree.h"
#include "extent-tree.h"
#include "transaction.h"
#include "disk-io.h"
#include "print-tree.h"
#include "volumes.h"
#include "raid56.h"
#include "locking.h"
#include "free-space-cache.h"
#include "free-space-tree.h"
#include "qgroup.h"
#include "ref-verify.h"
#include "space-info.h"
#include "block-rsv.h"
#include "discard.h"
#include "zoned.h"
#include "dev-replace.h"
#include "fs.h"
#include "accessors.h"
#include "root-tree.h"
#include "file-item.h"
#include "orphan.h"
#include "tree-checker.h"
#include "raid-stripe-tree.h"

#undef SCRAMBLE_DELAYED_REFS


static int __btrfs_free_extent(struct btrfs_trans_handle *trans,
			       struct btrfs_delayed_ref_head *href,
			       const struct btrfs_delayed_ref_node *node,
			       struct btrfs_delayed_extent_op *extra_op);
static void __run_delayed_extent_op(struct btrfs_delayed_extent_op *extent_op,
				    struct extent_buffer *leaf,
				    struct btrfs_extent_item *ei);
static int alloc_reserved_file_extent(struct btrfs_trans_handle *trans,
				      u64 parent, u64 root_objectid,
				      u64 flags, u64 owner, u64 offset,
				      struct btrfs_key *ins, int ref_mod, u64 oref_root);
static int alloc_reserved_tree_block(struct btrfs_trans_handle *trans,
				     const struct btrfs_delayed_ref_node *node,
				     struct btrfs_delayed_extent_op *extent_op);
static int find_next_key(const struct btrfs_path *path, int level,
			 struct btrfs_key *key);

static int block_group_bits(const struct btrfs_block_group *cache, u64 bits)
{
	return (cache->flags & bits) == bits;
}

/* simple helper to search for an existing data extent at a given offset */
int btrfs_lookup_data_extent(struct btrfs_fs_info *fs_info, u64 start, u64 len)
{
	struct btrfs_root *root = btrfs_extent_root(fs_info, start);
	struct btrfs_key key;
	BTRFS_PATH_AUTO_FREE(path);

	path = btrfs_alloc_path();
	if (!path)
		return -ENOMEM;

	key.objectid = start;
	key.type = BTRFS_EXTENT_ITEM_KEY;
	key.offset = len;
	return btrfs_search_slot(NULL, root, &key, path, 0, 0);
}

/*
 * helper function to lookup reference count and flags of a tree block.
 *
 * the head node for delayed ref is used to store the sum of all the
 * reference count modifications queued up in the rbtree. the head
 * node may also store the extent flags to set. This way you can check
 * to see what the reference count and extent flags would be if all of
 * the delayed refs are not processed.
 */
int btrfs_lookup_extent_info(struct btrfs_trans_handle *trans,
			     struct btrfs_fs_info *fs_info, u64 bytenr,
			     u64 offset, int metadata, u64 *refs, u64 *flags,
			     u64 *owning_root)
{
	struct btrfs_root *extent_root;
	struct btrfs_delayed_ref_head *head;
	struct btrfs_delayed_ref_root *delayed_refs;
	BTRFS_PATH_AUTO_FREE(path);
	struct btrfs_key key;
	u64 num_refs;
	u64 extent_flags;
	u64 owner = 0;
	int ret;

	/*
	 * If we don't have skinny metadata, don't bother doing anything
	 * different
	 */
	if (metadata && !btrfs_fs_incompat(fs_info, SKINNY_METADATA)) {
		offset = fs_info->nodesize;
		metadata = 0;
	}

	path = btrfs_alloc_path();
	if (!path)
		return -ENOMEM;

search_again:
	key.objectid = bytenr;
	if (metadata)
		key.type = BTRFS_METADATA_ITEM_KEY;
	else
		key.type = BTRFS_EXTENT_ITEM_KEY;
	key.offset = offset;

	extent_root = btrfs_extent_root(fs_info, bytenr);
	ret = btrfs_search_slot(NULL, extent_root, &key, path, 0, 0);
	if (ret < 0)
		return ret;

	if (ret > 0 && key.type == BTRFS_METADATA_ITEM_KEY) {
		if (path->slots[0]) {
			path->slots[0]--;
			btrfs_item_key_to_cpu(path->nodes[0], &key,
					      path->slots[0]);
			if (key.objectid == bytenr &&
			    key.type == BTRFS_EXTENT_ITEM_KEY &&
			    key.offset == fs_info->nodesize)
				ret = 0;
		}
	}

	if (ret == 0) {
		struct extent_buffer *leaf = path->nodes[0];
		struct btrfs_extent_item *ei;
		const u32 item_size = btrfs_item_size(leaf, path->slots[0]);

		if (unlikely(item_size < sizeof(*ei))) {
			ret = -EUCLEAN;
			btrfs_err(fs_info,
			"unexpected extent item size, has %u expect >= %zu",
				  item_size, sizeof(*ei));
			btrfs_abort_transaction(trans, ret);
			return ret;
		}

		ei = btrfs_item_ptr(leaf, path->slots[0], struct btrfs_extent_item);
		num_refs = btrfs_extent_refs(leaf, ei);
		if (unlikely(num_refs == 0)) {
			ret = -EUCLEAN;
			btrfs_err(fs_info,
		"unexpected zero reference count for extent item (%llu %u %llu)",
				  key.objectid, key.type, key.offset);
			btrfs_abort_transaction(trans, ret);
			return ret;
		}
		extent_flags = btrfs_extent_flags(leaf, ei);
		owner = btrfs_get_extent_owner_root(fs_info, leaf, path->slots[0]);
	} else {
		num_refs = 0;
		extent_flags = 0;
		ret = 0;
	}

	delayed_refs = &trans->transaction->delayed_refs;
	spin_lock(&delayed_refs->lock);
	head = btrfs_find_delayed_ref_head(fs_info, delayed_refs, bytenr);
	if (head) {
		if (!mutex_trylock(&head->mutex)) {
			refcount_inc(&head->refs);
			spin_unlock(&delayed_refs->lock);

			btrfs_release_path(path);

			/*
			 * Mutex was contended, block until it's released and try
			 * again
			 */
			mutex_lock(&head->mutex);
			mutex_unlock(&head->mutex);
			btrfs_put_delayed_ref_head(head);
			goto search_again;
		}
		spin_lock(&head->lock);
		if (head->extent_op && head->extent_op->update_flags)
			extent_flags |= head->extent_op->flags_to_set;

		num_refs += head->ref_mod;
		spin_unlock(&head->lock);
		mutex_unlock(&head->mutex);
	}
	spin_unlock(&delayed_refs->lock);

	WARN_ON(num_refs == 0);
	if (refs)
		*refs = num_refs;
	if (flags)
		*flags = extent_flags;
	if (owning_root)
		*owning_root = owner;

	return ret;
}

/*
 * Back reference rules.  Back refs have three main goals:
 *
 * 1) differentiate between all holders of references to an extent so that
 *    when a reference is dropped we can make sure it was a valid reference
 *    before freeing the extent.
 *
 * 2) Provide enough information to quickly find the holders of an extent
 *    if we notice a given block is corrupted or bad.
 *
 * 3) Make it easy to migrate blocks for FS shrinking or storage pool
 *    maintenance.  This is actually the same as #2, but with a slightly
 *    different use case.
 *
 * There are two kinds of back refs. The implicit back refs is optimized
 * for pointers in non-shared tree blocks. For a given pointer in a block,
 * back refs of this kind provide information about the block's owner tree
 * and the pointer's key. These information allow us to find the block by
 * b-tree searching. The full back refs is for pointers in tree blocks not
 * referenced by their owner trees. The location of tree block is recorded
 * in the back refs. Actually the full back refs is generic, and can be
 * used in all cases the implicit back refs is used. The major shortcoming
 * of the full back refs is its overhead. Every time a tree block gets
 * COWed, we have to update back refs entry for all pointers in it.
 *
 * For a newly allocated tree block, we use implicit back refs for
 * pointers in it. This means most tree related operations only involve
 * implicit back refs. For a tree block created in old transaction, the
 * only way to drop a reference to it is COW it. So we can detect the
 * event that tree block loses its owner tree's reference and do the
 * back refs conversion.
 *
 * When a tree block is COWed through a tree, there are four cases:
 *
 * The reference count of the block is one and the tree is the block's
 * owner tree. Nothing to do in this case.
 *
 * The reference count of the block is one and the tree is not the
 * block's owner tree. In this case, full back refs is used for pointers
 * in the block. Remove these full back refs, add implicit back refs for
 * every pointers in the new block.
 *
 * The reference count of the block is greater than one and the tree is
 * the block's owner tree. In this case, implicit back refs is used for
 * pointers in the block. Add full back refs for every pointers in the
 * block, increase lower level extents' reference counts. The original
 * implicit back refs are entailed to the new block.
 *
 * The reference count of the block is greater than one and the tree is
 * not the block's owner tree. Add implicit back refs for every pointer in
 * the new block, increase lower level extents' reference count.
 *
 * Back Reference Key composing:
 *
 * The key objectid corresponds to the first byte in the extent,
 * The key type is used to differentiate between types of back refs.
 * There are different meanings of the key offset for different types
 * of back refs.
 *
 * File extents can be referenced by:
 *
 * - multiple snapshots, subvolumes, or different generations in one subvol
 * - different files inside a single subvolume
 * - different offsets inside a file (bookend extents in file.c)
 *
 * The extent ref structure for the implicit back refs has fields for:
 *
 * - Objectid of the subvolume root
 * - objectid of the file holding the reference
 * - original offset in the file
 * - how many bookend extents
 *
 * The key offset for the implicit back refs is hash of the first
 * three fields.
 *
 * The extent ref structure for the full back refs has field for:
 *
 * - number of pointers in the tree leaf
 *
 * The key offset for the implicit back refs is the first byte of
 * the tree leaf
 *
 * When a file extent is allocated, The implicit back refs is used.
 * the fields are filled in:
 *
 *     (root_key.objectid, inode objectid, offset in file, 1)
 *
 * When a file extent is removed file truncation, we find the
 * corresponding implicit back refs and check the following fields:
 *
 *     (btrfs_header_owner(leaf), inode objectid, offset in file)
 *
 * Btree extents can be referenced by:
 *
 * - Different subvolumes
 *
 * Both the implicit back refs and the full back refs for tree blocks
 * only consist of key. The key offset for the implicit back refs is
 * objectid of block's owner tree. The key offset for the full back refs
 * is the first byte of parent block.
 *
 * When implicit back refs is used, information about the lowest key and
 * level of the tree block are required. These information are stored in
 * tree block info structure.
 */

/*
 * is_data == BTRFS_REF_TYPE_BLOCK, tree block type is required,
 * is_data == BTRFS_REF_TYPE_DATA, data type is requiried,
 * is_data == BTRFS_REF_TYPE_ANY, either type is OK.
 */
int btrfs_get_extent_inline_ref_type(const struct extent_buffer *eb,
				     const struct btrfs_extent_inline_ref *iref,
				     enum btrfs_inline_ref_type is_data)
{
	struct btrfs_fs_info *fs_info = eb->fs_info;
	int type = btrfs_extent_inline_ref_type(eb, iref);
	u64 offset = btrfs_extent_inline_ref_offset(eb, iref);

	if (type == BTRFS_EXTENT_OWNER_REF_KEY) {
		ASSERT(btrfs_fs_incompat(fs_info, SIMPLE_QUOTA));
		return type;
	}

	if (type == BTRFS_TREE_BLOCK_REF_KEY ||
	    type == BTRFS_SHARED_BLOCK_REF_KEY ||
	    type == BTRFS_SHARED_DATA_REF_KEY ||
	    type == BTRFS_EXTENT_DATA_REF_KEY) {
		if (is_data == BTRFS_REF_TYPE_BLOCK) {
			if (type == BTRFS_TREE_BLOCK_REF_KEY)
				return type;
			if (type == BTRFS_SHARED_BLOCK_REF_KEY) {
				ASSERT(fs_info);
				/*
				 * Every shared one has parent tree block,
				 * which must be aligned to sector size.
				 */
				if (offset && IS_ALIGNED(offset, fs_info->sectorsize))
					return type;
			}
		} else if (is_data == BTRFS_REF_TYPE_DATA) {
			if (type == BTRFS_EXTENT_DATA_REF_KEY)
				return type;
			if (type == BTRFS_SHARED_DATA_REF_KEY) {
				ASSERT(fs_info);
				/*
				 * Every shared one has parent tree block,
				 * which must be aligned to sector size.
				 */
				if (offset &&
				    IS_ALIGNED(offset, fs_info->sectorsize))
					return type;
			}
		} else {
			ASSERT(is_data == BTRFS_REF_TYPE_ANY);
			return type;
		}
	}

	WARN_ON(1);
	btrfs_print_leaf(eb);
	btrfs_err(fs_info,
		  "eb %llu iref 0x%lx invalid extent inline ref type %d",
		  eb->start, (unsigned long)iref, type);

	return BTRFS_REF_TYPE_INVALID;
}

u64 hash_extent_data_ref(u64 root_objectid, u64 owner, u64 offset)
{
	u32 high_crc = ~(u32)0;
	u32 low_crc = ~(u32)0;
	__le64 lenum;

	lenum = cpu_to_le64(root_objectid);
	high_crc = crc32c(high_crc, &lenum, sizeof(lenum));
	lenum = cpu_to_le64(owner);
	low_crc = crc32c(low_crc, &lenum, sizeof(lenum));
	lenum = cpu_to_le64(offset);
	low_crc = crc32c(low_crc, &lenum, sizeof(lenum));

	return ((u64)high_crc << 31) ^ (u64)low_crc;
}

static u64 hash_extent_data_ref_item(const struct extent_buffer *leaf,
				     const struct btrfs_extent_data_ref *ref)
{
	return hash_extent_data_ref(btrfs_extent_data_ref_root(leaf, ref),
				    btrfs_extent_data_ref_objectid(leaf, ref),
				    btrfs_extent_data_ref_offset(leaf, ref));
}

static bool match_extent_data_ref(const struct extent_buffer *leaf,
				  const struct btrfs_extent_data_ref *ref,
				  u64 root_objectid, u64 owner, u64 offset)
{
	if (btrfs_extent_data_ref_root(leaf, ref) != root_objectid ||
	    btrfs_extent_data_ref_objectid(leaf, ref) != owner ||
	    btrfs_extent_data_ref_offset(leaf, ref) != offset)
		return false;
	return true;
}

static noinline int lookup_extent_data_ref(struct btrfs_trans_handle *trans,
					   struct btrfs_path *path,
					   u64 bytenr, u64 parent,
					   u64 root_objectid,
					   u64 owner, u64 offset)
{
	struct btrfs_root *root = btrfs_extent_root(trans->fs_info, bytenr);
	struct btrfs_key key;
	struct btrfs_extent_data_ref *ref;
	struct extent_buffer *leaf;
	u32 nritems;
	int recow;
	int ret;

	key.objectid = bytenr;
	if (parent) {
		key.type = BTRFS_SHARED_DATA_REF_KEY;
		key.offset = parent;
	} else {
		key.type = BTRFS_EXTENT_DATA_REF_KEY;
		key.offset = hash_extent_data_ref(root_objectid,
						  owner, offset);
	}
again:
	recow = 0;
	ret = btrfs_search_slot(trans, root, &key, path, -1, 1);
	if (ret < 0)
		return ret;

	if (parent) {
		if (ret)
			return -ENOENT;
		return 0;
	}

	ret = -ENOENT;
	leaf = path->nodes[0];
	nritems = btrfs_header_nritems(leaf);
	while (1) {
		if (path->slots[0] >= nritems) {
			ret = btrfs_next_leaf(root, path);
			if (ret) {
				if (ret > 0)
					return -ENOENT;
				return ret;
			}

			leaf = path->nodes[0];
			nritems = btrfs_header_nritems(leaf);
			recow = 1;
		}

		btrfs_item_key_to_cpu(leaf, &key, path->slots[0]);
		if (key.objectid != bytenr ||
		    key.type != BTRFS_EXTENT_DATA_REF_KEY)
			goto fail;

		ref = btrfs_item_ptr(leaf, path->slots[0],
				     struct btrfs_extent_data_ref);

		if (match_extent_data_ref(leaf, ref, root_objectid,
					  owner, offset)) {
			if (recow) {
				btrfs_release_path(path);
				goto again;
			}
			ret = 0;
			break;
		}
		path->slots[0]++;
	}
fail:
	return ret;
}

static noinline int insert_extent_data_ref(struct btrfs_trans_handle *trans,
					   struct btrfs_path *path,
					   const struct btrfs_delayed_ref_node *node,
					   u64 bytenr)
{
	struct btrfs_root *root = btrfs_extent_root(trans->fs_info, bytenr);
	struct btrfs_key key;
	struct extent_buffer *leaf;
	u64 owner = btrfs_delayed_ref_owner(node);
	u64 offset = btrfs_delayed_ref_offset(node);
	u32 size;
	u32 num_refs;
	int ret;

	key.objectid = bytenr;
	if (node->parent) {
		key.type = BTRFS_SHARED_DATA_REF_KEY;
		key.offset = node->parent;
		size = sizeof(struct btrfs_shared_data_ref);
	} else {
		key.type = BTRFS_EXTENT_DATA_REF_KEY;
		key.offset = hash_extent_data_ref(node->ref_root, owner, offset);
		size = sizeof(struct btrfs_extent_data_ref);
	}

	ret = btrfs_insert_empty_item(trans, root, path, &key, size);
	if (ret && ret != -EEXIST)
		goto fail;

	leaf = path->nodes[0];
	if (node->parent) {
		struct btrfs_shared_data_ref *ref;
		ref = btrfs_item_ptr(leaf, path->slots[0],
				     struct btrfs_shared_data_ref);
		if (ret == 0) {
			btrfs_set_shared_data_ref_count(leaf, ref, node->ref_mod);
		} else {
			num_refs = btrfs_shared_data_ref_count(leaf, ref);
			num_refs += node->ref_mod;
			btrfs_set_shared_data_ref_count(leaf, ref, num_refs);
		}
	} else {
		struct btrfs_extent_data_ref *ref;
		while (ret == -EEXIST) {
			ref = btrfs_item_ptr(leaf, path->slots[0],
					     struct btrfs_extent_data_ref);
			if (match_extent_data_ref(leaf, ref, node->ref_root,
						  owner, offset))
				break;
			btrfs_release_path(path);
			key.offset++;
			ret = btrfs_insert_empty_item(trans, root, path, &key,
						      size);
			if (ret && ret != -EEXIST)
				goto fail;

			leaf = path->nodes[0];
		}
		ref = btrfs_item_ptr(leaf, path->slots[0],
				     struct btrfs_extent_data_ref);
		if (ret == 0) {
			btrfs_set_extent_data_ref_root(leaf, ref, node->ref_root);
			btrfs_set_extent_data_ref_objectid(leaf, ref, owner);
			btrfs_set_extent_data_ref_offset(leaf, ref, offset);
			btrfs_set_extent_data_ref_count(leaf, ref, node->ref_mod);
		} else {
			num_refs = btrfs_extent_data_ref_count(leaf, ref);
			num_refs += node->ref_mod;
			btrfs_set_extent_data_ref_count(leaf, ref, num_refs);
		}
	}
	ret = 0;
fail:
	btrfs_release_path(path);
	return ret;
}

static noinline int remove_extent_data_ref(struct btrfs_trans_handle *trans,
					   struct btrfs_root *root,
					   struct btrfs_path *path,
					   int refs_to_drop)
{
	struct btrfs_key key;
	struct btrfs_extent_data_ref *ref1 = NULL;
	struct btrfs_shared_data_ref *ref2 = NULL;
	struct extent_buffer *leaf;
	u32 num_refs = 0;
	int ret = 0;

	leaf = path->nodes[0];
	btrfs_item_key_to_cpu(leaf, &key, path->slots[0]);

	if (key.type == BTRFS_EXTENT_DATA_REF_KEY) {
		ref1 = btrfs_item_ptr(leaf, path->slots[0],
				      struct btrfs_extent_data_ref);
		num_refs = btrfs_extent_data_ref_count(leaf, ref1);
	} else if (key.type == BTRFS_SHARED_DATA_REF_KEY) {
		ref2 = btrfs_item_ptr(leaf, path->slots[0],
				      struct btrfs_shared_data_ref);
		num_refs = btrfs_shared_data_ref_count(leaf, ref2);
	} else {
		btrfs_err(trans->fs_info,
			  "unrecognized backref key (%llu %u %llu)",
			  key.objectid, key.type, key.offset);
		btrfs_abort_transaction(trans, -EUCLEAN);
		return -EUCLEAN;
	}

	BUG_ON(num_refs < refs_to_drop);
	num_refs -= refs_to_drop;

	if (num_refs == 0) {
		ret = btrfs_del_item(trans, root, path);
	} else {
		if (key.type == BTRFS_EXTENT_DATA_REF_KEY)
			btrfs_set_extent_data_ref_count(leaf, ref1, num_refs);
		else if (key.type == BTRFS_SHARED_DATA_REF_KEY)
			btrfs_set_shared_data_ref_count(leaf, ref2, num_refs);
	}
	return ret;
}

static noinline u32 extent_data_ref_count(const struct btrfs_path *path,
					  const struct btrfs_extent_inline_ref *iref)
{
	struct btrfs_key key;
	struct extent_buffer *leaf;
	const struct btrfs_extent_data_ref *ref1;
	const struct btrfs_shared_data_ref *ref2;
	u32 num_refs = 0;
	int type;

	leaf = path->nodes[0];
	btrfs_item_key_to_cpu(leaf, &key, path->slots[0]);

	if (iref) {
		/*
		 * If type is invalid, we should have bailed out earlier than
		 * this call.
		 */
		type = btrfs_get_extent_inline_ref_type(leaf, iref, BTRFS_REF_TYPE_DATA);
		ASSERT(type != BTRFS_REF_TYPE_INVALID);
		if (type == BTRFS_EXTENT_DATA_REF_KEY) {
			ref1 = (const struct btrfs_extent_data_ref *)(&iref->offset);
			num_refs = btrfs_extent_data_ref_count(leaf, ref1);
		} else {
			ref2 = (const struct btrfs_shared_data_ref *)(iref + 1);
			num_refs = btrfs_shared_data_ref_count(leaf, ref2);
		}
	} else if (key.type == BTRFS_EXTENT_DATA_REF_KEY) {
		ref1 = btrfs_item_ptr(leaf, path->slots[0],
				      struct btrfs_extent_data_ref);
		num_refs = btrfs_extent_data_ref_count(leaf, ref1);
	} else if (key.type == BTRFS_SHARED_DATA_REF_KEY) {
		ref2 = btrfs_item_ptr(leaf, path->slots[0],
				      struct btrfs_shared_data_ref);
		num_refs = btrfs_shared_data_ref_count(leaf, ref2);
	} else {
		WARN_ON(1);
	}
	return num_refs;
}

static noinline int lookup_tree_block_ref(struct btrfs_trans_handle *trans,
					  struct btrfs_path *path,
					  u64 bytenr, u64 parent,
					  u64 root_objectid)
{
	struct btrfs_root *root = btrfs_extent_root(trans->fs_info, bytenr);
	struct btrfs_key key;
	int ret;

	key.objectid = bytenr;
	if (parent) {
		key.type = BTRFS_SHARED_BLOCK_REF_KEY;
		key.offset = parent;
	} else {
		key.type = BTRFS_TREE_BLOCK_REF_KEY;
		key.offset = root_objectid;
	}

	ret = btrfs_search_slot(trans, root, &key, path, -1, 1);
	if (ret > 0)
		ret = -ENOENT;
	return ret;
}

static noinline int insert_tree_block_ref(struct btrfs_trans_handle *trans,
					  struct btrfs_path *path,
					  const struct btrfs_delayed_ref_node *node,
					  u64 bytenr)
{
	struct btrfs_root *root = btrfs_extent_root(trans->fs_info, bytenr);
	struct btrfs_key key;
	int ret;

	key.objectid = bytenr;
	if (node->parent) {
		key.type = BTRFS_SHARED_BLOCK_REF_KEY;
		key.offset = node->parent;
	} else {
		key.type = BTRFS_TREE_BLOCK_REF_KEY;
		key.offset = node->ref_root;
	}

	ret = btrfs_insert_empty_item(trans, root, path, &key, 0);
	btrfs_release_path(path);
	return ret;
}

static inline int extent_ref_type(u64 parent, u64 owner)
{
	int type;
	if (owner < BTRFS_FIRST_FREE_OBJECTID) {
		if (parent > 0)
			type = BTRFS_SHARED_BLOCK_REF_KEY;
		else
			type = BTRFS_TREE_BLOCK_REF_KEY;
	} else {
		if (parent > 0)
			type = BTRFS_SHARED_DATA_REF_KEY;
		else
			type = BTRFS_EXTENT_DATA_REF_KEY;
	}
	return type;
}

static int find_next_key(const struct btrfs_path *path, int level,
			 struct btrfs_key *key)

{
	for (; level < BTRFS_MAX_LEVEL; level++) {
		if (!path->nodes[level])
			break;
		if (path->slots[level] + 1 >=
		    btrfs_header_nritems(path->nodes[level]))
			continue;
		if (level == 0)
			btrfs_item_key_to_cpu(path->nodes[level], key,
					      path->slots[level] + 1);
		else
			btrfs_node_key_to_cpu(path->nodes[level], key,
					      path->slots[level] + 1);
		return 0;
	}
	return 1;
}

/*
 * look for inline back ref. if back ref is found, *ref_ret is set
 * to the address of inline back ref, and 0 is returned.
 *
 * if back ref isn't found, *ref_ret is set to the address where it
 * should be inserted, and -ENOENT is returned.
 *
 * if insert is true and there are too many inline back refs, the path
 * points to the extent item, and -EAGAIN is returned.
 *
 * NOTE: inline back refs are ordered in the same way that back ref
 *	 items in the tree are ordered.
 */
static noinline_for_stack
int lookup_inline_extent_backref(struct btrfs_trans_handle *trans,
				 struct btrfs_path *path,
				 struct btrfs_extent_inline_ref **ref_ret,
				 u64 bytenr, u64 num_bytes,
				 u64 parent, u64 root_objectid,
				 u64 owner, u64 offset, int insert)
{
	struct btrfs_fs_info *fs_info = trans->fs_info;
	struct btrfs_root *root = btrfs_extent_root(fs_info, bytenr);
	struct btrfs_key key;
	struct extent_buffer *leaf;
	struct btrfs_extent_item *ei;
	struct btrfs_extent_inline_ref *iref;
	u64 flags;
	u64 item_size;
	unsigned long ptr;
	unsigned long end;
	int extra_size;
	int type;
	int want;
	int ret;
	bool skinny_metadata = btrfs_fs_incompat(fs_info, SKINNY_METADATA);
	int needed;

	key.objectid = bytenr;
	key.type = BTRFS_EXTENT_ITEM_KEY;
	key.offset = num_bytes;

	want = extent_ref_type(parent, owner);
	if (insert) {
		extra_size = btrfs_extent_inline_ref_size(want);
		path->search_for_extension = 1;
	} else
		extra_size = -1;

	/*
	 * Owner is our level, so we can just add one to get the level for the
	 * block we are interested in.
	 */
	if (skinny_metadata && owner < BTRFS_FIRST_FREE_OBJECTID) {
		key.type = BTRFS_METADATA_ITEM_KEY;
		key.offset = owner;
	}

again:
	ret = btrfs_search_slot(trans, root, &key, path, extra_size, 1);
	if (ret < 0)
		goto out;

	/*
	 * We may be a newly converted file system which still has the old fat
	 * extent entries for metadata, so try and see if we have one of those.
	 */
	if (ret > 0 && skinny_metadata) {
		skinny_metadata = false;
		if (path->slots[0]) {
			path->slots[0]--;
			btrfs_item_key_to_cpu(path->nodes[0], &key,
					      path->slots[0]);
			if (key.objectid == bytenr &&
			    key.type == BTRFS_EXTENT_ITEM_KEY &&
			    key.offset == num_bytes)
				ret = 0;
		}
		if (ret) {
			key.objectid = bytenr;
			key.type = BTRFS_EXTENT_ITEM_KEY;
			key.offset = num_bytes;
			btrfs_release_path(path);
			goto again;
		}
	}

	if (ret && !insert) {
		ret = -ENOENT;
		goto out;
	} else if (WARN_ON(ret)) {
		btrfs_print_leaf(path->nodes[0]);
		btrfs_err(fs_info,
"extent item not found for insert, bytenr %llu num_bytes %llu parent %llu root_objectid %llu owner %llu offset %llu",
			  bytenr, num_bytes, parent, root_objectid, owner,
			  offset);
		ret = -EUCLEAN;
		goto out;
	}

	leaf = path->nodes[0];
	item_size = btrfs_item_size(leaf, path->slots[0]);
	if (unlikely(item_size < sizeof(*ei))) {
		ret = -EUCLEAN;
		btrfs_err(fs_info,
			  "unexpected extent item size, has %llu expect >= %zu",
			  item_size, sizeof(*ei));
		btrfs_abort_transaction(trans, ret);
		goto out;
	}

	ei = btrfs_item_ptr(leaf, path->slots[0], struct btrfs_extent_item);
	flags = btrfs_extent_flags(leaf, ei);

	ptr = (unsigned long)(ei + 1);
	end = (unsigned long)ei + item_size;

	if (flags & BTRFS_EXTENT_FLAG_TREE_BLOCK && !skinny_metadata) {
		ptr += sizeof(struct btrfs_tree_block_info);
		BUG_ON(ptr > end);
	}

	if (owner >= BTRFS_FIRST_FREE_OBJECTID)
		needed = BTRFS_REF_TYPE_DATA;
	else
		needed = BTRFS_REF_TYPE_BLOCK;

	ret = -ENOENT;
	while (ptr < end) {
		iref = (struct btrfs_extent_inline_ref *)ptr;
		type = btrfs_get_extent_inline_ref_type(leaf, iref, needed);
		if (type == BTRFS_EXTENT_OWNER_REF_KEY) {
			ASSERT(btrfs_fs_incompat(fs_info, SIMPLE_QUOTA));
			ptr += btrfs_extent_inline_ref_size(type);
			continue;
		}
		if (type == BTRFS_REF_TYPE_INVALID) {
			ret = -EUCLEAN;
			goto out;
		}

		if (want < type)
			break;
		if (want > type) {
			ptr += btrfs_extent_inline_ref_size(type);
			continue;
		}

		if (type == BTRFS_EXTENT_DATA_REF_KEY) {
			struct btrfs_extent_data_ref *dref;
			dref = (struct btrfs_extent_data_ref *)(&iref->offset);
			if (match_extent_data_ref(leaf, dref, root_objectid,
						  owner, offset)) {
				ret = 0;
				break;
			}
			if (hash_extent_data_ref_item(leaf, dref) <
			    hash_extent_data_ref(root_objectid, owner, offset))
				break;
		} else {
			u64 ref_offset;
			ref_offset = btrfs_extent_inline_ref_offset(leaf, iref);
			if (parent > 0) {
				if (parent == ref_offset) {
					ret = 0;
					break;
				}
				if (ref_offset < parent)
					break;
			} else {
				if (root_objectid == ref_offset) {
					ret = 0;
					break;
				}
				if (ref_offset < root_objectid)
					break;
			}
		}
		ptr += btrfs_extent_inline_ref_size(type);
	}

	if (unlikely(ptr > end)) {
		ret = -EUCLEAN;
		btrfs_print_leaf(path->nodes[0]);
		btrfs_crit(fs_info,
"overrun extent record at slot %d while looking for inline extent for root %llu owner %llu offset %llu parent %llu",
			   path->slots[0], root_objectid, owner, offset, parent);
		goto out;
	}

	if (ret == -ENOENT && insert) {
		if (item_size + extra_size >=
		    BTRFS_MAX_EXTENT_ITEM_SIZE(root)) {
			ret = -EAGAIN;
			goto out;
		}

		if (path->slots[0] + 1 < btrfs_header_nritems(path->nodes[0])) {
			struct btrfs_key tmp_key;

			btrfs_item_key_to_cpu(path->nodes[0], &tmp_key, path->slots[0] + 1);
			if (tmp_key.objectid == bytenr &&
			    tmp_key.type < BTRFS_BLOCK_GROUP_ITEM_KEY) {
				ret = -EAGAIN;
				goto out;
			}
			goto out_no_entry;
		}

		if (!path->keep_locks) {
			btrfs_release_path(path);
			path->keep_locks = 1;
			goto again;
		}

		/*
		 * To add new inline back ref, we have to make sure
		 * there is no corresponding back ref item.
		 * For simplicity, we just do not add new inline back
		 * ref if there is any kind of item for this block
		 */
		if (find_next_key(path, 0, &key) == 0 &&
		    key.objectid == bytenr &&
		    key.type < BTRFS_BLOCK_GROUP_ITEM_KEY) {
			ret = -EAGAIN;
			goto out;
		}
	}
out_no_entry:
	*ref_ret = (struct btrfs_extent_inline_ref *)ptr;
out:
	if (path->keep_locks) {
		path->keep_locks = 0;
		btrfs_unlock_up_safe(path, 1);
	}
	if (insert)
		path->search_for_extension = 0;
	return ret;
}

/*
 * helper to add new inline back ref
 */
static noinline_for_stack
void setup_inline_extent_backref(struct btrfs_trans_handle *trans,
				 struct btrfs_path *path,
				 struct btrfs_extent_inline_ref *iref,
				 u64 parent, u64 root_objectid,
				 u64 owner, u64 offset, int refs_to_add,
				 struct btrfs_delayed_extent_op *extent_op)
{
	struct extent_buffer *leaf;
	struct btrfs_extent_item *ei;
	unsigned long ptr;
	unsigned long end;
	unsigned long item_offset;
	u64 refs;
	int size;
	int type;

	leaf = path->nodes[0];
	ei = btrfs_item_ptr(leaf, path->slots[0], struct btrfs_extent_item);
	item_offset = (unsigned long)iref - (unsigned long)ei;

	type = extent_ref_type(parent, owner);
	size = btrfs_extent_inline_ref_size(type);

	btrfs_extend_item(trans, path, size);

	ei = btrfs_item_ptr(leaf, path->slots[0], struct btrfs_extent_item);
	refs = btrfs_extent_refs(leaf, ei);
	refs += refs_to_add;
	btrfs_set_extent_refs(leaf, ei, refs);
	if (extent_op)
		__run_delayed_extent_op(extent_op, leaf, ei);

	ptr = (unsigned long)ei + item_offset;
	end = (unsigned long)ei + btrfs_item_size(leaf, path->slots[0]);
	if (ptr < end - size)
		memmove_extent_buffer(leaf, ptr + size, ptr,
				      end - size - ptr);

	iref = (struct btrfs_extent_inline_ref *)ptr;
	btrfs_set_extent_inline_ref_type(leaf, iref, type);
	if (type == BTRFS_EXTENT_DATA_REF_KEY) {
		struct btrfs_extent_data_ref *dref;
		dref = (struct btrfs_extent_data_ref *)(&iref->offset);
		btrfs_set_extent_data_ref_root(leaf, dref, root_objectid);
		btrfs_set_extent_data_ref_objectid(leaf, dref, owner);
		btrfs_set_extent_data_ref_offset(leaf, dref, offset);
		btrfs_set_extent_data_ref_count(leaf, dref, refs_to_add);
	} else if (type == BTRFS_SHARED_DATA_REF_KEY) {
		struct btrfs_shared_data_ref *sref;
		sref = (struct btrfs_shared_data_ref *)(iref + 1);
		btrfs_set_shared_data_ref_count(leaf, sref, refs_to_add);
		btrfs_set_extent_inline_ref_offset(leaf, iref, parent);
	} else if (type == BTRFS_SHARED_BLOCK_REF_KEY) {
		btrfs_set_extent_inline_ref_offset(leaf, iref, parent);
	} else {
		btrfs_set_extent_inline_ref_offset(leaf, iref, root_objectid);
	}
}

static int lookup_extent_backref(struct btrfs_trans_handle *trans,
				 struct btrfs_path *path,
				 struct btrfs_extent_inline_ref **ref_ret,
				 u64 bytenr, u64 num_bytes, u64 parent,
				 u64 root_objectid, u64 owner, u64 offset)
{
	int ret;

	ret = lookup_inline_extent_backref(trans, path, ref_ret, bytenr,
					   num_bytes, parent, root_objectid,
					   owner, offset, 0);
	if (ret != -ENOENT)
		return ret;

	btrfs_release_path(path);
	*ref_ret = NULL;

	if (owner < BTRFS_FIRST_FREE_OBJECTID) {
		ret = lookup_tree_block_ref(trans, path, bytenr, parent,
					    root_objectid);
	} else {
		ret = lookup_extent_data_ref(trans, path, bytenr, parent,
					     root_objectid, owner, offset);
	}
	return ret;
}

/*
 * helper to update/remove inline back ref
 */
static noinline_for_stack int update_inline_extent_backref(
				  struct btrfs_trans_handle *trans,
				  struct btrfs_path *path,
				  struct btrfs_extent_inline_ref *iref,
				  int refs_to_mod,
				  struct btrfs_delayed_extent_op *extent_op)
{
	struct extent_buffer *leaf = path->nodes[0];
	struct btrfs_fs_info *fs_info = leaf->fs_info;
	struct btrfs_extent_item *ei;
	struct btrfs_extent_data_ref *dref = NULL;
	struct btrfs_shared_data_ref *sref = NULL;
	unsigned long ptr;
	unsigned long end;
	u32 item_size;
	int size;
	int type;
	u64 refs;

	ei = btrfs_item_ptr(leaf, path->slots[0], struct btrfs_extent_item);
	refs = btrfs_extent_refs(leaf, ei);
	if (unlikely(refs_to_mod < 0 && refs + refs_to_mod <= 0)) {
		struct btrfs_key key;
		u32 extent_size;

		btrfs_item_key_to_cpu(leaf, &key, path->slots[0]);
		if (key.type == BTRFS_METADATA_ITEM_KEY)
			extent_size = fs_info->nodesize;
		else
			extent_size = key.offset;
		btrfs_print_leaf(leaf);
		btrfs_err(fs_info,
	"invalid refs_to_mod for extent %llu num_bytes %u, has %d expect >= -%llu",
			  key.objectid, extent_size, refs_to_mod, refs);
		return -EUCLEAN;
	}
	refs += refs_to_mod;
	btrfs_set_extent_refs(leaf, ei, refs);
	if (extent_op)
		__run_delayed_extent_op(extent_op, leaf, ei);

	type = btrfs_get_extent_inline_ref_type(leaf, iref, BTRFS_REF_TYPE_ANY);
	/*
	 * Function btrfs_get_extent_inline_ref_type() has already printed
	 * error messages.
	 */
	if (unlikely(type == BTRFS_REF_TYPE_INVALID))
		return -EUCLEAN;

	if (type == BTRFS_EXTENT_DATA_REF_KEY) {
		dref = (struct btrfs_extent_data_ref *)(&iref->offset);
		refs = btrfs_extent_data_ref_count(leaf, dref);
	} else if (type == BTRFS_SHARED_DATA_REF_KEY) {
		sref = (struct btrfs_shared_data_ref *)(iref + 1);
		refs = btrfs_shared_data_ref_count(leaf, sref);
	} else {
		refs = 1;
		/*
		 * For tree blocks we can only drop one ref for it, and tree
		 * blocks should not have refs > 1.
		 *
		 * Furthermore if we're inserting a new inline backref, we
		 * won't reach this path either. That would be
		 * setup_inline_extent_backref().
		 */
		if (unlikely(refs_to_mod != -1)) {
			struct btrfs_key key;

			btrfs_item_key_to_cpu(leaf, &key, path->slots[0]);

			btrfs_print_leaf(leaf);
			btrfs_err(fs_info,
			"invalid refs_to_mod for tree block %llu, has %d expect -1",
				  key.objectid, refs_to_mod);
			return -EUCLEAN;
		}
	}

	if (unlikely(refs_to_mod < 0 && refs < -refs_to_mod)) {
		struct btrfs_key key;
		u32 extent_size;

		btrfs_item_key_to_cpu(leaf, &key, path->slots[0]);
		if (key.type == BTRFS_METADATA_ITEM_KEY)
			extent_size = fs_info->nodesize;
		else
			extent_size = key.offset;
		btrfs_print_leaf(leaf);
		btrfs_err(fs_info,
"invalid refs_to_mod for backref entry, iref %lu extent %llu num_bytes %u, has %d expect >= -%llu",
			  (unsigned long)iref, key.objectid, extent_size,
			  refs_to_mod, refs);
		return -EUCLEAN;
	}
	refs += refs_to_mod;

	if (refs > 0) {
		if (type == BTRFS_EXTENT_DATA_REF_KEY)
			btrfs_set_extent_data_ref_count(leaf, dref, refs);
		else
			btrfs_set_shared_data_ref_count(leaf, sref, refs);
	} else {
		size =  btrfs_extent_inline_ref_size(type);
		item_size = btrfs_item_size(leaf, path->slots[0]);
		ptr = (unsigned long)iref;
		end = (unsigned long)ei + item_size;
		if (ptr + size < end)
			memmove_extent_buffer(leaf, ptr, ptr + size,
					      end - ptr - size);
		item_size -= size;
		btrfs_truncate_item(trans, path, item_size, 1);
	}
	return 0;
}

static noinline_for_stack
int insert_inline_extent_backref(struct btrfs_trans_handle *trans,
				 struct btrfs_path *path,
				 u64 bytenr, u64 num_bytes, u64 parent,
				 u64 root_objectid, u64 owner,
				 u64 offset, int refs_to_add,
				 struct btrfs_delayed_extent_op *extent_op)
{
	struct btrfs_extent_inline_ref *iref;
	int ret;

	ret = lookup_inline_extent_backref(trans, path, &iref, bytenr,
					   num_bytes, parent, root_objectid,
					   owner, offset, 1);
	if (ret == 0) {
		/*
		 * We're adding refs to a tree block we already own, this
		 * should not happen at all.
		 */
		if (owner < BTRFS_FIRST_FREE_OBJECTID) {
			btrfs_print_leaf(path->nodes[0]);
			btrfs_crit(trans->fs_info,
"adding refs to an existing tree ref, bytenr %llu num_bytes %llu root_objectid %llu slot %u",
				   bytenr, num_bytes, root_objectid, path->slots[0]);
			return -EUCLEAN;
		}
		ret = update_inline_extent_backref(trans, path, iref,
						   refs_to_add, extent_op);
	} else if (ret == -ENOENT) {
		setup_inline_extent_backref(trans, path, iref, parent,
					    root_objectid, owner, offset,
					    refs_to_add, extent_op);
		ret = 0;
	}
	return ret;
}

static int remove_extent_backref(struct btrfs_trans_handle *trans,
				 struct btrfs_root *root,
				 struct btrfs_path *path,
				 struct btrfs_extent_inline_ref *iref,
				 int refs_to_drop, int is_data)
{
	int ret = 0;

	BUG_ON(!is_data && refs_to_drop != 1);
	if (iref)
		ret = update_inline_extent_backref(trans, path, iref,
						   -refs_to_drop, NULL);
	else if (is_data)
		ret = remove_extent_data_ref(trans, root, path, refs_to_drop);
	else
		ret = btrfs_del_item(trans, root, path);
	return ret;
}

static int btrfs_issue_discard(struct block_device *bdev, u64 start, u64 len,
			       u64 *discarded_bytes)
{
	int j, ret = 0;
	u64 bytes_left, end;
	u64 aligned_start = ALIGN(start, SECTOR_SIZE);

	/* Adjust the range to be aligned to 512B sectors if necessary. */
	if (start != aligned_start) {
		len -= aligned_start - start;
		len = round_down(len, SECTOR_SIZE);
		start = aligned_start;
	}

	*discarded_bytes = 0;

	if (!len)
		return 0;

	end = start + len;
	bytes_left = len;

	/* Skip any superblocks on this device. */
	for (j = 0; j < BTRFS_SUPER_MIRROR_MAX; j++) {
		u64 sb_start = btrfs_sb_offset(j);
		u64 sb_end = sb_start + BTRFS_SUPER_INFO_SIZE;
		u64 size = sb_start - start;

		if (!in_range(sb_start, start, bytes_left) &&
		    !in_range(sb_end, start, bytes_left) &&
		    !in_range(start, sb_start, BTRFS_SUPER_INFO_SIZE))
			continue;

		/*
		 * Superblock spans beginning of range.  Adjust start and
		 * try again.
		 */
		if (sb_start <= start) {
			start += sb_end - start;
			if (start > end) {
				bytes_left = 0;
				break;
			}
			bytes_left = end - start;
			continue;
		}

		if (size) {
			ret = blkdev_issue_discard(bdev, start >> SECTOR_SHIFT,
						   size >> SECTOR_SHIFT,
						   GFP_NOFS);
			if (!ret)
				*discarded_bytes += size;
			else if (ret != -EOPNOTSUPP)
				return ret;
		}

		start = sb_end;
		if (start > end) {
			bytes_left = 0;
			break;
		}
		bytes_left = end - start;
	}

	while (bytes_left) {
		u64 bytes_to_discard = min(BTRFS_MAX_DISCARD_CHUNK_SIZE, bytes_left);

		ret = blkdev_issue_discard(bdev, start >> SECTOR_SHIFT,
					   bytes_to_discard >> SECTOR_SHIFT,
					   GFP_NOFS);

		if (ret) {
			if (ret != -EOPNOTSUPP)
				break;
			continue;
		}

		start += bytes_to_discard;
		bytes_left -= bytes_to_discard;
		*discarded_bytes += bytes_to_discard;

		if (btrfs_trim_interrupted()) {
			ret = -ERESTARTSYS;
			break;
		}
	}

	return ret;
}

static int do_discard_extent(struct btrfs_discard_stripe *stripe, u64 *bytes)
{
	struct btrfs_device *dev = stripe->dev;
	struct btrfs_fs_info *fs_info = dev->fs_info;
	struct btrfs_dev_replace *dev_replace = &fs_info->dev_replace;
	u64 phys = stripe->physical;
	u64 len = stripe->length;
	u64 discarded = 0;
	int ret = 0;

	/* Zone reset on a zoned filesystem */
	if (btrfs_can_zone_reset(dev, phys, len)) {
		u64 src_disc;

		ret = btrfs_reset_device_zone(dev, phys, len, &discarded);
		if (ret)
			goto out;

		if (!btrfs_dev_replace_is_ongoing(dev_replace) ||
		    dev != dev_replace->srcdev)
			goto out;

		src_disc = discarded;

		/* Send to replace target as well */
		ret = btrfs_reset_device_zone(dev_replace->tgtdev, phys, len,
					      &discarded);
		discarded += src_disc;
	} else if (bdev_max_discard_sectors(stripe->dev->bdev)) {
		ret = btrfs_issue_discard(dev->bdev, phys, len, &discarded);
	} else {
		ret = 0;
		*bytes = 0;
	}

out:
	*bytes = discarded;
	return ret;
}

int btrfs_discard_extent(struct btrfs_fs_info *fs_info, u64 bytenr,
			 u64 num_bytes, u64 *actual_bytes)
{
	int ret = 0;
	u64 discarded_bytes = 0;
	u64 end = bytenr + num_bytes;
	u64 cur = bytenr;

	/*
	 * Avoid races with device replace and make sure the devices in the
	 * stripes don't go away while we are discarding.
	 */
	btrfs_bio_counter_inc_blocked(fs_info);
	while (cur < end) {
		struct btrfs_discard_stripe *stripes;
		unsigned int num_stripes;
		int i;

		num_bytes = end - cur;
		stripes = btrfs_map_discard(fs_info, cur, &num_bytes, &num_stripes);
		if (IS_ERR(stripes)) {
			ret = PTR_ERR(stripes);
			if (ret == -EOPNOTSUPP)
				ret = 0;
			break;
		}

		for (i = 0; i < num_stripes; i++) {
			struct btrfs_discard_stripe *stripe = stripes + i;
			u64 bytes;

			if (!stripe->dev->bdev) {
				ASSERT(btrfs_test_opt(fs_info, DEGRADED));
				continue;
			}

			if (!test_bit(BTRFS_DEV_STATE_WRITEABLE,
					&stripe->dev->dev_state))
				continue;

			ret = do_discard_extent(stripe, &bytes);
			if (ret) {
				/*
				 * Keep going if discard is not supported by the
				 * device.
				 */
				if (ret != -EOPNOTSUPP)
					break;
				ret = 0;
			} else {
				discarded_bytes += bytes;
			}
		}
		kfree(stripes);
		if (ret)
			break;
		cur += num_bytes;
	}
	btrfs_bio_counter_dec(fs_info);
	if (actual_bytes)
		*actual_bytes = discarded_bytes;
	return ret;
}

/* Can return -ENOMEM */
int btrfs_inc_extent_ref(struct btrfs_trans_handle *trans,
			 struct btrfs_ref *generic_ref)
{
	struct btrfs_fs_info *fs_info = trans->fs_info;
	int ret;

	ASSERT(generic_ref->type != BTRFS_REF_NOT_SET &&
	       generic_ref->action);
	BUG_ON(generic_ref->type == BTRFS_REF_METADATA &&
	       generic_ref->ref_root == BTRFS_TREE_LOG_OBJECTID);

	if (generic_ref->type == BTRFS_REF_METADATA)
		ret = btrfs_add_delayed_tree_ref(trans, generic_ref, NULL);
	else
		ret = btrfs_add_delayed_data_ref(trans, generic_ref, 0);

	btrfs_ref_tree_mod(fs_info, generic_ref);

	return ret;
}

/*
 * Insert backreference for a given extent.
 *
 * The counterpart is in __btrfs_free_extent(), with examples and more details
 * how it works.
 *
 * @trans:	    Handle of transaction
 *
 * @node:	    The delayed ref node used to get the bytenr/length for
 *		    extent whose references are incremented.
 *
 * @extent_op       Pointer to a structure, holding information necessary when
 *                  updating a tree block's flags
 *
 */
static int __btrfs_inc_extent_ref(struct btrfs_trans_handle *trans,
				  const struct btrfs_delayed_ref_node *node,
				  struct btrfs_delayed_extent_op *extent_op)
{
	BTRFS_PATH_AUTO_FREE(path);
	struct extent_buffer *leaf;
	struct btrfs_extent_item *item;
	struct btrfs_key key;
	u64 bytenr = node->bytenr;
	u64 num_bytes = node->num_bytes;
	u64 owner = btrfs_delayed_ref_owner(node);
	u64 offset = btrfs_delayed_ref_offset(node);
	u64 refs;
	int refs_to_add = node->ref_mod;
	int ret;

	path = btrfs_alloc_path();
	if (!path)
		return -ENOMEM;

	/* this will setup the path even if it fails to insert the back ref */
	ret = insert_inline_extent_backref(trans, path, bytenr, num_bytes,
					   node->parent, node->ref_root, owner,
					   offset, refs_to_add, extent_op);
	if ((ret < 0 && ret != -EAGAIN) || !ret)
		return ret;

	/*
	 * Ok we had -EAGAIN which means we didn't have space to insert and
	 * inline extent ref, so just update the reference count and add a
	 * normal backref.
	 */
	leaf = path->nodes[0];
	btrfs_item_key_to_cpu(leaf, &key, path->slots[0]);
	item = btrfs_item_ptr(leaf, path->slots[0], struct btrfs_extent_item);
	refs = btrfs_extent_refs(leaf, item);
	btrfs_set_extent_refs(leaf, item, refs + refs_to_add);
	if (extent_op)
		__run_delayed_extent_op(extent_op, leaf, item);

	btrfs_release_path(path);

	/* now insert the actual backref */
	if (owner < BTRFS_FIRST_FREE_OBJECTID) {
		ret = insert_tree_block_ref(trans, path, node, bytenr);
		if (ret)
			btrfs_abort_transaction(trans, ret);
	} else {
		ret = insert_extent_data_ref(trans, path, node, bytenr);
		if (ret)
			btrfs_abort_transaction(trans, ret);
	}

	return ret;
}

static void free_head_ref_squota_rsv(struct btrfs_fs_info *fs_info,
				     const struct btrfs_delayed_ref_head *href)
{
	u64 root = href->owning_root;

	/*
	 * Don't check must_insert_reserved, as this is called from contexts
	 * where it has already been unset.
	 */
	if (btrfs_qgroup_mode(fs_info) != BTRFS_QGROUP_MODE_SIMPLE ||
	    !href->is_data || !btrfs_is_fstree(root))
		return;

	btrfs_qgroup_free_refroot(fs_info, root, href->reserved_bytes,
				  BTRFS_QGROUP_RSV_DATA);
}

static int run_delayed_data_ref(struct btrfs_trans_handle *trans,
				struct btrfs_delayed_ref_head *href,
				const struct btrfs_delayed_ref_node *node,
				struct btrfs_delayed_extent_op *extent_op,
				bool insert_reserved)
{
	int ret = 0;
	u64 parent = 0;
	u64 flags = 0;

	trace_run_delayed_data_ref(trans->fs_info, node);

	if (node->type == BTRFS_SHARED_DATA_REF_KEY)
		parent = node->parent;

	if (node->action == BTRFS_ADD_DELAYED_REF && insert_reserved) {
		struct btrfs_key key;
		struct btrfs_squota_delta delta = {
			.root = href->owning_root,
			.num_bytes = node->num_bytes,
			.is_data = true,
			.is_inc	= true,
			.generation = trans->transid,
		};
		u64 owner = btrfs_delayed_ref_owner(node);
		u64 offset = btrfs_delayed_ref_offset(node);

		if (extent_op)
			flags |= extent_op->flags_to_set;

		key.objectid = node->bytenr;
		key.type = BTRFS_EXTENT_ITEM_KEY;
		key.offset = node->num_bytes;

		ret = alloc_reserved_file_extent(trans, parent, node->ref_root,
						 flags, owner, offset, &key,
						 node->ref_mod,
						 href->owning_root);
		free_head_ref_squota_rsv(trans->fs_info, href);
		if (!ret)
			ret = btrfs_record_squota_delta(trans->fs_info, &delta);
	} else if (node->action == BTRFS_ADD_DELAYED_REF) {
		ret = __btrfs_inc_extent_ref(trans, node, extent_op);
	} else if (node->action == BTRFS_DROP_DELAYED_REF) {
		ret = __btrfs_free_extent(trans, href, node, extent_op);
	} else {
		BUG();
	}
	return ret;
}

static void __run_delayed_extent_op(struct btrfs_delayed_extent_op *extent_op,
				    struct extent_buffer *leaf,
				    struct btrfs_extent_item *ei)
{
	u64 flags = btrfs_extent_flags(leaf, ei);
	if (extent_op->update_flags) {
		flags |= extent_op->flags_to_set;
		btrfs_set_extent_flags(leaf, ei, flags);
	}

	if (extent_op->update_key) {
		struct btrfs_tree_block_info *bi;
		BUG_ON(!(flags & BTRFS_EXTENT_FLAG_TREE_BLOCK));
		bi = (struct btrfs_tree_block_info *)(ei + 1);
		btrfs_set_tree_block_key(leaf, bi, &extent_op->key);
	}
}

static int run_delayed_extent_op(struct btrfs_trans_handle *trans,
				 const struct btrfs_delayed_ref_head *head,
				 struct btrfs_delayed_extent_op *extent_op)
{
	struct btrfs_fs_info *fs_info = trans->fs_info;
	struct btrfs_root *root;
	struct btrfs_key key;
	BTRFS_PATH_AUTO_FREE(path);
	struct btrfs_extent_item *ei;
	struct extent_buffer *leaf;
	u32 item_size;
	int ret;
	int metadata = 1;

	if (TRANS_ABORTED(trans))
		return 0;

	if (!btrfs_fs_incompat(fs_info, SKINNY_METADATA))
		metadata = 0;

	path = btrfs_alloc_path();
	if (!path)
		return -ENOMEM;

	key.objectid = head->bytenr;

	if (metadata) {
		key.type = BTRFS_METADATA_ITEM_KEY;
		key.offset = head->level;
	} else {
		key.type = BTRFS_EXTENT_ITEM_KEY;
		key.offset = head->num_bytes;
	}

	root = btrfs_extent_root(fs_info, key.objectid);
again:
	ret = btrfs_search_slot(trans, root, &key, path, 0, 1);
	if (ret < 0) {
		return ret;
	} else if (ret > 0) {
		if (metadata) {
			if (path->slots[0] > 0) {
				path->slots[0]--;
				btrfs_item_key_to_cpu(path->nodes[0], &key,
						      path->slots[0]);
				if (key.objectid == head->bytenr &&
				    key.type == BTRFS_EXTENT_ITEM_KEY &&
				    key.offset == head->num_bytes)
					ret = 0;
			}
			if (ret > 0) {
				btrfs_release_path(path);
				metadata = 0;

				key.objectid = head->bytenr;
				key.type = BTRFS_EXTENT_ITEM_KEY;
				key.offset = head->num_bytes;
				goto again;
			}
		} else {
			ret = -EUCLEAN;
			btrfs_err(fs_info,
		  "missing extent item for extent %llu num_bytes %llu level %d",
				  head->bytenr, head->num_bytes, head->level);
			return ret;
		}
	}

	leaf = path->nodes[0];
	item_size = btrfs_item_size(leaf, path->slots[0]);

	if (unlikely(item_size < sizeof(*ei))) {
		ret = -EUCLEAN;
		btrfs_err(fs_info,
			  "unexpected extent item size, has %u expect >= %zu",
			  item_size, sizeof(*ei));
		btrfs_abort_transaction(trans, ret);
		return ret;
	}

	ei = btrfs_item_ptr(leaf, path->slots[0], struct btrfs_extent_item);
	__run_delayed_extent_op(extent_op, leaf, ei);

	return ret;
}

static int run_delayed_tree_ref(struct btrfs_trans_handle *trans,
				struct btrfs_delayed_ref_head *href,
				const struct btrfs_delayed_ref_node *node,
				struct btrfs_delayed_extent_op *extent_op,
				bool insert_reserved)
{
	int ret = 0;
	struct btrfs_fs_info *fs_info = trans->fs_info;
	u64 parent = 0;
	u64 ref_root = 0;

	trace_run_delayed_tree_ref(trans->fs_info, node);

	if (node->type == BTRFS_SHARED_BLOCK_REF_KEY)
		parent = node->parent;
	ref_root = node->ref_root;

	if (unlikely(node->ref_mod != 1)) {
		btrfs_err(trans->fs_info,
	"btree block %llu has %d references rather than 1: action %d ref_root %llu parent %llu",
			  node->bytenr, node->ref_mod, node->action, ref_root,
			  parent);
		return -EUCLEAN;
	}
	if (node->action == BTRFS_ADD_DELAYED_REF && insert_reserved) {
		struct btrfs_squota_delta delta = {
			.root = href->owning_root,
			.num_bytes = fs_info->nodesize,
			.is_data = false,
			.is_inc = true,
			.generation = trans->transid,
		};

		ret = alloc_reserved_tree_block(trans, node, extent_op);
		if (!ret)
			btrfs_record_squota_delta(fs_info, &delta);
	} else if (node->action == BTRFS_ADD_DELAYED_REF) {
		ret = __btrfs_inc_extent_ref(trans, node, extent_op);
	} else if (node->action == BTRFS_DROP_DELAYED_REF) {
		ret = __btrfs_free_extent(trans, href, node, extent_op);
	} else {
		BUG();
	}
	return ret;
}

/* helper function to actually process a single delayed ref entry */
static int run_one_delayed_ref(struct btrfs_trans_handle *trans,
			       struct btrfs_delayed_ref_head *href,
			       const struct btrfs_delayed_ref_node *node,
			       struct btrfs_delayed_extent_op *extent_op,
			       bool insert_reserved)
{
	int ret = 0;

	if (TRANS_ABORTED(trans)) {
		if (insert_reserved) {
			btrfs_pin_extent(trans, node->bytenr, node->num_bytes, 1);
			free_head_ref_squota_rsv(trans->fs_info, href);
		}
		return 0;
	}

	if (node->type == BTRFS_TREE_BLOCK_REF_KEY ||
	    node->type == BTRFS_SHARED_BLOCK_REF_KEY)
		ret = run_delayed_tree_ref(trans, href, node, extent_op,
					   insert_reserved);
	else if (node->type == BTRFS_EXTENT_DATA_REF_KEY ||
		 node->type == BTRFS_SHARED_DATA_REF_KEY)
		ret = run_delayed_data_ref(trans, href, node, extent_op,
					   insert_reserved);
	else if (node->type == BTRFS_EXTENT_OWNER_REF_KEY)
		ret = 0;
	else
		BUG();
	if (ret && insert_reserved)
		btrfs_pin_extent(trans, node->bytenr, node->num_bytes, 1);
	if (ret < 0)
		btrfs_err(trans->fs_info,
"failed to run delayed ref for logical %llu num_bytes %llu type %u action %u ref_mod %d: %d",
			  node->bytenr, node->num_bytes, node->type,
			  node->action, node->ref_mod, ret);
	return ret;
}

static struct btrfs_delayed_extent_op *cleanup_extent_op(
				struct btrfs_delayed_ref_head *head)
{
	struct btrfs_delayed_extent_op *extent_op = head->extent_op;

	if (!extent_op)
		return NULL;

	if (head->must_insert_reserved) {
		head->extent_op = NULL;
		btrfs_free_delayed_extent_op(extent_op);
		return NULL;
	}
	return extent_op;
}

static int run_and_cleanup_extent_op(struct btrfs_trans_handle *trans,
				     struct btrfs_delayed_ref_head *head)
{
	struct btrfs_delayed_extent_op *extent_op;
	int ret;

	extent_op = cleanup_extent_op(head);
	if (!extent_op)
		return 0;
	head->extent_op = NULL;
	spin_unlock(&head->lock);
	ret = run_delayed_extent_op(trans, head, extent_op);
	btrfs_free_delayed_extent_op(extent_op);
	return ret ? ret : 1;
}

u64 btrfs_cleanup_ref_head_accounting(struct btrfs_fs_info *fs_info,
				  struct btrfs_delayed_ref_root *delayed_refs,
				  struct btrfs_delayed_ref_head *head)
{
	u64 ret = 0;

	/*
	 * We had csum deletions accounted for in our delayed refs rsv, we need
	 * to drop the csum leaves for this update from our delayed_refs_rsv.
	 */
	if (head->total_ref_mod < 0 && head->is_data) {
		int nr_csums;

		spin_lock(&delayed_refs->lock);
		delayed_refs->pending_csums -= head->num_bytes;
		spin_unlock(&delayed_refs->lock);
		nr_csums = btrfs_csum_bytes_to_leaves(fs_info, head->num_bytes);

		btrfs_delayed_refs_rsv_release(fs_info, 0, nr_csums);

		ret = btrfs_calc_delayed_ref_csum_bytes(fs_info, nr_csums);
	}
	/* must_insert_reserved can be set only if we didn't run the head ref. */
	if (head->must_insert_reserved)
		free_head_ref_squota_rsv(fs_info, head);

	return ret;
}

static int cleanup_ref_head(struct btrfs_trans_handle *trans,
			    struct btrfs_delayed_ref_head *head,
			    u64 *bytes_released)
{

	struct btrfs_fs_info *fs_info = trans->fs_info;
	struct btrfs_delayed_ref_root *delayed_refs;
	int ret;

	delayed_refs = &trans->transaction->delayed_refs;

	ret = run_and_cleanup_extent_op(trans, head);
	if (ret < 0) {
		btrfs_unselect_ref_head(delayed_refs, head);
		btrfs_debug(fs_info, "run_delayed_extent_op returned %d", ret);
		return ret;
	} else if (ret) {
		return ret;
	}

	/*
	 * Need to drop our head ref lock and re-acquire the delayed ref lock
	 * and then re-check to make sure nobody got added.
	 */
	spin_unlock(&head->lock);
	spin_lock(&delayed_refs->lock);
	spin_lock(&head->lock);
	if (!RB_EMPTY_ROOT(&head->ref_tree.rb_root) || head->extent_op) {
		spin_unlock(&head->lock);
		spin_unlock(&delayed_refs->lock);
		return 1;
	}
	btrfs_delete_ref_head(fs_info, delayed_refs, head);
	spin_unlock(&head->lock);
	spin_unlock(&delayed_refs->lock);

	if (head->must_insert_reserved) {
		btrfs_pin_extent(trans, head->bytenr, head->num_bytes, 1);
		if (head->is_data) {
			struct btrfs_root *csum_root;

			csum_root = btrfs_csum_root(fs_info, head->bytenr);
			ret = btrfs_del_csums(trans, csum_root, head->bytenr,
					      head->num_bytes);
		}
	}

	*bytes_released += btrfs_cleanup_ref_head_accounting(fs_info, delayed_refs, head);

	trace_run_delayed_ref_head(fs_info, head, 0);
	btrfs_delayed_ref_unlock(head);
	btrfs_put_delayed_ref_head(head);
	return ret;
}

static int btrfs_run_delayed_refs_for_head(struct btrfs_trans_handle *trans,
					   struct btrfs_delayed_ref_head *locked_ref,
					   u64 *bytes_released)
{
	struct btrfs_fs_info *fs_info = trans->fs_info;
	struct btrfs_delayed_ref_root *delayed_refs;
	struct btrfs_delayed_extent_op *extent_op;
	struct btrfs_delayed_ref_node *ref;
	bool must_insert_reserved;
	int ret;

	delayed_refs = &trans->transaction->delayed_refs;

	lockdep_assert_held(&locked_ref->mutex);
	lockdep_assert_held(&locked_ref->lock);

	while ((ref = btrfs_select_delayed_ref(locked_ref))) {
		if (ref->seq &&
		    btrfs_check_delayed_seq(fs_info, ref->seq)) {
			spin_unlock(&locked_ref->lock);
			btrfs_unselect_ref_head(delayed_refs, locked_ref);
			return -EAGAIN;
		}

		rb_erase_cached(&ref->ref_node, &locked_ref->ref_tree);
		RB_CLEAR_NODE(&ref->ref_node);
		if (!list_empty(&ref->add_list))
			list_del(&ref->add_list);
		/*
		 * When we play the delayed ref, also correct the ref_mod on
		 * head
		 */
		switch (ref->action) {
		case BTRFS_ADD_DELAYED_REF:
		case BTRFS_ADD_DELAYED_EXTENT:
			locked_ref->ref_mod -= ref->ref_mod;
			break;
		case BTRFS_DROP_DELAYED_REF:
			locked_ref->ref_mod += ref->ref_mod;
			break;
		default:
			WARN_ON(1);
		}

		/*
		 * Record the must_insert_reserved flag before we drop the
		 * spin lock.
		 */
		must_insert_reserved = locked_ref->must_insert_reserved;
		/*
		 * Unsetting this on the head ref relinquishes ownership of
		 * the rsv_bytes, so it is critical that every possible code
		 * path from here forward frees all reserves including qgroup
		 * reserve.
		 */
		locked_ref->must_insert_reserved = false;

		extent_op = locked_ref->extent_op;
		locked_ref->extent_op = NULL;
		spin_unlock(&locked_ref->lock);

		ret = run_one_delayed_ref(trans, locked_ref, ref, extent_op,
					  must_insert_reserved);
		btrfs_delayed_refs_rsv_release(fs_info, 1, 0);
		*bytes_released += btrfs_calc_delayed_ref_bytes(fs_info, 1);

		btrfs_free_delayed_extent_op(extent_op);
		if (ret) {
			btrfs_unselect_ref_head(delayed_refs, locked_ref);
			btrfs_put_delayed_ref(ref);
			return ret;
		}

		btrfs_put_delayed_ref(ref);
		cond_resched();

		spin_lock(&locked_ref->lock);
		btrfs_merge_delayed_refs(fs_info, delayed_refs, locked_ref);
	}

	return 0;
}

/*
 * Returns 0 on success or if called with an already aborted transaction.
 * Returns -ENOMEM or -EIO on failure and will abort the transaction.
 */
static noinline int __btrfs_run_delayed_refs(struct btrfs_trans_handle *trans,
					     u64 min_bytes)
{
	struct btrfs_fs_info *fs_info = trans->fs_info;
	struct btrfs_delayed_ref_root *delayed_refs;
	struct btrfs_delayed_ref_head *locked_ref = NULL;
	int ret;
	unsigned long count = 0;
	unsigned long max_count = 0;
	u64 bytes_processed = 0;

	delayed_refs = &trans->transaction->delayed_refs;
	if (min_bytes == 0) {
		/*
		 * We may be subject to a harmless race if some task is
		 * concurrently adding or removing a delayed ref, so silence
		 * KCSAN and similar tools.
		 */
		max_count = data_race(delayed_refs->num_heads_ready);
		min_bytes = U64_MAX;
	}

	do {
		if (!locked_ref) {
			locked_ref = btrfs_select_ref_head(fs_info, delayed_refs);
			if (IS_ERR_OR_NULL(locked_ref)) {
				if (PTR_ERR(locked_ref) == -EAGAIN) {
					continue;
				} else {
					break;
				}
			}
			count++;
		}
		/*
		 * We need to try and merge add/drops of the same ref since we
		 * can run into issues with relocate dropping the implicit ref
		 * and then it being added back again before the drop can
		 * finish.  If we merged anything we need to re-loop so we can
		 * get a good ref.
		 * Or we can get node references of the same type that weren't
		 * merged when created due to bumps in the tree mod seq, and
		 * we need to merge them to prevent adding an inline extent
		 * backref before dropping it (triggering a BUG_ON at
		 * insert_inline_extent_backref()).
		 */
		spin_lock(&locked_ref->lock);
		btrfs_merge_delayed_refs(fs_info, delayed_refs, locked_ref);

		ret = btrfs_run_delayed_refs_for_head(trans, locked_ref, &bytes_processed);
		if (ret < 0 && ret != -EAGAIN) {
			/*
			 * Error, btrfs_run_delayed_refs_for_head already
			 * unlocked everything so just bail out
			 */
			return ret;
		} else if (!ret) {
			/*
			 * Success, perform the usual cleanup of a processed
			 * head
			 */
			ret = cleanup_ref_head(trans, locked_ref, &bytes_processed);
			if (ret > 0 ) {
				/* We dropped our lock, we need to loop. */
				ret = 0;
				continue;
			} else if (ret) {
				return ret;
			}
		}

		/*
		 * Either success case or btrfs_run_delayed_refs_for_head
		 * returned -EAGAIN, meaning we need to select another head
		 */

		locked_ref = NULL;
		cond_resched();
	} while ((min_bytes != U64_MAX && bytes_processed < min_bytes) ||
		 (max_count > 0 && count < max_count) ||
		 locked_ref);

	return 0;
}

#ifdef SCRAMBLE_DELAYED_REFS
/*
 * Normally delayed refs get processed in ascending bytenr order. This
 * correlates in most cases to the order added. To expose dependencies on this
 * order, we start to process the tree in the middle instead of the beginning
 */
static u64 find_middle(struct rb_root *root)
{
	struct rb_node *n = root->rb_node;
	struct btrfs_delayed_ref_node *entry;
	int alt = 1;
	u64 middle;
	u64 first = 0, last = 0;

	n = rb_first(root);
	if (n) {
		entry = rb_entry(n, struct btrfs_delayed_ref_node, rb_node);
		first = entry->bytenr;
	}
	n = rb_last(root);
	if (n) {
		entry = rb_entry(n, struct btrfs_delayed_ref_node, rb_node);
		last = entry->bytenr;
	}
	n = root->rb_node;

	while (n) {
		entry = rb_entry(n, struct btrfs_delayed_ref_node, rb_node);
		WARN_ON(!entry->in_tree);

		middle = entry->bytenr;

		if (alt)
			n = n->rb_left;
		else
			n = n->rb_right;

		alt = 1 - alt;
	}
	return middle;
}
#endif

/*
 * Start processing the delayed reference count updates and extent insertions
 * we have queued up so far.
 *
 * @trans:	Transaction handle.
 * @min_bytes:	How many bytes of delayed references to process. After this
 *		many bytes we stop processing delayed references if there are
 *		any more. If 0 it means to run all existing delayed references,
 *		but not new ones added after running all existing ones.
 *		Use (u64)-1 (U64_MAX) to run all existing delayed references
 *		plus any new ones that are added.
 *
 * Returns 0 on success or if called with an aborted transaction
 * Returns <0 on error and aborts the transaction
 */
int btrfs_run_delayed_refs(struct btrfs_trans_handle *trans, u64 min_bytes)
{
	struct btrfs_fs_info *fs_info = trans->fs_info;
	struct btrfs_delayed_ref_root *delayed_refs;
	int ret;

	/* We'll clean this up in btrfs_cleanup_transaction */
	if (TRANS_ABORTED(trans))
		return 0;

	if (test_bit(BTRFS_FS_CREATING_FREE_SPACE_TREE, &fs_info->flags))
		return 0;

	delayed_refs = &trans->transaction->delayed_refs;
again:
#ifdef SCRAMBLE_DELAYED_REFS
	delayed_refs->run_delayed_start = find_middle(&delayed_refs->root);
#endif
	ret = __btrfs_run_delayed_refs(trans, min_bytes);
	if (ret < 0) {
		btrfs_abort_transaction(trans, ret);
		return ret;
	}

	if (min_bytes == U64_MAX) {
		btrfs_create_pending_block_groups(trans);

		spin_lock(&delayed_refs->lock);
		if (xa_empty(&delayed_refs->head_refs)) {
			spin_unlock(&delayed_refs->lock);
			return 0;
		}
		spin_unlock(&delayed_refs->lock);

		cond_resched();
		goto again;
	}

	return 0;
}

int btrfs_set_disk_extent_flags(struct btrfs_trans_handle *trans,
				struct extent_buffer *eb, u64 flags)
{
	struct btrfs_delayed_extent_op *extent_op;
	int ret;

	extent_op = btrfs_alloc_delayed_extent_op();
	if (!extent_op)
		return -ENOMEM;

	extent_op->flags_to_set = flags;
	extent_op->update_flags = true;
	extent_op->update_key = false;

	ret = btrfs_add_delayed_extent_op(trans, eb->start, eb->len,
					  btrfs_header_level(eb), extent_op);
	if (ret)
		btrfs_free_delayed_extent_op(extent_op);
	return ret;
}

static noinline int check_delayed_ref(struct btrfs_inode *inode,
				      struct btrfs_path *path,
				      u64 offset, u64 bytenr)
{
	struct btrfs_root *root = inode->root;
	struct btrfs_delayed_ref_head *head;
	struct btrfs_delayed_ref_node *ref;
	struct btrfs_delayed_ref_root *delayed_refs;
	struct btrfs_transaction *cur_trans;
	struct rb_node *node;
	int ret = 0;

	spin_lock(&root->fs_info->trans_lock);
	cur_trans = root->fs_info->running_transaction;
	if (cur_trans)
		refcount_inc(&cur_trans->use_count);
	spin_unlock(&root->fs_info->trans_lock);
	if (!cur_trans)
		return 0;

	delayed_refs = &cur_trans->delayed_refs;
	spin_lock(&delayed_refs->lock);
	head = btrfs_find_delayed_ref_head(root->fs_info, delayed_refs, bytenr);
	if (!head) {
		spin_unlock(&delayed_refs->lock);
		btrfs_put_transaction(cur_trans);
		return 0;
	}

	if (!mutex_trylock(&head->mutex)) {
		if (path->nowait) {
			spin_unlock(&delayed_refs->lock);
			btrfs_put_transaction(cur_trans);
			return -EAGAIN;
		}

		refcount_inc(&head->refs);
		spin_unlock(&delayed_refs->lock);

		btrfs_release_path(path);

		/*
		 * Mutex was contended, block until it's released and let
		 * caller try again
		 */
		mutex_lock(&head->mutex);
		mutex_unlock(&head->mutex);
		btrfs_put_delayed_ref_head(head);
		btrfs_put_transaction(cur_trans);
		return -EAGAIN;
	}
	spin_unlock(&delayed_refs->lock);

	spin_lock(&head->lock);
	/*
	 * XXX: We should replace this with a proper search function in the
	 * future.
	 */
	for (node = rb_first_cached(&head->ref_tree); node;
	     node = rb_next(node)) {
		u64 ref_owner;
		u64 ref_offset;

		ref = rb_entry(node, struct btrfs_delayed_ref_node, ref_node);
		/* If it's a shared ref we know a cross reference exists */
		if (ref->type != BTRFS_EXTENT_DATA_REF_KEY) {
			ret = 1;
			break;
		}

		ref_owner = btrfs_delayed_ref_owner(ref);
		ref_offset = btrfs_delayed_ref_offset(ref);

		/*
		 * If our ref doesn't match the one we're currently looking at
		 * then we have a cross reference.
		 */
		if (ref->ref_root != btrfs_root_id(root) ||
		    ref_owner != btrfs_ino(inode) || ref_offset != offset) {
			ret = 1;
			break;
		}
	}
	spin_unlock(&head->lock);
	mutex_unlock(&head->mutex);
	btrfs_put_transaction(cur_trans);
	return ret;
}

/*
 * Check if there are references for a data extent other than the one belonging
 * to the given inode and offset.
 *
 * @inode:     The only inode we expect to find associated with the data extent.
 * @path:      A path to use for searching the extent tree.
 * @offset:    The only offset we expect to find associated with the data extent.
 * @bytenr:    The logical address of the data extent.
 *
 * When the extent does not have any other references other than the one we
 * expect to find, we always return a value of 0 with the path having a locked
 * leaf that contains the extent's extent item - this is necessary to ensure
 * we don't race with a task running delayed references, and our caller must
 * have such a path when calling check_delayed_ref() - it must lock a delayed
 * ref head while holding the leaf locked. In case the extent item is not found
 * in the extent tree, we return -ENOENT with the path having the leaf (locked)
 * where the extent item should be, in order to prevent races with another task
 * running delayed references, so that we don't miss any reference when calling
 * check_delayed_ref().
 *
 * Note: this may return false positives, and this is because we want to be
 *       quick here as we're called in write paths (when flushing delalloc and
 *       in the direct IO write path). For example we can have an extent with
 *       a single reference but that reference is not inlined, or we may have
 *       many references in the extent tree but we also have delayed references
 *       that cancel all the reference except the one for our inode and offset,
 *       but it would be expensive to do such checks and complex due to all
 *       locking to avoid races between the checks and flushing delayed refs,
 *       plus non-inline references may be located on leaves other than the one
 *       that contains the extent item in the extent tree. The important thing
 *       here is to not return false negatives and that the false positives are
 *       not very common.
 *
 * Returns: 0 if there are no cross references and with the path having a locked
 *          leaf from the extent tree that contains the extent's extent item.
 *
 *          1 if there are cross references (false positives can happen).
 *
 *          < 0 in case of an error. In case of -ENOENT the leaf in the extent
 *          tree where the extent item should be located at is read locked and
 *          accessible in the given path.
 */
static noinline int check_committed_ref(struct btrfs_inode *inode,
					struct btrfs_path *path,
					u64 offset, u64 bytenr)
{
	struct btrfs_root *root = inode->root;
	struct btrfs_fs_info *fs_info = root->fs_info;
	struct btrfs_root *extent_root = btrfs_extent_root(fs_info, bytenr);
	struct extent_buffer *leaf;
	struct btrfs_extent_data_ref *ref;
	struct btrfs_extent_inline_ref *iref;
	struct btrfs_extent_item *ei;
	struct btrfs_key key;
	u32 item_size;
	u32 expected_size;
	int type;
	int ret;

	key.objectid = bytenr;
	key.type = BTRFS_EXTENT_ITEM_KEY;
	key.offset = (u64)-1;

	ret = btrfs_search_slot(NULL, extent_root, &key, path, 0, 0);
	if (ret < 0)
		return ret;
	if (ret == 0) {
		/*
		 * Key with offset -1 found, there would have to exist an extent
		 * item with such offset, but this is out of the valid range.
		 */
		return -EUCLEAN;
	}

	if (path->slots[0] == 0)
		return -ENOENT;

	path->slots[0]--;
	leaf = path->nodes[0];
	btrfs_item_key_to_cpu(leaf, &key, path->slots[0]);

	if (key.objectid != bytenr || key.type != BTRFS_EXTENT_ITEM_KEY)
		return -ENOENT;

	item_size = btrfs_item_size(leaf, path->slots[0]);
	ei = btrfs_item_ptr(leaf, path->slots[0], struct btrfs_extent_item);
	expected_size = sizeof(*ei) + btrfs_extent_inline_ref_size(BTRFS_EXTENT_DATA_REF_KEY);

	/* No inline refs; we need to bail before checking for owner ref. */
	if (item_size == sizeof(*ei))
		return 1;

	/* Check for an owner ref; skip over it to the real inline refs. */
	iref = (struct btrfs_extent_inline_ref *)(ei + 1);
	type = btrfs_get_extent_inline_ref_type(leaf, iref, BTRFS_REF_TYPE_DATA);
	if (btrfs_fs_incompat(fs_info, SIMPLE_QUOTA) && type == BTRFS_EXTENT_OWNER_REF_KEY) {
		expected_size += btrfs_extent_inline_ref_size(BTRFS_EXTENT_OWNER_REF_KEY);
		iref = (struct btrfs_extent_inline_ref *)(iref + 1);
		type = btrfs_get_extent_inline_ref_type(leaf, iref, BTRFS_REF_TYPE_DATA);
	}

	/* If extent item has more than 1 inline ref then it's shared */
	if (item_size != expected_size)
		return 1;

	/* If this extent has SHARED_DATA_REF then it's shared */
	if (type != BTRFS_EXTENT_DATA_REF_KEY)
		return 1;

	ref = (struct btrfs_extent_data_ref *)(&iref->offset);
	if (btrfs_extent_refs(leaf, ei) !=
	    btrfs_extent_data_ref_count(leaf, ref) ||
	    btrfs_extent_data_ref_root(leaf, ref) != btrfs_root_id(root) ||
	    btrfs_extent_data_ref_objectid(leaf, ref) != btrfs_ino(inode) ||
	    btrfs_extent_data_ref_offset(leaf, ref) != offset)
		return 1;

	return 0;
}

int btrfs_cross_ref_exist(struct btrfs_inode *inode, u64 offset,
			  u64 bytenr, struct btrfs_path *path)
{
	int ret;

	do {
		ret = check_committed_ref(inode, path, offset, bytenr);
		if (ret && ret != -ENOENT)
			goto out;

		/*
		 * The path must have a locked leaf from the extent tree where
		 * the extent item for our extent is located, in case it exists,
		 * or where it should be located in case it doesn't exist yet
		 * because it's new and its delayed ref was not yet flushed.
		 * We need to lock the delayed ref head at check_delayed_ref(),
		 * if one exists, while holding the leaf locked in order to not
		 * race with delayed ref flushing, missing references and
		 * incorrectly reporting that the extent is not shared.
		 */
		if (IS_ENABLED(CONFIG_BTRFS_ASSERT)) {
			struct extent_buffer *leaf = path->nodes[0];

			ASSERT(leaf != NULL);
			btrfs_assert_tree_read_locked(leaf);

			if (ret != -ENOENT) {
				struct btrfs_key key;

				btrfs_item_key_to_cpu(leaf, &key, path->slots[0]);
				ASSERT(key.objectid == bytenr);
				ASSERT(key.type == BTRFS_EXTENT_ITEM_KEY);
			}
		}

		ret = check_delayed_ref(inode, path, offset, bytenr);
	} while (ret == -EAGAIN && !path->nowait);

out:
	btrfs_release_path(path);
	if (btrfs_is_data_reloc_root(inode->root))
		WARN_ON(ret > 0);
	return ret;
}

static int __btrfs_mod_ref(struct btrfs_trans_handle *trans,
			   struct btrfs_root *root,
			   struct extent_buffer *buf,
			   int full_backref, int inc)
{
	struct btrfs_fs_info *fs_info = root->fs_info;
	u64 parent;
	u64 ref_root;
	u32 nritems;
	struct btrfs_key key;
	struct btrfs_file_extent_item *fi;
	bool for_reloc = btrfs_header_flag(buf, BTRFS_HEADER_FLAG_RELOC);
	int i;
	int action;
	int level;
	int ret = 0;

	if (btrfs_is_testing(fs_info))
		return 0;

	ref_root = btrfs_header_owner(buf);
	nritems = btrfs_header_nritems(buf);
	level = btrfs_header_level(buf);

	if (!test_bit(BTRFS_ROOT_SHAREABLE, &root->state) && level == 0)
		return 0;

	if (full_backref)
		parent = buf->start;
	else
		parent = 0;
	if (inc)
		action = BTRFS_ADD_DELAYED_REF;
	else
		action = BTRFS_DROP_DELAYED_REF;

	for (i = 0; i < nritems; i++) {
		struct btrfs_ref ref = {
			.action = action,
			.parent = parent,
			.ref_root = ref_root,
		};

		if (level == 0) {
			btrfs_item_key_to_cpu(buf, &key, i);
			if (key.type != BTRFS_EXTENT_DATA_KEY)
				continue;
			fi = btrfs_item_ptr(buf, i,
					    struct btrfs_file_extent_item);
			if (btrfs_file_extent_type(buf, fi) ==
			    BTRFS_FILE_EXTENT_INLINE)
				continue;
			ref.bytenr = btrfs_file_extent_disk_bytenr(buf, fi);
			if (ref.bytenr == 0)
				continue;

			ref.num_bytes = btrfs_file_extent_disk_num_bytes(buf, fi);
			ref.owning_root = ref_root;

			key.offset -= btrfs_file_extent_offset(buf, fi);
			btrfs_init_data_ref(&ref, key.objectid, key.offset,
					    btrfs_root_id(root), for_reloc);
			if (inc)
				ret = btrfs_inc_extent_ref(trans, &ref);
			else
				ret = btrfs_free_extent(trans, &ref);
			if (ret)
				goto fail;
		} else {
			/* We don't know the owning_root, leave as 0. */
			ref.bytenr = btrfs_node_blockptr(buf, i);
			ref.num_bytes = fs_info->nodesize;

			btrfs_init_tree_ref(&ref, level - 1,
					    btrfs_root_id(root), for_reloc);
			if (inc)
				ret = btrfs_inc_extent_ref(trans, &ref);
			else
				ret = btrfs_free_extent(trans, &ref);
			if (ret)
				goto fail;
		}
	}
	return 0;
fail:
	return ret;
}

int btrfs_inc_ref(struct btrfs_trans_handle *trans, struct btrfs_root *root,
		  struct extent_buffer *buf, int full_backref)
{
	return __btrfs_mod_ref(trans, root, buf, full_backref, 1);
}

int btrfs_dec_ref(struct btrfs_trans_handle *trans, struct btrfs_root *root,
		  struct extent_buffer *buf, int full_backref)
{
	return __btrfs_mod_ref(trans, root, buf, full_backref, 0);
}

static u64 get_alloc_profile_by_root(struct btrfs_root *root, int data)
{
	struct btrfs_fs_info *fs_info = root->fs_info;
	u64 flags;
	u64 ret;

	if (data)
		flags = BTRFS_BLOCK_GROUP_DATA;
	else if (root == fs_info->chunk_root)
		flags = BTRFS_BLOCK_GROUP_SYSTEM;
	else
		flags = BTRFS_BLOCK_GROUP_METADATA;

	ret = btrfs_get_alloc_profile(fs_info, flags);
	return ret;
}

static u64 first_logical_byte(struct btrfs_fs_info *fs_info)
{
	struct rb_node *leftmost;
	u64 bytenr = 0;

	read_lock(&fs_info->block_group_cache_lock);
	/* Get the block group with the lowest logical start address. */
	leftmost = rb_first_cached(&fs_info->block_group_cache_tree);
	if (leftmost) {
		struct btrfs_block_group *bg;

		bg = rb_entry(leftmost, struct btrfs_block_group, cache_node);
		bytenr = bg->start;
	}
	read_unlock(&fs_info->block_group_cache_lock);

	return bytenr;
}

static int pin_down_extent(struct btrfs_trans_handle *trans,
			   struct btrfs_block_group *cache,
			   u64 bytenr, u64 num_bytes, int reserved)
{
	spin_lock(&cache->space_info->lock);
	spin_lock(&cache->lock);
	cache->pinned += num_bytes;
	btrfs_space_info_update_bytes_pinned(cache->space_info, num_bytes);
	if (reserved) {
		cache->reserved -= num_bytes;
		cache->space_info->bytes_reserved -= num_bytes;
	}
	spin_unlock(&cache->lock);
	spin_unlock(&cache->space_info->lock);

	btrfs_set_extent_bit(&trans->transaction->pinned_extents, bytenr,
			     bytenr + num_bytes - 1, EXTENT_DIRTY, NULL);
	return 0;
}

int btrfs_pin_extent(struct btrfs_trans_handle *trans,
		     u64 bytenr, u64 num_bytes, int reserved)
{
	struct btrfs_block_group *cache;

	cache = btrfs_lookup_block_group(trans->fs_info, bytenr);
	BUG_ON(!cache); /* Logic error */

	pin_down_extent(trans, cache, bytenr, num_bytes, reserved);

	btrfs_put_block_group(cache);
	return 0;
}

int btrfs_pin_extent_for_log_replay(struct btrfs_trans_handle *trans,
				    const struct extent_buffer *eb)
{
	struct btrfs_block_group *cache;
	int ret;

	cache = btrfs_lookup_block_group(trans->fs_info, eb->start);
	if (!cache)
		return -EINVAL;

	/*
	 * Fully cache the free space first so that our pin removes the free space
	 * from the cache.
	 */
	ret = btrfs_cache_block_group(cache, true);
	if (ret)
		goto out;

	pin_down_extent(trans, cache, eb->start, eb->len, 0);

	/* remove us from the free space cache (if we're there at all) */
	ret = btrfs_remove_free_space(cache, eb->start, eb->len);
out:
	btrfs_put_block_group(cache);
	return ret;
}

static int __exclude_logged_extent(struct btrfs_fs_info *fs_info,
				   u64 start, u64 num_bytes)
{
	int ret;
	struct btrfs_block_group *block_group;

	block_group = btrfs_lookup_block_group(fs_info, start);
	if (!block_group)
		return -EINVAL;

	ret = btrfs_cache_block_group(block_group, true);
	if (ret)
		goto out;

	ret = btrfs_remove_free_space(block_group, start, num_bytes);
out:
	btrfs_put_block_group(block_group);
	return ret;
}

int btrfs_exclude_logged_extents(struct extent_buffer *eb)
{
	struct btrfs_fs_info *fs_info = eb->fs_info;
	struct btrfs_file_extent_item *item;
	struct btrfs_key key;
	int found_type;
	int i;
	int ret = 0;

	if (!btrfs_fs_incompat(fs_info, MIXED_GROUPS))
		return 0;

	for (i = 0; i < btrfs_header_nritems(eb); i++) {
		btrfs_item_key_to_cpu(eb, &key, i);
		if (key.type != BTRFS_EXTENT_DATA_KEY)
			continue;
		item = btrfs_item_ptr(eb, i, struct btrfs_file_extent_item);
		found_type = btrfs_file_extent_type(eb, item);
		if (found_type == BTRFS_FILE_EXTENT_INLINE)
			continue;
		if (btrfs_file_extent_disk_bytenr(eb, item) == 0)
			continue;
		key.objectid = btrfs_file_extent_disk_bytenr(eb, item);
		key.offset = btrfs_file_extent_disk_num_bytes(eb, item);
		ret = __exclude_logged_extent(fs_info, key.objectid, key.offset);
		if (ret)
			break;
	}

	return ret;
}

static void
btrfs_inc_block_group_reservations(struct btrfs_block_group *bg)
{
	atomic_inc(&bg->reservations);
}

/*
 * Returns the free cluster for the given space info and sets empty_cluster to
 * what it should be based on the mount options.
 */
static struct btrfs_free_cluster *
fetch_cluster_info(struct btrfs_fs_info *fs_info,
		   struct btrfs_space_info *space_info, u64 *empty_cluster)
{
	struct btrfs_free_cluster *ret = NULL;

	*empty_cluster = 0;
	if (btrfs_mixed_space_info(space_info))
		return ret;

	if (space_info->flags & BTRFS_BLOCK_GROUP_METADATA) {
		ret = &fs_info->meta_alloc_cluster;
		if (btrfs_test_opt(fs_info, SSD))
			*empty_cluster = SZ_2M;
		else
			*empty_cluster = SZ_64K;
	} else if ((space_info->flags & BTRFS_BLOCK_GROUP_DATA) &&
		   btrfs_test_opt(fs_info, SSD_SPREAD)) {
		*empty_cluster = SZ_2M;
		ret = &fs_info->data_alloc_cluster;
	}

	return ret;
}

static int unpin_extent_range(struct btrfs_fs_info *fs_info,
			      u64 start, u64 end,
			      const bool return_free_space)
{
	struct btrfs_block_group *cache = NULL;
	struct btrfs_space_info *space_info;
	struct btrfs_free_cluster *cluster = NULL;
	u64 total_unpinned = 0;
	u64 empty_cluster = 0;
	bool readonly;
	int ret = 0;

	while (start <= end) {
		u64 len;

		readonly = false;
		if (!cache ||
		    start >= cache->start + cache->length) {
			if (cache)
				btrfs_put_block_group(cache);
			total_unpinned = 0;
			cache = btrfs_lookup_block_group(fs_info, start);
			if (cache == NULL) {
				/* Logic error, something removed the block group. */
				ret = -EUCLEAN;
				goto out;
			}

			cluster = fetch_cluster_info(fs_info,
						     cache->space_info,
						     &empty_cluster);
			empty_cluster <<= 1;
		}

		len = cache->start + cache->length - start;
		len = min(len, end + 1 - start);

		if (return_free_space)
			btrfs_add_free_space(cache, start, len);

		start += len;
		total_unpinned += len;
		space_info = cache->space_info;

		/*
		 * If this space cluster has been marked as fragmented and we've
		 * unpinned enough in this block group to potentially allow a
		 * cluster to be created inside of it go ahead and clear the
		 * fragmented check.
		 */
		if (cluster && cluster->fragmented &&
		    total_unpinned > empty_cluster) {
			spin_lock(&cluster->lock);
			cluster->fragmented = 0;
			spin_unlock(&cluster->lock);
		}

		spin_lock(&space_info->lock);
		spin_lock(&cache->lock);
		cache->pinned -= len;
		btrfs_space_info_update_bytes_pinned(space_info, -len);
		space_info->max_extent_size = 0;
		if (cache->ro) {
			space_info->bytes_readonly += len;
			readonly = true;
		} else if (btrfs_is_zoned(fs_info)) {
			/* Need reset before reusing in a zoned block group */
			btrfs_space_info_update_bytes_zone_unusable(space_info, len);
			readonly = true;
		}
		spin_unlock(&cache->lock);
		if (!readonly && return_free_space)
			btrfs_return_free_space(space_info, len);
		spin_unlock(&space_info->lock);
	}

	if (cache)
		btrfs_put_block_group(cache);
out:
	return ret;
}

int btrfs_finish_extent_commit(struct btrfs_trans_handle *trans)
{
	struct btrfs_fs_info *fs_info = trans->fs_info;
	struct btrfs_block_group *block_group, *tmp;
	struct list_head *deleted_bgs;
	struct extent_io_tree *unpin = &trans->transaction->pinned_extents;
	struct extent_state *cached_state = NULL;
	u64 start;
	u64 end;
	int unpin_error = 0;
	int ret;

	mutex_lock(&fs_info->unused_bg_unpin_mutex);
	btrfs_find_first_extent_bit(unpin, 0, &start, &end, EXTENT_DIRTY, &cached_state);

	while (!TRANS_ABORTED(trans) && cached_state) {
		struct extent_state *next_state;

		if (btrfs_test_opt(fs_info, DISCARD_SYNC))
			ret = btrfs_discard_extent(fs_info, start,
						   end + 1 - start, NULL);

		next_state = btrfs_next_extent_state(unpin, cached_state);
		btrfs_clear_extent_dirty(unpin, start, end, &cached_state);
		ret = unpin_extent_range(fs_info, start, end, true);
		/*
		 * If we get an error unpinning an extent range, store the first
		 * error to return later after trying to unpin all ranges and do
		 * the sync discards. Our caller will abort the transaction
		 * (which already wrote new superblocks) and on the next mount
		 * the space will be available as it was pinned by in-memory
		 * only structures in this phase.
		 */
		if (ret) {
			btrfs_err_rl(fs_info,
"failed to unpin extent range [%llu, %llu] when committing transaction %llu: %s (%d)",
				     start, end, trans->transid,
				     btrfs_decode_error(ret), ret);
			if (!unpin_error)
				unpin_error = ret;
		}

		btrfs_free_extent_state(cached_state);

		if (need_resched()) {
			btrfs_free_extent_state(next_state);
			mutex_unlock(&fs_info->unused_bg_unpin_mutex);
			cond_resched();
			cached_state = NULL;
			mutex_lock(&fs_info->unused_bg_unpin_mutex);
			btrfs_find_first_extent_bit(unpin, 0, &start, &end,
						    EXTENT_DIRTY, &cached_state);
		} else {
			cached_state = next_state;
			if (cached_state) {
				start = cached_state->start;
				end = cached_state->end;
			}
		}
	}
	mutex_unlock(&fs_info->unused_bg_unpin_mutex);
	btrfs_free_extent_state(cached_state);

	if (btrfs_test_opt(fs_info, DISCARD_ASYNC)) {
		btrfs_discard_calc_delay(&fs_info->discard_ctl);
		btrfs_discard_schedule_work(&fs_info->discard_ctl, true);
	}

	/*
	 * Transaction is finished.  We don't need the lock anymore.  We
	 * do need to clean up the block groups in case of a transaction
	 * abort.
	 */
	deleted_bgs = &trans->transaction->deleted_bgs;
	list_for_each_entry_safe(block_group, tmp, deleted_bgs, bg_list) {
		ret = -EROFS;
		if (!TRANS_ABORTED(trans))
			ret = btrfs_discard_extent(fs_info, block_group->start,
						   block_group->length, NULL);

		/*
		 * Not strictly necessary to lock, as the block_group should be
		 * read-only from btrfs_delete_unused_bgs().
		 */
		ASSERT(block_group->ro);
		spin_lock(&fs_info->unused_bgs_lock);
		list_del_init(&block_group->bg_list);
		spin_unlock(&fs_info->unused_bgs_lock);

		btrfs_unfreeze_block_group(block_group);
		btrfs_put_block_group(block_group);

		if (ret) {
			const char *errstr = btrfs_decode_error(ret);
			btrfs_warn(fs_info,
			   "discard failed while removing blockgroup: errno=%d %s",
				   ret, errstr);
		}
	}

	return unpin_error;
}

/*
 * Parse an extent item's inline extents looking for a simple quotas owner ref.
 *
 * @fs_info:	the btrfs_fs_info for this mount
 * @leaf:	a leaf in the extent tree containing the extent item
 * @slot:	the slot in the leaf where the extent item is found
 *
 * Returns the objectid of the root that originally allocated the extent item
 * if the inline owner ref is expected and present, otherwise 0.
 *
 * If an extent item has an owner ref item, it will be the first inline ref
 * item. Therefore the logic is to check whether there are any inline ref
 * items, then check the type of the first one.
 */
u64 btrfs_get_extent_owner_root(struct btrfs_fs_info *fs_info,
				struct extent_buffer *leaf, int slot)
{
	struct btrfs_extent_item *ei;
	struct btrfs_extent_inline_ref *iref;
	struct btrfs_extent_owner_ref *oref;
	unsigned long ptr;
	unsigned long end;
	int type;

	if (!btrfs_fs_incompat(fs_info, SIMPLE_QUOTA))
		return 0;

	ei = btrfs_item_ptr(leaf, slot, struct btrfs_extent_item);
	ptr = (unsigned long)(ei + 1);
	end = (unsigned long)ei + btrfs_item_size(leaf, slot);

	/* No inline ref items of any kind, can't check type. */
	if (ptr == end)
		return 0;

	iref = (struct btrfs_extent_inline_ref *)ptr;
	type = btrfs_get_extent_inline_ref_type(leaf, iref, BTRFS_REF_TYPE_ANY);

	/* We found an owner ref, get the root out of it. */
	if (type == BTRFS_EXTENT_OWNER_REF_KEY) {
		oref = (struct btrfs_extent_owner_ref *)(&iref->offset);
		return btrfs_extent_owner_ref_root_id(leaf, oref);
	}

	/* We have inline refs, but not an owner ref. */
	return 0;
}

static int do_free_extent_accounting(struct btrfs_trans_handle *trans,
				     u64 bytenr, struct btrfs_squota_delta *delta)
{
	int ret;
	u64 num_bytes = delta->num_bytes;

	if (delta->is_data) {
		struct btrfs_root *csum_root;

		csum_root = btrfs_csum_root(trans->fs_info, bytenr);
		ret = btrfs_del_csums(trans, csum_root, bytenr, num_bytes);
		if (ret) {
			btrfs_abort_transaction(trans, ret);
			return ret;
		}

		ret = btrfs_delete_raid_extent(trans, bytenr, num_bytes);
		if (ret) {
			btrfs_abort_transaction(trans, ret);
			return ret;
		}
	}

	ret = btrfs_record_squota_delta(trans->fs_info, delta);
	if (ret) {
		btrfs_abort_transaction(trans, ret);
		return ret;
	}

	ret = btrfs_add_to_free_space_tree(trans, bytenr, num_bytes);
	if (ret) {
		btrfs_abort_transaction(trans, ret);
		return ret;
	}

	ret = btrfs_update_block_group(trans, bytenr, num_bytes, false);
	if (ret)
		btrfs_abort_transaction(trans, ret);

	return ret;
}

#define abort_and_dump(trans, path, fmt, args...)	\
({							\
	btrfs_abort_transaction(trans, -EUCLEAN);	\
	btrfs_print_leaf(path->nodes[0]);		\
	btrfs_crit(trans->fs_info, fmt, ##args);	\
})

/*
 * Drop one or more refs of @node.
 *
 * 1. Locate the extent refs.
 *    It's either inline in EXTENT/METADATA_ITEM or in keyed SHARED_* item.
 *    Locate it, then reduce the refs number or remove the ref line completely.
 *
 * 2. Update the refs count in EXTENT/METADATA_ITEM
 *
 * Inline backref case:
 *
 * in extent tree we have:
 *
 * 	item 0 key (13631488 EXTENT_ITEM 1048576) itemoff 16201 itemsize 82
 *		refs 2 gen 6 flags DATA
 *		extent data backref root FS_TREE objectid 258 offset 0 count 1
 *		extent data backref root FS_TREE objectid 257 offset 0 count 1
 *
 * This function gets called with:
 *
 *    node->bytenr = 13631488
 *    node->num_bytes = 1048576
 *    root_objectid = FS_TREE
 *    owner_objectid = 257
 *    owner_offset = 0
 *    refs_to_drop = 1
 *
 * Then we should get some like:
 *
 * 	item 0 key (13631488 EXTENT_ITEM 1048576) itemoff 16201 itemsize 82
 *		refs 1 gen 6 flags DATA
 *		extent data backref root FS_TREE objectid 258 offset 0 count 1
 *
 * Keyed backref case:
 *
 * in extent tree we have:
 *
 *	item 0 key (13631488 EXTENT_ITEM 1048576) itemoff 3971 itemsize 24
 *		refs 754 gen 6 flags DATA
 *	[...]
 *	item 2 key (13631488 EXTENT_DATA_REF <HASH>) itemoff 3915 itemsize 28
 *		extent data backref root FS_TREE objectid 866 offset 0 count 1
 *
 * This function get called with:
 *
 *    node->bytenr = 13631488
 *    node->num_bytes = 1048576
 *    root_objectid = FS_TREE
 *    owner_objectid = 866
 *    owner_offset = 0
 *    refs_to_drop = 1
 *
 * Then we should get some like:
 *
 *	item 0 key (13631488 EXTENT_ITEM 1048576) itemoff 3971 itemsize 24
 *		refs 753 gen 6 flags DATA
 *
 * And that (13631488 EXTENT_DATA_REF <HASH>) gets removed.
 */
static int __btrfs_free_extent(struct btrfs_trans_handle *trans,
			       struct btrfs_delayed_ref_head *href,
			       const struct btrfs_delayed_ref_node *node,
			       struct btrfs_delayed_extent_op *extent_op)
{
	struct btrfs_fs_info *info = trans->fs_info;
	struct btrfs_key key;
	struct btrfs_path *path;
	struct btrfs_root *extent_root;
	struct extent_buffer *leaf;
	struct btrfs_extent_item *ei;
	struct btrfs_extent_inline_ref *iref;
	int ret;
	int is_data;
	int extent_slot = 0;
	int found_extent = 0;
	int num_to_del = 1;
	int refs_to_drop = node->ref_mod;
	u32 item_size;
	u64 refs;
	u64 bytenr = node->bytenr;
	u64 num_bytes = node->num_bytes;
	u64 owner_objectid = btrfs_delayed_ref_owner(node);
	u64 owner_offset = btrfs_delayed_ref_offset(node);
	bool skinny_metadata = btrfs_fs_incompat(info, SKINNY_METADATA);
	u64 delayed_ref_root = href->owning_root;

	extent_root = btrfs_extent_root(info, bytenr);
	ASSERT(extent_root);

	path = btrfs_alloc_path();
	if (!path)
		return -ENOMEM;

	is_data = owner_objectid >= BTRFS_FIRST_FREE_OBJECTID;

	if (!is_data && refs_to_drop != 1) {
		btrfs_crit(info,
"invalid refs_to_drop, dropping more than 1 refs for tree block %llu refs_to_drop %u",
			   node->bytenr, refs_to_drop);
		ret = -EINVAL;
		btrfs_abort_transaction(trans, ret);
		goto out;
	}

	if (is_data)
		skinny_metadata = false;

	ret = lookup_extent_backref(trans, path, &iref, bytenr, num_bytes,
				    node->parent, node->ref_root, owner_objectid,
				    owner_offset);
	if (ret == 0) {
		/*
		 * Either the inline backref or the SHARED_DATA_REF/
		 * SHARED_BLOCK_REF is found
		 *
		 * Here is a quick path to locate EXTENT/METADATA_ITEM.
		 * It's possible the EXTENT/METADATA_ITEM is near current slot.
		 */
		extent_slot = path->slots[0];
		while (extent_slot >= 0) {
			btrfs_item_key_to_cpu(path->nodes[0], &key,
					      extent_slot);
			if (key.objectid != bytenr)
				break;
			if (key.type == BTRFS_EXTENT_ITEM_KEY &&
			    key.offset == num_bytes) {
				found_extent = 1;
				break;
			}
			if (key.type == BTRFS_METADATA_ITEM_KEY &&
			    key.offset == owner_objectid) {
				found_extent = 1;
				break;
			}

			/* Quick path didn't find the EXTENT/METADATA_ITEM */
			if (path->slots[0] - extent_slot > 5)
				break;
			extent_slot--;
		}

		if (!found_extent) {
			if (iref) {
				abort_and_dump(trans, path,
"invalid iref slot %u, no EXTENT/METADATA_ITEM found but has inline extent ref",
					   path->slots[0]);
				ret = -EUCLEAN;
				goto out;
			}
			/* Must be SHARED_* item, remove the backref first */
			ret = remove_extent_backref(trans, extent_root, path,
						    NULL, refs_to_drop, is_data);
			if (ret) {
				btrfs_abort_transaction(trans, ret);
				goto out;
			}
			btrfs_release_path(path);

			/* Slow path to locate EXTENT/METADATA_ITEM */
			key.objectid = bytenr;
			key.type = BTRFS_EXTENT_ITEM_KEY;
			key.offset = num_bytes;

			if (!is_data && skinny_metadata) {
				key.type = BTRFS_METADATA_ITEM_KEY;
				key.offset = owner_objectid;
			}

			ret = btrfs_search_slot(trans, extent_root,
						&key, path, -1, 1);
			if (ret > 0 && skinny_metadata && path->slots[0]) {
				/*
				 * Couldn't find our skinny metadata item,
				 * see if we have ye olde extent item.
				 */
				path->slots[0]--;
				btrfs_item_key_to_cpu(path->nodes[0], &key,
						      path->slots[0]);
				if (key.objectid == bytenr &&
				    key.type == BTRFS_EXTENT_ITEM_KEY &&
				    key.offset == num_bytes)
					ret = 0;
			}

			if (ret > 0 && skinny_metadata) {
				skinny_metadata = false;
				key.objectid = bytenr;
				key.type = BTRFS_EXTENT_ITEM_KEY;
				key.offset = num_bytes;
				btrfs_release_path(path);
				ret = btrfs_search_slot(trans, extent_root,
							&key, path, -1, 1);
			}

			if (ret) {
				if (ret > 0)
					btrfs_print_leaf(path->nodes[0]);
				btrfs_err(info,
			"umm, got %d back from search, was looking for %llu, slot %d",
					  ret, bytenr, path->slots[0]);
			}
			if (ret < 0) {
				btrfs_abort_transaction(trans, ret);
				goto out;
			}
			extent_slot = path->slots[0];
		}
	} else if (WARN_ON(ret == -ENOENT)) {
		abort_and_dump(trans, path,
"unable to find ref byte nr %llu parent %llu root %llu owner %llu offset %llu slot %d",
			       bytenr, node->parent, node->ref_root, owner_objectid,
			       owner_offset, path->slots[0]);
		goto out;
	} else {
		btrfs_abort_transaction(trans, ret);
		goto out;
	}

	leaf = path->nodes[0];
	item_size = btrfs_item_size(leaf, extent_slot);
	if (unlikely(item_size < sizeof(*ei))) {
		ret = -EUCLEAN;
		btrfs_err(trans->fs_info,
			  "unexpected extent item size, has %u expect >= %zu",
			  item_size, sizeof(*ei));
		btrfs_abort_transaction(trans, ret);
		goto out;
	}
	ei = btrfs_item_ptr(leaf, extent_slot,
			    struct btrfs_extent_item);
	if (owner_objectid < BTRFS_FIRST_FREE_OBJECTID &&
	    key.type == BTRFS_EXTENT_ITEM_KEY) {
		struct btrfs_tree_block_info *bi;

		if (item_size < sizeof(*ei) + sizeof(*bi)) {
			abort_and_dump(trans, path,
"invalid extent item size for key (%llu, %u, %llu) slot %u owner %llu, has %u expect >= %zu",
				       key.objectid, key.type, key.offset,
				       path->slots[0], owner_objectid, item_size,
				       sizeof(*ei) + sizeof(*bi));
			ret = -EUCLEAN;
			goto out;
		}
		bi = (struct btrfs_tree_block_info *)(ei + 1);
		WARN_ON(owner_objectid != btrfs_tree_block_level(leaf, bi));
	}

	refs = btrfs_extent_refs(leaf, ei);
	if (refs < refs_to_drop) {
		abort_and_dump(trans, path,
		"trying to drop %d refs but we only have %llu for bytenr %llu slot %u",
			       refs_to_drop, refs, bytenr, path->slots[0]);
		ret = -EUCLEAN;
		goto out;
	}
	refs -= refs_to_drop;

	if (refs > 0) {
		if (extent_op)
			__run_delayed_extent_op(extent_op, leaf, ei);
		/*
		 * In the case of inline back ref, reference count will
		 * be updated by remove_extent_backref
		 */
		if (iref) {
			if (!found_extent) {
				abort_and_dump(trans, path,
"invalid iref, got inlined extent ref but no EXTENT/METADATA_ITEM found, slot %u",
					       path->slots[0]);
				ret = -EUCLEAN;
				goto out;
			}
		} else {
			btrfs_set_extent_refs(leaf, ei, refs);
		}
		if (found_extent) {
			ret = remove_extent_backref(trans, extent_root, path,
						    iref, refs_to_drop, is_data);
			if (ret) {
				btrfs_abort_transaction(trans, ret);
				goto out;
			}
		}
	} else {
		struct btrfs_squota_delta delta = {
			.root = delayed_ref_root,
			.num_bytes = num_bytes,
			.is_data = is_data,
			.is_inc = false,
			.generation = btrfs_extent_generation(leaf, ei),
		};

		/* In this branch refs == 1 */
		if (found_extent) {
			if (is_data && refs_to_drop !=
			    extent_data_ref_count(path, iref)) {
				abort_and_dump(trans, path,
		"invalid refs_to_drop, current refs %u refs_to_drop %u slot %u",
					       extent_data_ref_count(path, iref),
					       refs_to_drop, path->slots[0]);
				ret = -EUCLEAN;
				goto out;
			}
			if (iref) {
				if (path->slots[0] != extent_slot) {
					abort_and_dump(trans, path,
"invalid iref, extent item key (%llu %u %llu) slot %u doesn't have wanted iref",
						       key.objectid, key.type,
						       key.offset, path->slots[0]);
					ret = -EUCLEAN;
					goto out;
				}
			} else {
				/*
				 * No inline ref, we must be at SHARED_* item,
				 * And it's single ref, it must be:
				 * |	extent_slot	  ||extent_slot + 1|
				 * [ EXTENT/METADATA_ITEM ][ SHARED_* ITEM ]
				 */
				if (path->slots[0] != extent_slot + 1) {
					abort_and_dump(trans, path,
	"invalid SHARED_* item slot %u, previous item is not EXTENT/METADATA_ITEM",
						       path->slots[0]);
					ret = -EUCLEAN;
					goto out;
				}
				path->slots[0] = extent_slot;
				num_to_del = 2;
			}
		}
		/*
		 * We can't infer the data owner from the delayed ref, so we need
		 * to try to get it from the owning ref item.
		 *
		 * If it is not present, then that extent was not written under
		 * simple quotas mode, so we don't need to account for its deletion.
		 */
		if (is_data)
			delta.root = btrfs_get_extent_owner_root(trans->fs_info,
								 leaf, extent_slot);

		ret = btrfs_del_items(trans, extent_root, path, path->slots[0],
				      num_to_del);
		if (ret) {
			btrfs_abort_transaction(trans, ret);
			goto out;
		}
		btrfs_release_path(path);

		ret = do_free_extent_accounting(trans, bytenr, &delta);
	}
	btrfs_release_path(path);

out:
	btrfs_free_path(path);
	return ret;
}

/*
 * when we free an block, it is possible (and likely) that we free the last
 * delayed ref for that extent as well.  This searches the delayed ref tree for
 * a given extent, and if there are no other delayed refs to be processed, it
 * removes it from the tree.
 */
static noinline int check_ref_cleanup(struct btrfs_trans_handle *trans,
				      u64 bytenr)
{
	struct btrfs_fs_info *fs_info = trans->fs_info;
	struct btrfs_delayed_ref_head *head;
	struct btrfs_delayed_ref_root *delayed_refs;
	int ret = 0;

	delayed_refs = &trans->transaction->delayed_refs;
	spin_lock(&delayed_refs->lock);
	head = btrfs_find_delayed_ref_head(fs_info, delayed_refs, bytenr);
	if (!head)
		goto out_delayed_unlock;

	spin_lock(&head->lock);
	if (!RB_EMPTY_ROOT(&head->ref_tree.rb_root))
		goto out;

	if (cleanup_extent_op(head) != NULL)
		goto out;

	/*
	 * waiting for the lock here would deadlock.  If someone else has it
	 * locked they are already in the process of dropping it anyway
	 */
	if (!mutex_trylock(&head->mutex))
		goto out;

	btrfs_delete_ref_head(fs_info, delayed_refs, head);
	head->processing = false;

	spin_unlock(&head->lock);
	spin_unlock(&delayed_refs->lock);

	BUG_ON(head->extent_op);
	if (head->must_insert_reserved)
		ret = 1;

	btrfs_cleanup_ref_head_accounting(fs_info, delayed_refs, head);
	mutex_unlock(&head->mutex);
	btrfs_put_delayed_ref_head(head);
	return ret;
out:
	spin_unlock(&head->lock);

out_delayed_unlock:
	spin_unlock(&delayed_refs->lock);
	return 0;
}

int btrfs_free_tree_block(struct btrfs_trans_handle *trans,
			  u64 root_id,
			  struct extent_buffer *buf,
			  u64 parent, int last_ref)
{
	struct btrfs_fs_info *fs_info = trans->fs_info;
	struct btrfs_block_group *bg;
	int ret;

	if (root_id != BTRFS_TREE_LOG_OBJECTID) {
		struct btrfs_ref generic_ref = {
			.action = BTRFS_DROP_DELAYED_REF,
			.bytenr = buf->start,
			.num_bytes = buf->len,
			.parent = parent,
			.owning_root = btrfs_header_owner(buf),
			.ref_root = root_id,
		};

		/*
		 * Assert that the extent buffer is not cleared due to
		 * EXTENT_BUFFER_ZONED_ZEROOUT. Please refer
		 * btrfs_clear_buffer_dirty() and btree_csum_one_bio() for
		 * detail.
		 */
		ASSERT(btrfs_header_bytenr(buf) != 0);

		btrfs_init_tree_ref(&generic_ref, btrfs_header_level(buf), 0, false);
		btrfs_ref_tree_mod(fs_info, &generic_ref);
		ret = btrfs_add_delayed_tree_ref(trans, &generic_ref, NULL);
		if (ret < 0)
			return ret;
	}

	if (!last_ref)
		return 0;

	if (btrfs_header_generation(buf) != trans->transid)
		goto out;

	if (root_id != BTRFS_TREE_LOG_OBJECTID) {
		ret = check_ref_cleanup(trans, buf->start);
		if (!ret)
			goto out;
	}

	bg = btrfs_lookup_block_group(fs_info, buf->start);

	if (btrfs_header_flag(buf, BTRFS_HEADER_FLAG_WRITTEN)) {
		pin_down_extent(trans, bg, buf->start, buf->len, 1);
		btrfs_put_block_group(bg);
		goto out;
	}

	/*
	 * If there are tree mod log users we may have recorded mod log
	 * operations for this node.  If we re-allocate this node we
	 * could replay operations on this node that happened when it
	 * existed in a completely different root.  For example if it
	 * was part of root A, then was reallocated to root B, and we
	 * are doing a btrfs_old_search_slot(root b), we could replay
	 * operations that happened when the block was part of root A,
	 * giving us an inconsistent view of the btree.
	 *
	 * We are safe from races here because at this point no other
	 * node or root points to this extent buffer, so if after this
	 * check a new tree mod log user joins we will not have an
	 * existing log of operations on this node that we have to
	 * contend with.
	 */

	if (test_bit(BTRFS_FS_TREE_MOD_LOG_USERS, &fs_info->flags)
		     || btrfs_is_zoned(fs_info)) {
		pin_down_extent(trans, bg, buf->start, buf->len, 1);
		btrfs_put_block_group(bg);
		goto out;
	}

	WARN_ON(test_bit(EXTENT_BUFFER_DIRTY, &buf->bflags));

	btrfs_add_free_space(bg, buf->start, buf->len);
	btrfs_free_reserved_bytes(bg, buf->len, false);
	btrfs_put_block_group(bg);
	trace_btrfs_reserved_extent_free(fs_info, buf->start, buf->len);

out:
	return 0;
}

/* Can return -ENOMEM */
int btrfs_free_extent(struct btrfs_trans_handle *trans, struct btrfs_ref *ref)
{
	struct btrfs_fs_info *fs_info = trans->fs_info;
	int ret;

	if (btrfs_is_testing(fs_info))
		return 0;

	/*
	 * tree log blocks never actually go into the extent allocation
	 * tree, just update pinning info and exit early.
	 */
	if (ref->ref_root == BTRFS_TREE_LOG_OBJECTID) {
		btrfs_pin_extent(trans, ref->bytenr, ref->num_bytes, 1);
		ret = 0;
	} else if (ref->type == BTRFS_REF_METADATA) {
		ret = btrfs_add_delayed_tree_ref(trans, ref, NULL);
	} else {
		ret = btrfs_add_delayed_data_ref(trans, ref, 0);
	}

	if (ref->ref_root != BTRFS_TREE_LOG_OBJECTID)
		btrfs_ref_tree_mod(fs_info, ref);

	return ret;
}

enum btrfs_loop_type {
	/*
	 * Start caching block groups but do not wait for progress or for them
	 * to be done.
	 */
	LOOP_CACHING_NOWAIT,

	/*
	 * Wait for the block group free_space >= the space we're waiting for if
	 * the block group isn't cached.
	 */
	LOOP_CACHING_WAIT,

	/*
	 * Allow allocations to happen from block groups that do not yet have a
	 * size classification.
	 */
	LOOP_UNSET_SIZE_CLASS,

	/*
	 * Allocate a chunk and then retry the allocation.
	 */
	LOOP_ALLOC_CHUNK,

	/*
	 * Ignore the size class restrictions for this allocation.
	 */
	LOOP_WRONG_SIZE_CLASS,

	/*
	 * Ignore the empty size, only try to allocate the number of bytes
	 * needed for this allocation.
	 */
	LOOP_NO_EMPTY_SIZE,
};

static inline void
btrfs_lock_block_group(struct btrfs_block_group *cache,
		       int delalloc)
{
	if (delalloc)
		down_read(&cache->data_rwsem);
}

static inline void btrfs_grab_block_group(struct btrfs_block_group *cache,
		       int delalloc)
{
	btrfs_get_block_group(cache);
	if (delalloc)
		down_read(&cache->data_rwsem);
}

static struct btrfs_block_group *btrfs_lock_cluster(
		   struct btrfs_block_group *block_group,
		   struct btrfs_free_cluster *cluster,
		   int delalloc)
	__acquires(&cluster->refill_lock)
{
	struct btrfs_block_group *used_bg = NULL;

	spin_lock(&cluster->refill_lock);
	while (1) {
		used_bg = cluster->block_group;
		if (!used_bg)
			return NULL;

		if (used_bg == block_group)
			return used_bg;

		btrfs_get_block_group(used_bg);

		if (!delalloc)
			return used_bg;

		if (down_read_trylock(&used_bg->data_rwsem))
			return used_bg;

		spin_unlock(&cluster->refill_lock);

		/* We should only have one-level nested. */
		down_read_nested(&used_bg->data_rwsem, SINGLE_DEPTH_NESTING);

		spin_lock(&cluster->refill_lock);
		if (used_bg == cluster->block_group)
			return used_bg;

		up_read(&used_bg->data_rwsem);
		btrfs_put_block_group(used_bg);
	}
}

static inline void
btrfs_release_block_group(struct btrfs_block_group *cache,
			 int delalloc)
{
	if (delalloc)
		up_read(&cache->data_rwsem);
	btrfs_put_block_group(cache);
}

static bool find_free_extent_check_size_class(const struct find_free_extent_ctl *ffe_ctl,
					      const struct btrfs_block_group *bg)
{
	if (ffe_ctl->policy == BTRFS_EXTENT_ALLOC_ZONED)
		return true;
	if (!btrfs_block_group_should_use_size_class(bg))
		return true;
	if (ffe_ctl->loop >= LOOP_WRONG_SIZE_CLASS)
		return true;
	if (ffe_ctl->loop >= LOOP_UNSET_SIZE_CLASS &&
	    bg->size_class == BTRFS_BG_SZ_NONE)
		return true;
	return ffe_ctl->size_class == bg->size_class;
}

/*
 * Helper function for find_free_extent().
 *
 * Return -ENOENT to inform caller that we need fallback to unclustered mode.
 * Return >0 to inform caller that we find nothing
 * Return 0 means we have found a location and set ffe_ctl->found_offset.
 */
static int find_free_extent_clustered(struct btrfs_block_group *bg,
				      struct find_free_extent_ctl *ffe_ctl,
				      struct btrfs_block_group **cluster_bg_ret)
{
	struct btrfs_block_group *cluster_bg;
	struct btrfs_free_cluster *last_ptr = ffe_ctl->last_ptr;
	u64 aligned_cluster;
	u64 offset;
	int ret;

	cluster_bg = btrfs_lock_cluster(bg, last_ptr, ffe_ctl->delalloc);
	if (!cluster_bg)
		goto refill_cluster;
	if (cluster_bg != bg && (cluster_bg->ro ||
	    !block_group_bits(cluster_bg, ffe_ctl->flags) ||
	    !find_free_extent_check_size_class(ffe_ctl, cluster_bg)))
		goto release_cluster;

	offset = btrfs_alloc_from_cluster(cluster_bg, last_ptr,
			ffe_ctl->num_bytes, cluster_bg->start,
			&ffe_ctl->max_extent_size);
	if (offset) {
		/* We have a block, we're done */
		spin_unlock(&last_ptr->refill_lock);
		trace_btrfs_reserve_extent_cluster(cluster_bg, ffe_ctl);
		*cluster_bg_ret = cluster_bg;
		ffe_ctl->found_offset = offset;
		return 0;
	}
	WARN_ON(last_ptr->block_group != cluster_bg);

release_cluster:
	/*
	 * If we are on LOOP_NO_EMPTY_SIZE, we can't set up a new clusters, so
	 * lets just skip it and let the allocator find whatever block it can
	 * find. If we reach this point, we will have tried the cluster
	 * allocator plenty of times and not have found anything, so we are
	 * likely way too fragmented for the clustering stuff to find anything.
	 *
	 * However, if the cluster is taken from the current block group,
	 * release the cluster first, so that we stand a better chance of
	 * succeeding in the unclustered allocation.
	 */
	if (ffe_ctl->loop >= LOOP_NO_EMPTY_SIZE && cluster_bg != bg) {
		spin_unlock(&last_ptr->refill_lock);
		btrfs_release_block_group(cluster_bg, ffe_ctl->delalloc);
		return -ENOENT;
	}

	/* This cluster didn't work out, free it and start over */
	btrfs_return_cluster_to_free_space(NULL, last_ptr);

	if (cluster_bg != bg)
		btrfs_release_block_group(cluster_bg, ffe_ctl->delalloc);

refill_cluster:
	if (ffe_ctl->loop >= LOOP_NO_EMPTY_SIZE) {
		spin_unlock(&last_ptr->refill_lock);
		return -ENOENT;
	}

	aligned_cluster = max_t(u64,
			ffe_ctl->empty_cluster + ffe_ctl->empty_size,
			bg->full_stripe_len);
	ret = btrfs_find_space_cluster(bg, last_ptr, ffe_ctl->search_start,
			ffe_ctl->num_bytes, aligned_cluster);
	if (ret == 0) {
		/* Now pull our allocation out of this cluster */
		offset = btrfs_alloc_from_cluster(bg, last_ptr,
				ffe_ctl->num_bytes, ffe_ctl->search_start,
				&ffe_ctl->max_extent_size);
		if (offset) {
			/* We found one, proceed */
			spin_unlock(&last_ptr->refill_lock);
			ffe_ctl->found_offset = offset;
			trace_btrfs_reserve_extent_cluster(bg, ffe_ctl);
			return 0;
		}
	}
	/*
	 * At this point we either didn't find a cluster or we weren't able to
	 * allocate a block from our cluster.  Free the cluster we've been
	 * trying to use, and go to the next block group.
	 */
	btrfs_return_cluster_to_free_space(NULL, last_ptr);
	spin_unlock(&last_ptr->refill_lock);
	return 1;
}

/*
 * Return >0 to inform caller that we find nothing
 * Return 0 when we found an free extent and set ffe_ctrl->found_offset
 */
static int find_free_extent_unclustered(struct btrfs_block_group *bg,
					struct find_free_extent_ctl *ffe_ctl)
{
	struct btrfs_free_cluster *last_ptr = ffe_ctl->last_ptr;
	u64 offset;

	/*
	 * We are doing an unclustered allocation, set the fragmented flag so
	 * we don't bother trying to setup a cluster again until we get more
	 * space.
	 */
	if (unlikely(last_ptr)) {
		spin_lock(&last_ptr->lock);
		last_ptr->fragmented = 1;
		spin_unlock(&last_ptr->lock);
	}
	if (ffe_ctl->cached) {
		struct btrfs_free_space_ctl *free_space_ctl;

		free_space_ctl = bg->free_space_ctl;
		spin_lock(&free_space_ctl->tree_lock);
		if (free_space_ctl->free_space <
		    ffe_ctl->num_bytes + ffe_ctl->empty_cluster +
		    ffe_ctl->empty_size) {
			ffe_ctl->total_free_space = max_t(u64,
					ffe_ctl->total_free_space,
					free_space_ctl->free_space);
			spin_unlock(&free_space_ctl->tree_lock);
			return 1;
		}
		spin_unlock(&free_space_ctl->tree_lock);
	}

	offset = btrfs_find_space_for_alloc(bg, ffe_ctl->search_start,
			ffe_ctl->num_bytes, ffe_ctl->empty_size,
			&ffe_ctl->max_extent_size);
	if (!offset)
		return 1;
	ffe_ctl->found_offset = offset;
	return 0;
}

static int do_allocation_clustered(struct btrfs_block_group *block_group,
				   struct find_free_extent_ctl *ffe_ctl,
				   struct btrfs_block_group **bg_ret)
{
	int ret;

	/* We want to try and use the cluster allocator, so lets look there */
	if (ffe_ctl->last_ptr && ffe_ctl->use_cluster) {
		ret = find_free_extent_clustered(block_group, ffe_ctl, bg_ret);
		if (ret >= 0)
			return ret;
		/* ret == -ENOENT case falls through */
	}

	return find_free_extent_unclustered(block_group, ffe_ctl);
}

/*
 * Tree-log block group locking
 * ============================
 *
 * fs_info::treelog_bg_lock protects the fs_info::treelog_bg which
 * indicates the starting address of a block group, which is reserved only
 * for tree-log metadata.
 *
 * Lock nesting
 * ============
 *
 * space_info::lock
 *   block_group::lock
 *     fs_info::treelog_bg_lock
 */

/*
 * Simple allocator for sequential-only block group. It only allows sequential
 * allocation. No need to play with trees. This function also reserves the
 * bytes as in btrfs_add_reserved_bytes.
 */
static int do_allocation_zoned(struct btrfs_block_group *block_group,
			       struct find_free_extent_ctl *ffe_ctl,
			       struct btrfs_block_group **bg_ret)
{
	struct btrfs_fs_info *fs_info = block_group->fs_info;
	struct btrfs_space_info *space_info = block_group->space_info;
	struct btrfs_free_space_ctl *ctl = block_group->free_space_ctl;
	u64 start = block_group->start;
	u64 num_bytes = ffe_ctl->num_bytes;
	u64 avail;
	u64 bytenr = block_group->start;
	u64 log_bytenr;
	u64 data_reloc_bytenr;
	int ret = 0;
	bool skip = false;

	ASSERT(btrfs_is_zoned(block_group->fs_info));

	/*
	 * Do not allow non-tree-log blocks in the dedicated tree-log block
	 * group, and vice versa.
	 */
	spin_lock(&fs_info->treelog_bg_lock);
	log_bytenr = fs_info->treelog_bg;
	if (log_bytenr && ((ffe_ctl->for_treelog && bytenr != log_bytenr) ||
			   (!ffe_ctl->for_treelog && bytenr == log_bytenr)))
		skip = true;
	spin_unlock(&fs_info->treelog_bg_lock);
	if (skip)
		return 1;

	/*
	 * Do not allow non-relocation blocks in the dedicated relocation block
	 * group, and vice versa.
	 */
	spin_lock(&fs_info->relocation_bg_lock);
	data_reloc_bytenr = fs_info->data_reloc_bg;
	if (data_reloc_bytenr &&
	    ((ffe_ctl->for_data_reloc && bytenr != data_reloc_bytenr) ||
	     (!ffe_ctl->for_data_reloc && bytenr == data_reloc_bytenr)))
		skip = true;
	spin_unlock(&fs_info->relocation_bg_lock);
	if (skip)
		return 1;

	/* Check RO and no space case before trying to activate it */
	spin_lock(&block_group->lock);
	if (block_group->ro || btrfs_zoned_bg_is_full(block_group)) {
		ret = 1;
		/*
		 * May need to clear fs_info->{treelog,data_reloc}_bg.
		 * Return the error after taking the locks.
		 */
	}
	spin_unlock(&block_group->lock);

	/* Metadata block group is activated at write time. */
	if (!ret && (block_group->flags & BTRFS_BLOCK_GROUP_DATA) &&
	    !btrfs_zone_activate(block_group)) {
		ret = 1;
		/*
		 * May need to clear fs_info->{treelog,data_reloc}_bg.
		 * Return the error after taking the locks.
		 */
	}

	spin_lock(&space_info->lock);
	spin_lock(&block_group->lock);
	spin_lock(&fs_info->treelog_bg_lock);
	spin_lock(&fs_info->relocation_bg_lock);

	if (ret)
		goto out;

	ASSERT(!ffe_ctl->for_treelog ||
	       block_group->start == fs_info->treelog_bg ||
	       fs_info->treelog_bg == 0);
	ASSERT(!ffe_ctl->for_data_reloc ||
	       block_group->start == fs_info->data_reloc_bg ||
	       fs_info->data_reloc_bg == 0);

	if (block_group->ro ||
	    (!ffe_ctl->for_data_reloc &&
	     test_bit(BLOCK_GROUP_FLAG_ZONED_DATA_RELOC, &block_group->runtime_flags))) {
		ret = 1;
		goto out;
	}

	/*
	 * Do not allow currently using block group to be tree-log dedicated
	 * block group.
	 */
	if (ffe_ctl->for_treelog && !fs_info->treelog_bg &&
	    (block_group->used || block_group->reserved)) {
		ret = 1;
		goto out;
	}

	/*
	 * Do not allow currently used block group to be the data relocation
	 * dedicated block group.
	 */
	if (ffe_ctl->for_data_reloc && !fs_info->data_reloc_bg &&
	    (block_group->used || block_group->reserved)) {
		ret = 1;
		goto out;
	}

	WARN_ON_ONCE(block_group->alloc_offset > block_group->zone_capacity);
	avail = block_group->zone_capacity - block_group->alloc_offset;
	if (avail < num_bytes) {
		if (ffe_ctl->max_extent_size < avail) {
			/*
			 * With sequential allocator, free space is always
			 * contiguous
			 */
			ffe_ctl->max_extent_size = avail;
			ffe_ctl->total_free_space = avail;
		}
		ret = 1;
		goto out;
	}

	if (ffe_ctl->for_treelog && !fs_info->treelog_bg)
		fs_info->treelog_bg = block_group->start;

	if (ffe_ctl->for_data_reloc) {
		if (!fs_info->data_reloc_bg)
			fs_info->data_reloc_bg = block_group->start;
		/*
		 * Do not allow allocations from this block group, unless it is
		 * for data relocation. Compared to increasing the ->ro, setting
		 * the ->zoned_data_reloc_ongoing flag still allows nocow
		 * writers to come in. See btrfs_inc_nocow_writers().
		 *
		 * We need to disable an allocation to avoid an allocation of
		 * regular (non-relocation data) extent. With mix of relocation
		 * extents and regular extents, we can dispatch WRITE commands
		 * (for relocation extents) and ZONE APPEND commands (for
		 * regular extents) at the same time to the same zone, which
		 * easily break the write pointer.
		 *
		 * Also, this flag avoids this block group to be zone finished.
		 */
		set_bit(BLOCK_GROUP_FLAG_ZONED_DATA_RELOC, &block_group->runtime_flags);
	}

	ffe_ctl->found_offset = start + block_group->alloc_offset;
	block_group->alloc_offset += num_bytes;
	spin_lock(&ctl->tree_lock);
	ctl->free_space -= num_bytes;
	spin_unlock(&ctl->tree_lock);

	/*
	 * We do not check if found_offset is aligned to stripesize. The
	 * address is anyway rewritten when using zone append writing.
	 */

	ffe_ctl->search_start = ffe_ctl->found_offset;

out:
	if (ret && ffe_ctl->for_treelog)
		fs_info->treelog_bg = 0;
	if (ret && ffe_ctl->for_data_reloc)
		fs_info->data_reloc_bg = 0;
	spin_unlock(&fs_info->relocation_bg_lock);
	spin_unlock(&fs_info->treelog_bg_lock);
	spin_unlock(&block_group->lock);
	spin_unlock(&space_info->lock);
	return ret;
}

static int do_allocation(struct btrfs_block_group *block_group,
			 struct find_free_extent_ctl *ffe_ctl,
			 struct btrfs_block_group **bg_ret)
{
	switch (ffe_ctl->policy) {
	case BTRFS_EXTENT_ALLOC_CLUSTERED:
		return do_allocation_clustered(block_group, ffe_ctl, bg_ret);
	case BTRFS_EXTENT_ALLOC_ZONED:
		return do_allocation_zoned(block_group, ffe_ctl, bg_ret);
	default:
		BUG();
	}
}

static void release_block_group(struct btrfs_block_group *block_group,
				struct find_free_extent_ctl *ffe_ctl,
				int delalloc)
{
	switch (ffe_ctl->policy) {
	case BTRFS_EXTENT_ALLOC_CLUSTERED:
		ffe_ctl->retry_uncached = false;
		break;
	case BTRFS_EXTENT_ALLOC_ZONED:
		/* Nothing to do */
		break;
	default:
		BUG();
	}

	BUG_ON(btrfs_bg_flags_to_raid_index(block_group->flags) !=
	       ffe_ctl->index);
	btrfs_release_block_group(block_group, delalloc);
}

static void found_extent_clustered(struct find_free_extent_ctl *ffe_ctl,
				   struct btrfs_key *ins)
{
	struct btrfs_free_cluster *last_ptr = ffe_ctl->last_ptr;

	if (!ffe_ctl->use_cluster && last_ptr) {
		spin_lock(&last_ptr->lock);
		last_ptr->window_start = ins->objectid;
		spin_unlock(&last_ptr->lock);
	}
}

static void found_extent(struct find_free_extent_ctl *ffe_ctl,
			 struct btrfs_key *ins)
{
	switch (ffe_ctl->policy) {
	case BTRFS_EXTENT_ALLOC_CLUSTERED:
		found_extent_clustered(ffe_ctl, ins);
		break;
	case BTRFS_EXTENT_ALLOC_ZONED:
		/* Nothing to do */
		break;
	default:
		BUG();
	}
}

static int can_allocate_chunk_zoned(struct btrfs_fs_info *fs_info,
				    struct find_free_extent_ctl *ffe_ctl)
{
	/* Block group's activeness is not a requirement for METADATA block groups. */
	if (!(ffe_ctl->flags & BTRFS_BLOCK_GROUP_DATA))
		return 0;

	/* If we can activate new zone, just allocate a chunk and use it */
	if (btrfs_can_activate_zone(fs_info->fs_devices, ffe_ctl->flags))
		return 0;

	/*
	 * We already reached the max active zones. Try to finish one block
	 * group to make a room for a new block group. This is only possible
	 * for a data block group because btrfs_zone_finish() may need to wait
	 * for a running transaction which can cause a deadlock for metadata
	 * allocation.
	 */
	if (ffe_ctl->flags & BTRFS_BLOCK_GROUP_DATA) {
		int ret = btrfs_zone_finish_one_bg(fs_info);

		if (ret == 1)
			return 0;
		else if (ret < 0)
			return ret;
	}

	/*
	 * If we have enough free space left in an already active block group
	 * and we can't activate any other zone now, do not allow allocating a
	 * new chunk and let find_free_extent() retry with a smaller size.
	 */
	if (ffe_ctl->max_extent_size >= ffe_ctl->min_alloc_size)
		return -ENOSPC;

	/*
	 * Even min_alloc_size is not left in any block groups. Since we cannot
	 * activate a new block group, allocating it may not help. Let's tell a
	 * caller to try again and hope it progress something by writing some
	 * parts of the region. That is only possible for data block groups,
	 * where a part of the region can be written.
	 */
	if (ffe_ctl->flags & BTRFS_BLOCK_GROUP_DATA)
		return -EAGAIN;

	/*
	 * We cannot activate a new block group and no enough space left in any
	 * block groups. So, allocating a new block group may not help. But,
	 * there is nothing to do anyway, so let's go with it.
	 */
	return 0;
}

static int can_allocate_chunk(struct btrfs_fs_info *fs_info,
			      struct find_free_extent_ctl *ffe_ctl)
{
	switch (ffe_ctl->policy) {
	case BTRFS_EXTENT_ALLOC_CLUSTERED:
		return 0;
	case BTRFS_EXTENT_ALLOC_ZONED:
		return can_allocate_chunk_zoned(fs_info, ffe_ctl);
	default:
		BUG();
	}
}

/*
 * Return >0 means caller needs to re-search for free extent
 * Return 0 means we have the needed free extent.
 * Return <0 means we failed to locate any free extent.
 */
static int find_free_extent_update_loop(struct btrfs_fs_info *fs_info,
					struct btrfs_key *ins,
					struct find_free_extent_ctl *ffe_ctl,
					struct btrfs_space_info *space_info,
					bool full_search)
{
	struct btrfs_root *root = fs_info->chunk_root;
	int ret;

	if ((ffe_ctl->loop == LOOP_CACHING_NOWAIT) &&
	    ffe_ctl->have_caching_bg && !ffe_ctl->orig_have_caching_bg)
		ffe_ctl->orig_have_caching_bg = true;

	if (ins->objectid) {
		found_extent(ffe_ctl, ins);
		return 0;
	}

	if (ffe_ctl->loop >= LOOP_CACHING_WAIT && ffe_ctl->have_caching_bg)
		return 1;

	ffe_ctl->index++;
	if (ffe_ctl->index < BTRFS_NR_RAID_TYPES)
		return 1;

	/* See the comments for btrfs_loop_type for an explanation of the phases. */
	if (ffe_ctl->loop < LOOP_NO_EMPTY_SIZE) {
		ffe_ctl->index = 0;
		/*
		 * We want to skip the LOOP_CACHING_WAIT step if we don't have
		 * any uncached bgs and we've already done a full search
		 * through.
		 */
		if (ffe_ctl->loop == LOOP_CACHING_NOWAIT &&
		    (!ffe_ctl->orig_have_caching_bg && full_search))
			ffe_ctl->loop++;
		ffe_ctl->loop++;

		if (ffe_ctl->loop == LOOP_ALLOC_CHUNK) {
			struct btrfs_trans_handle *trans;
			int exist = 0;

			/* Check if allocation policy allows to create a new chunk */
			ret = can_allocate_chunk(fs_info, ffe_ctl);
			if (ret)
				return ret;

			trans = current->journal_info;
			if (trans)
				exist = 1;
			else
				trans = btrfs_join_transaction(root);

			if (IS_ERR(trans)) {
				ret = PTR_ERR(trans);
				return ret;
			}

			ret = btrfs_chunk_alloc(trans, space_info, ffe_ctl->flags,
						CHUNK_ALLOC_FORCE_FOR_EXTENT);

			/* Do not bail out on ENOSPC since we can do more. */
			if (ret == -ENOSPC) {
				ret = 0;
				ffe_ctl->loop++;
			}
			else if (ret < 0)
				btrfs_abort_transaction(trans, ret);
			else
				ret = 0;
			if (!exist)
				btrfs_end_transaction(trans);
			if (ret)
				return ret;
		}

		if (ffe_ctl->loop == LOOP_NO_EMPTY_SIZE) {
			if (ffe_ctl->policy != BTRFS_EXTENT_ALLOC_CLUSTERED)
				return -ENOSPC;

			/*
			 * Don't loop again if we already have no empty_size and
			 * no empty_cluster.
			 */
			if (ffe_ctl->empty_size == 0 &&
			    ffe_ctl->empty_cluster == 0)
				return -ENOSPC;
			ffe_ctl->empty_size = 0;
			ffe_ctl->empty_cluster = 0;
		}
		return 1;
	}
	return -ENOSPC;
}

static int prepare_allocation_clustered(struct btrfs_fs_info *fs_info,
					struct find_free_extent_ctl *ffe_ctl,
					struct btrfs_space_info *space_info,
					struct btrfs_key *ins)
{
	/*
	 * If our free space is heavily fragmented we may not be able to make
	 * big contiguous allocations, so instead of doing the expensive search
	 * for free space, simply return ENOSPC with our max_extent_size so we
	 * can go ahead and search for a more manageable chunk.
	 *
	 * If our max_extent_size is large enough for our allocation simply
	 * disable clustering since we will likely not be able to find enough
	 * space to create a cluster and induce latency trying.
	 */
	if (space_info->max_extent_size) {
		spin_lock(&space_info->lock);
		if (space_info->max_extent_size &&
		    ffe_ctl->num_bytes > space_info->max_extent_size) {
			ins->offset = space_info->max_extent_size;
			spin_unlock(&space_info->lock);
			return -ENOSPC;
		} else if (space_info->max_extent_size) {
			ffe_ctl->use_cluster = false;
		}
		spin_unlock(&space_info->lock);
	}

	ffe_ctl->last_ptr = fetch_cluster_info(fs_info, space_info,
					       &ffe_ctl->empty_cluster);
	if (ffe_ctl->last_ptr) {
		struct btrfs_free_cluster *last_ptr = ffe_ctl->last_ptr;

		spin_lock(&last_ptr->lock);
		if (last_ptr->block_group)
			ffe_ctl->hint_byte = last_ptr->window_start;
		if (last_ptr->fragmented) {
			/*
			 * We still set window_start so we can keep track of the
			 * last place we found an allocation to try and save
			 * some time.
			 */
			ffe_ctl->hint_byte = last_ptr->window_start;
			ffe_ctl->use_cluster = false;
		}
		spin_unlock(&last_ptr->lock);
	}

	return 0;
}

static int prepare_allocation_zoned(struct btrfs_fs_info *fs_info,
				    struct find_free_extent_ctl *ffe_ctl)
{
	if (ffe_ctl->for_treelog) {
		spin_lock(&fs_info->treelog_bg_lock);
		if (fs_info->treelog_bg)
			ffe_ctl->hint_byte = fs_info->treelog_bg;
		spin_unlock(&fs_info->treelog_bg_lock);
	} else if (ffe_ctl->for_data_reloc) {
		spin_lock(&fs_info->relocation_bg_lock);
		if (fs_info->data_reloc_bg)
			ffe_ctl->hint_byte = fs_info->data_reloc_bg;
		spin_unlock(&fs_info->relocation_bg_lock);
	} else if (ffe_ctl->flags & BTRFS_BLOCK_GROUP_DATA) {
		struct btrfs_block_group *block_group;

		spin_lock(&fs_info->zone_active_bgs_lock);
		list_for_each_entry(block_group, &fs_info->zone_active_bgs, active_bg_list) {
			/*
			 * No lock is OK here because avail is monotinically
			 * decreasing, and this is just a hint.
			 */
			u64 avail = block_group->zone_capacity - block_group->alloc_offset;

			if (block_group_bits(block_group, ffe_ctl->flags) &&
			    avail >= ffe_ctl->num_bytes) {
				ffe_ctl->hint_byte = block_group->start;
				break;
			}
		}
		spin_unlock(&fs_info->zone_active_bgs_lock);
	}

	return 0;
}

static int prepare_allocation(struct btrfs_fs_info *fs_info,
			      struct find_free_extent_ctl *ffe_ctl,
			      struct btrfs_space_info *space_info,
			      struct btrfs_key *ins)
{
	switch (ffe_ctl->policy) {
	case BTRFS_EXTENT_ALLOC_CLUSTERED:
		return prepare_allocation_clustered(fs_info, ffe_ctl,
						    space_info, ins);
	case BTRFS_EXTENT_ALLOC_ZONED:
		return prepare_allocation_zoned(fs_info, ffe_ctl);
	default:
		BUG();
	}
}

/*
 * walks the btree of allocated extents and find a hole of a given size.
 * The key ins is changed to record the hole:
 * ins->objectid == start position
 * ins->flags = BTRFS_EXTENT_ITEM_KEY
 * ins->offset == the size of the hole.
 * Any available blocks before search_start are skipped.
 *
 * If there is no suitable free space, we will record the max size of
 * the free space extent currently.
 *
 * The overall logic and call chain:
 *
 * find_free_extent()
 * |- Iterate through all block groups
 * |  |- Get a valid block group
 * |  |- Try to do clustered allocation in that block group
 * |  |- Try to do unclustered allocation in that block group
 * |  |- Check if the result is valid
 * |  |  |- If valid, then exit
 * |  |- Jump to next block group
 * |
 * |- Push harder to find free extents
 *    |- If not found, re-iterate all block groups
 */
static noinline int find_free_extent(struct btrfs_root *root,
				     struct btrfs_key *ins,
				     struct find_free_extent_ctl *ffe_ctl)
{
	struct btrfs_fs_info *fs_info = root->fs_info;
	int ret = 0;
	int cache_block_group_error = 0;
	struct btrfs_block_group *block_group = NULL;
	struct btrfs_space_info *space_info;
	bool full_search = false;

	WARN_ON(ffe_ctl->num_bytes < fs_info->sectorsize);

	ffe_ctl->search_start = 0;
	/* For clustered allocation */
	ffe_ctl->empty_cluster = 0;
	ffe_ctl->last_ptr = NULL;
	ffe_ctl->use_cluster = true;
	ffe_ctl->have_caching_bg = false;
	ffe_ctl->orig_have_caching_bg = false;
	ffe_ctl->index = btrfs_bg_flags_to_raid_index(ffe_ctl->flags);
	ffe_ctl->loop = 0;
	ffe_ctl->retry_uncached = false;
	ffe_ctl->cached = 0;
	ffe_ctl->max_extent_size = 0;
	ffe_ctl->total_free_space = 0;
	ffe_ctl->found_offset = 0;
	ffe_ctl->policy = BTRFS_EXTENT_ALLOC_CLUSTERED;
	ffe_ctl->size_class = btrfs_calc_block_group_size_class(ffe_ctl->num_bytes);

	if (btrfs_is_zoned(fs_info))
		ffe_ctl->policy = BTRFS_EXTENT_ALLOC_ZONED;

	ins->type = BTRFS_EXTENT_ITEM_KEY;
	ins->objectid = 0;
	ins->offset = 0;

	trace_btrfs_find_free_extent(root, ffe_ctl);

	space_info = btrfs_find_space_info(fs_info, ffe_ctl->flags);
	if (btrfs_is_zoned(fs_info) && space_info) {
		/* Use dedicated sub-space_info for dedicated block group users. */
		if (ffe_ctl->for_data_reloc) {
			space_info = space_info->sub_group[0];
			ASSERT(space_info->subgroup_id == BTRFS_SUB_GROUP_DATA_RELOC);
		} else if (ffe_ctl->for_treelog) {
			space_info = space_info->sub_group[0];
			ASSERT(space_info->subgroup_id == BTRFS_SUB_GROUP_TREELOG);
		}
	}
	if (!space_info) {
		btrfs_err(fs_info, "no space info for %llu, tree-log %d, relocation %d",
			  ffe_ctl->flags, ffe_ctl->for_treelog, ffe_ctl->for_data_reloc);
		return -ENOSPC;
	}

	ret = prepare_allocation(fs_info, ffe_ctl, space_info, ins);
	if (ret < 0)
		return ret;

	ffe_ctl->search_start = max(ffe_ctl->search_start,
				    first_logical_byte(fs_info));
	ffe_ctl->search_start = max(ffe_ctl->search_start, ffe_ctl->hint_byte);
	if (ffe_ctl->search_start == ffe_ctl->hint_byte) {
		block_group = btrfs_lookup_block_group(fs_info,
						       ffe_ctl->search_start);
		/*
		 * we don't want to use the block group if it doesn't match our
		 * allocation bits, or if its not cached.
		 *
		 * However if we are re-searching with an ideal block group
		 * picked out then we don't care that the block group is cached.
		 */
		if (block_group && block_group_bits(block_group, ffe_ctl->flags) &&
		    block_group->space_info == space_info &&
		    block_group->cached != BTRFS_CACHE_NO) {
			down_read(&space_info->groups_sem);
			if (list_empty(&block_group->list) ||
			    block_group->ro) {
				/*
				 * someone is removing this block group,
				 * we can't jump into the have_block_group
				 * target because our list pointers are not
				 * valid
				 */
				btrfs_put_block_group(block_group);
				up_read(&space_info->groups_sem);
			} else {
				ffe_ctl->index = btrfs_bg_flags_to_raid_index(
							block_group->flags);
				btrfs_lock_block_group(block_group,
						       ffe_ctl->delalloc);
				ffe_ctl->hinted = true;
				goto have_block_group;
			}
		} else if (block_group) {
			btrfs_put_block_group(block_group);
		}
	}
search:
	trace_btrfs_find_free_extent_search_loop(root, ffe_ctl);
	ffe_ctl->have_caching_bg = false;
	if (ffe_ctl->index == btrfs_bg_flags_to_raid_index(ffe_ctl->flags) ||
	    ffe_ctl->index == 0)
		full_search = true;
	down_read(&space_info->groups_sem);
	list_for_each_entry(block_group,
			    &space_info->block_groups[ffe_ctl->index], list) {
		struct btrfs_block_group *bg_ret;

		ffe_ctl->hinted = false;
		/* If the block group is read-only, we can skip it entirely. */
		if (unlikely(block_group->ro)) {
			if (ffe_ctl->for_treelog)
				btrfs_clear_treelog_bg(block_group);
			if (ffe_ctl->for_data_reloc)
				btrfs_clear_data_reloc_bg(block_group);
			continue;
		}

		btrfs_grab_block_group(block_group, ffe_ctl->delalloc);
		ffe_ctl->search_start = block_group->start;

		/*
		 * this can happen if we end up cycling through all the
		 * raid types, but we want to make sure we only allocate
		 * for the proper type.
		 */
		if (!block_group_bits(block_group, ffe_ctl->flags)) {
			u64 extra = BTRFS_BLOCK_GROUP_DUP |
				BTRFS_BLOCK_GROUP_RAID1_MASK |
				BTRFS_BLOCK_GROUP_RAID56_MASK |
				BTRFS_BLOCK_GROUP_RAID10;

			/*
			 * if they asked for extra copies and this block group
			 * doesn't provide them, bail.  This does allow us to
			 * fill raid0 from raid1.
			 */
			if ((ffe_ctl->flags & extra) && !(block_group->flags & extra))
				goto loop;

			/*
			 * This block group has different flags than we want.
			 * It's possible that we have MIXED_GROUP flag but no
			 * block group is mixed.  Just skip such block group.
			 */
			btrfs_release_block_group(block_group, ffe_ctl->delalloc);
			continue;
		}

have_block_group:
		trace_btrfs_find_free_extent_have_block_group(root, ffe_ctl, block_group);
		ffe_ctl->cached = btrfs_block_group_done(block_group);
		if (unlikely(!ffe_ctl->cached)) {
			ffe_ctl->have_caching_bg = true;
			ret = btrfs_cache_block_group(block_group, false);

			/*
			 * If we get ENOMEM here or something else we want to
			 * try other block groups, because it may not be fatal.
			 * However if we can't find anything else we need to
			 * save our return here so that we return the actual
			 * error that caused problems, not ENOSPC.
			 */
			if (ret < 0) {
				if (!cache_block_group_error)
					cache_block_group_error = ret;
				ret = 0;
				goto loop;
			}
			ret = 0;
		}

		if (unlikely(block_group->cached == BTRFS_CACHE_ERROR)) {
			if (!cache_block_group_error)
				cache_block_group_error = -EIO;
			goto loop;
		}

		if (!find_free_extent_check_size_class(ffe_ctl, block_group))
			goto loop;

		bg_ret = NULL;
		ret = do_allocation(block_group, ffe_ctl, &bg_ret);
		if (ret > 0)
			goto loop;

		if (bg_ret && bg_ret != block_group) {
			btrfs_release_block_group(block_group, ffe_ctl->delalloc);
			block_group = bg_ret;
		}

		/* Checks */
		ffe_ctl->search_start = round_up(ffe_ctl->found_offset,
						 fs_info->stripesize);

		/* move on to the next group */
		if (ffe_ctl->search_start + ffe_ctl->num_bytes >
		    block_group->start + block_group->length) {
			btrfs_add_free_space_unused(block_group,
					    ffe_ctl->found_offset,
					    ffe_ctl->num_bytes);
			goto loop;
		}

		if (ffe_ctl->found_offset < ffe_ctl->search_start)
			btrfs_add_free_space_unused(block_group,
					ffe_ctl->found_offset,
					ffe_ctl->search_start - ffe_ctl->found_offset);

		ret = btrfs_add_reserved_bytes(block_group, ffe_ctl->ram_bytes,
					       ffe_ctl->num_bytes,
					       ffe_ctl->delalloc,
					       ffe_ctl->loop >= LOOP_WRONG_SIZE_CLASS);
		if (ret == -EAGAIN) {
			btrfs_add_free_space_unused(block_group,
					ffe_ctl->found_offset,
					ffe_ctl->num_bytes);
			goto loop;
		}
		btrfs_inc_block_group_reservations(block_group);

		/* we are all good, lets return */
		ins->objectid = ffe_ctl->search_start;
		ins->offset = ffe_ctl->num_bytes;

		trace_btrfs_reserve_extent(block_group, ffe_ctl);
		btrfs_release_block_group(block_group, ffe_ctl->delalloc);
		break;
loop:
		if (!ffe_ctl->cached && ffe_ctl->loop > LOOP_CACHING_NOWAIT &&
		    !ffe_ctl->retry_uncached) {
			ffe_ctl->retry_uncached = true;
			btrfs_wait_block_group_cache_progress(block_group,
						ffe_ctl->num_bytes +
						ffe_ctl->empty_cluster +
						ffe_ctl->empty_size);
			goto have_block_group;
		}
		release_block_group(block_group, ffe_ctl, ffe_ctl->delalloc);
		cond_resched();
	}
	up_read(&space_info->groups_sem);

	ret = find_free_extent_update_loop(fs_info, ins, ffe_ctl, space_info,
					   full_search);
	if (ret > 0)
		goto search;

	if (ret == -ENOSPC && !cache_block_group_error) {
		/*
		 * Use ffe_ctl->total_free_space as fallback if we can't find
		 * any contiguous hole.
		 */
		if (!ffe_ctl->max_extent_size)
			ffe_ctl->max_extent_size = ffe_ctl->total_free_space;
		spin_lock(&space_info->lock);
		space_info->max_extent_size = ffe_ctl->max_extent_size;
		spin_unlock(&space_info->lock);
		ins->offset = ffe_ctl->max_extent_size;
	} else if (ret == -ENOSPC) {
		ret = cache_block_group_error;
	}
	return ret;
}

/*
 * Entry point to the extent allocator. Tries to find a hole that is at least
 * as big as @num_bytes.
 *
 * @root           -	The root that will contain this extent
 *
 * @ram_bytes      -	The amount of space in ram that @num_bytes take. This
 *			is used for accounting purposes. This value differs
 *			from @num_bytes only in the case of compressed extents.
 *
 * @num_bytes      -	Number of bytes to allocate on-disk.
 *
 * @min_alloc_size -	Indicates the minimum amount of space that the
 *			allocator should try to satisfy. In some cases
 *			@num_bytes may be larger than what is required and if
 *			the filesystem is fragmented then allocation fails.
 *			However, the presence of @min_alloc_size gives a
 *			chance to try and satisfy the smaller allocation.
 *
 * @empty_size     -	A hint that you plan on doing more COW. This is the
 *			size in bytes the allocator should try to find free
 *			next to the block it returns.  This is just a hint and
 *			may be ignored by the allocator.
 *
 * @hint_byte      -	Hint to the allocator to start searching above the byte
 *			address passed. It might be ignored.
 *
 * @ins            -	This key is modified to record the found hole. It will
 *			have the following values:
 *			ins->objectid == start position
 *			ins->flags = BTRFS_EXTENT_ITEM_KEY
 *			ins->offset == the size of the hole.
 *
 * @is_data        -	Boolean flag indicating whether an extent is
 *			allocated for data (true) or metadata (false)
 *
 * @delalloc       -	Boolean flag indicating whether this allocation is for
 *			delalloc or not. If 'true' data_rwsem of block groups
 *			is going to be acquired.
 *
 *
 * Returns 0 when an allocation succeeded or < 0 when an error occurred. In
 * case -ENOSPC is returned then @ins->offset will contain the size of the
 * largest available hole the allocator managed to find.
 */
int btrfs_reserve_extent(struct btrfs_root *root, u64 ram_bytes,
			 u64 num_bytes, u64 min_alloc_size,
			 u64 empty_size, u64 hint_byte,
			 struct btrfs_key *ins, int is_data, int delalloc)
{
	struct btrfs_fs_info *fs_info = root->fs_info;
	struct find_free_extent_ctl ffe_ctl = {};
	bool final_tried = num_bytes == min_alloc_size;
	u64 flags;
	int ret;
	bool for_treelog = (btrfs_root_id(root) == BTRFS_TREE_LOG_OBJECTID);
	bool for_data_reloc = (btrfs_is_data_reloc_root(root) && is_data);

	flags = get_alloc_profile_by_root(root, is_data);
again:
	WARN_ON(num_bytes < fs_info->sectorsize);

	ffe_ctl.ram_bytes = ram_bytes;
	ffe_ctl.num_bytes = num_bytes;
	ffe_ctl.min_alloc_size = min_alloc_size;
	ffe_ctl.empty_size = empty_size;
	ffe_ctl.flags = flags;
	ffe_ctl.delalloc = delalloc;
	ffe_ctl.hint_byte = hint_byte;
	ffe_ctl.for_treelog = for_treelog;
	ffe_ctl.for_data_reloc = for_data_reloc;

	ret = find_free_extent(root, ins, &ffe_ctl);
	if (!ret && !is_data) {
		btrfs_dec_block_group_reservations(fs_info, ins->objectid);
	} else if (ret == -ENOSPC) {
		if (!final_tried && ins->offset) {
			num_bytes = min(num_bytes >> 1, ins->offset);
			num_bytes = round_down(num_bytes,
					       fs_info->sectorsize);
			num_bytes = max(num_bytes, min_alloc_size);
			ram_bytes = num_bytes;
			if (num_bytes == min_alloc_size)
				final_tried = true;
			goto again;
		} else if (btrfs_test_opt(fs_info, ENOSPC_DEBUG)) {
			struct btrfs_space_info *sinfo;

			sinfo = btrfs_find_space_info(fs_info, flags);
			btrfs_err(fs_info,
	"allocation failed flags %llu, wanted %llu tree-log %d, relocation: %d",
				  flags, num_bytes, for_treelog, for_data_reloc);
			if (sinfo)
				btrfs_dump_space_info(fs_info, sinfo,
						      num_bytes, 1);
		}
	}

	return ret;
}

int btrfs_free_reserved_extent(struct btrfs_fs_info *fs_info, u64 start, u64 len,
			       bool is_delalloc)
{
	struct btrfs_block_group *cache;

	cache = btrfs_lookup_block_group(fs_info, start);
	if (!cache) {
		btrfs_err(fs_info, "Unable to find block group for %llu",
			  start);
		return -ENOSPC;
	}

	btrfs_add_free_space(cache, start, len);
	btrfs_free_reserved_bytes(cache, len, is_delalloc);
	trace_btrfs_reserved_extent_free(fs_info, start, len);

	btrfs_put_block_group(cache);
	return 0;
}

int btrfs_pin_reserved_extent(struct btrfs_trans_handle *trans,
			      const struct extent_buffer *eb)
{
	struct btrfs_block_group *cache;
	int ret = 0;

	cache = btrfs_lookup_block_group(trans->fs_info, eb->start);
	if (!cache) {
		btrfs_err(trans->fs_info, "unable to find block group for %llu",
			  eb->start);
		return -ENOSPC;
	}

	ret = pin_down_extent(trans, cache, eb->start, eb->len, 1);
	btrfs_put_block_group(cache);
	return ret;
}

static int alloc_reserved_extent(struct btrfs_trans_handle *trans, u64 bytenr,
				 u64 num_bytes)
{
	struct btrfs_fs_info *fs_info = trans->fs_info;
	int ret;

	ret = btrfs_remove_from_free_space_tree(trans, bytenr, num_bytes);
	if (ret)
		return ret;

	ret = btrfs_update_block_group(trans, bytenr, num_bytes, true);
	if (ret) {
		ASSERT(!ret);
		btrfs_err(fs_info, "update block group failed for %llu %llu",
			  bytenr, num_bytes);
		return ret;
	}

	trace_btrfs_reserved_extent_alloc(fs_info, bytenr, num_bytes);
	return 0;
}

static int alloc_reserved_file_extent(struct btrfs_trans_handle *trans,
				      u64 parent, u64 root_objectid,
				      u64 flags, u64 owner, u64 offset,
				      struct btrfs_key *ins, int ref_mod, u64 oref_root)
{
	struct btrfs_fs_info *fs_info = trans->fs_info;
	struct btrfs_root *extent_root;
	int ret;
	struct btrfs_extent_item *extent_item;
	struct btrfs_extent_owner_ref *oref;
	struct btrfs_extent_inline_ref *iref;
	struct btrfs_path *path;
	struct extent_buffer *leaf;
	int type;
	u32 size;
	const bool simple_quota = (btrfs_qgroup_mode(fs_info) == BTRFS_QGROUP_MODE_SIMPLE);

	if (parent > 0)
		type = BTRFS_SHARED_DATA_REF_KEY;
	else
		type = BTRFS_EXTENT_DATA_REF_KEY;

	size = sizeof(*extent_item);
	if (simple_quota)
		size += btrfs_extent_inline_ref_size(BTRFS_EXTENT_OWNER_REF_KEY);
	size += btrfs_extent_inline_ref_size(type);

	path = btrfs_alloc_path();
	if (!path)
		return -ENOMEM;

	extent_root = btrfs_extent_root(fs_info, ins->objectid);
	ret = btrfs_insert_empty_item(trans, extent_root, path, ins, size);
	if (ret) {
		btrfs_free_path(path);
		return ret;
	}

	leaf = path->nodes[0];
	extent_item = btrfs_item_ptr(leaf, path->slots[0],
				     struct btrfs_extent_item);
	btrfs_set_extent_refs(leaf, extent_item, ref_mod);
	btrfs_set_extent_generation(leaf, extent_item, trans->transid);
	btrfs_set_extent_flags(leaf, extent_item,
			       flags | BTRFS_EXTENT_FLAG_DATA);

	iref = (struct btrfs_extent_inline_ref *)(extent_item + 1);
	if (simple_quota) {
		btrfs_set_extent_inline_ref_type(leaf, iref, BTRFS_EXTENT_OWNER_REF_KEY);
		oref = (struct btrfs_extent_owner_ref *)(&iref->offset);
		btrfs_set_extent_owner_ref_root_id(leaf, oref, oref_root);
		iref = (struct btrfs_extent_inline_ref *)(oref + 1);
	}
	btrfs_set_extent_inline_ref_type(leaf, iref, type);

	if (parent > 0) {
		struct btrfs_shared_data_ref *ref;
		ref = (struct btrfs_shared_data_ref *)(iref + 1);
		btrfs_set_extent_inline_ref_offset(leaf, iref, parent);
		btrfs_set_shared_data_ref_count(leaf, ref, ref_mod);
	} else {
		struct btrfs_extent_data_ref *ref;
		ref = (struct btrfs_extent_data_ref *)(&iref->offset);
		btrfs_set_extent_data_ref_root(leaf, ref, root_objectid);
		btrfs_set_extent_data_ref_objectid(leaf, ref, owner);
		btrfs_set_extent_data_ref_offset(leaf, ref, offset);
		btrfs_set_extent_data_ref_count(leaf, ref, ref_mod);
	}

	btrfs_free_path(path);

	return alloc_reserved_extent(trans, ins->objectid, ins->offset);
}

static int alloc_reserved_tree_block(struct btrfs_trans_handle *trans,
				     const struct btrfs_delayed_ref_node *node,
				     struct btrfs_delayed_extent_op *extent_op)
{
	struct btrfs_fs_info *fs_info = trans->fs_info;
	struct btrfs_root *extent_root;
	int ret;
	struct btrfs_extent_item *extent_item;
	struct btrfs_key extent_key;
	struct btrfs_tree_block_info *block_info;
	struct btrfs_extent_inline_ref *iref;
	struct btrfs_path *path;
	struct extent_buffer *leaf;
	u32 size = sizeof(*extent_item) + sizeof(*iref);
	const u64 flags = (extent_op ? extent_op->flags_to_set : 0);
	/* The owner of a tree block is the level. */
	int level = btrfs_delayed_ref_owner(node);
	bool skinny_metadata = btrfs_fs_incompat(fs_info, SKINNY_METADATA);

	extent_key.objectid = node->bytenr;
	if (skinny_metadata) {
		/* The owner of a tree block is the level. */
		extent_key.offset = level;
		extent_key.type = BTRFS_METADATA_ITEM_KEY;
	} else {
		extent_key.offset = node->num_bytes;
		extent_key.type = BTRFS_EXTENT_ITEM_KEY;
		size += sizeof(*block_info);
	}

	path = btrfs_alloc_path();
	if (!path)
		return -ENOMEM;

	extent_root = btrfs_extent_root(fs_info, extent_key.objectid);
	ret = btrfs_insert_empty_item(trans, extent_root, path, &extent_key,
				      size);
	if (ret) {
		btrfs_free_path(path);
		return ret;
	}

	leaf = path->nodes[0];
	extent_item = btrfs_item_ptr(leaf, path->slots[0],
				     struct btrfs_extent_item);
	btrfs_set_extent_refs(leaf, extent_item, 1);
	btrfs_set_extent_generation(leaf, extent_item, trans->transid);
	btrfs_set_extent_flags(leaf, extent_item,
			       flags | BTRFS_EXTENT_FLAG_TREE_BLOCK);

	if (skinny_metadata) {
		iref = (struct btrfs_extent_inline_ref *)(extent_item + 1);
	} else {
		block_info = (struct btrfs_tree_block_info *)(extent_item + 1);
		btrfs_set_tree_block_key(leaf, block_info, &extent_op->key);
		btrfs_set_tree_block_level(leaf, block_info, level);
		iref = (struct btrfs_extent_inline_ref *)(block_info + 1);
	}

	if (node->type == BTRFS_SHARED_BLOCK_REF_KEY) {
		btrfs_set_extent_inline_ref_type(leaf, iref,
						 BTRFS_SHARED_BLOCK_REF_KEY);
		btrfs_set_extent_inline_ref_offset(leaf, iref, node->parent);
	} else {
		btrfs_set_extent_inline_ref_type(leaf, iref,
						 BTRFS_TREE_BLOCK_REF_KEY);
		btrfs_set_extent_inline_ref_offset(leaf, iref, node->ref_root);
	}

	btrfs_free_path(path);

	return alloc_reserved_extent(trans, node->bytenr, fs_info->nodesize);
}

int btrfs_alloc_reserved_file_extent(struct btrfs_trans_handle *trans,
				     struct btrfs_root *root, u64 owner,
				     u64 offset, u64 ram_bytes,
				     struct btrfs_key *ins)
{
	struct btrfs_ref generic_ref = {
		.action = BTRFS_ADD_DELAYED_EXTENT,
		.bytenr = ins->objectid,
		.num_bytes = ins->offset,
		.owning_root = btrfs_root_id(root),
		.ref_root = btrfs_root_id(root),
	};

	ASSERT(generic_ref.ref_root != BTRFS_TREE_LOG_OBJECTID);

	if (btrfs_is_data_reloc_root(root) && btrfs_is_fstree(root->relocation_src_root))
		generic_ref.owning_root = root->relocation_src_root;

	btrfs_init_data_ref(&generic_ref, owner, offset, 0, false);
	btrfs_ref_tree_mod(root->fs_info, &generic_ref);

	return btrfs_add_delayed_data_ref(trans, &generic_ref, ram_bytes);
}

/*
 * this is used by the tree logging recovery code.  It records that
 * an extent has been allocated and makes sure to clear the free
 * space cache bits as well
 */
int btrfs_alloc_logged_file_extent(struct btrfs_trans_handle *trans,
				   u64 root_objectid, u64 owner, u64 offset,
				   struct btrfs_key *ins)
{
	struct btrfs_fs_info *fs_info = trans->fs_info;
	int ret;
	struct btrfs_block_group *block_group;
	struct btrfs_space_info *space_info;
	const struct btrfs_squota_delta delta = {
		.root = root_objectid,
		.num_bytes = ins->offset,
		.generation = trans->transid,
		.is_data = true,
		.is_inc = true,
	};

	/*
	 * Mixed block groups will exclude before processing the log so we only
	 * need to do the exclude dance if this fs isn't mixed.
	 */
	if (!btrfs_fs_incompat(fs_info, MIXED_GROUPS)) {
		ret = __exclude_logged_extent(fs_info, ins->objectid,
					      ins->offset);
		if (ret)
			return ret;
	}

	block_group = btrfs_lookup_block_group(fs_info, ins->objectid);
	if (!block_group)
		return -EINVAL;

	space_info = block_group->space_info;
	spin_lock(&space_info->lock);
	spin_lock(&block_group->lock);
	space_info->bytes_reserved += ins->offset;
	block_group->reserved += ins->offset;
	spin_unlock(&block_group->lock);
	spin_unlock(&space_info->lock);

	ret = alloc_reserved_file_extent(trans, 0, root_objectid, 0, owner,
					 offset, ins, 1, root_objectid);
	if (ret)
		btrfs_pin_extent(trans, ins->objectid, ins->offset, 1);
	ret = btrfs_record_squota_delta(fs_info, &delta);
	btrfs_put_block_group(block_group);
	return ret;
}

#ifdef CONFIG_BTRFS_DEBUG
/*
 * Extra safety check in case the extent tree is corrupted and extent allocator
 * chooses to use a tree block which is already used and locked.
 */
static bool check_eb_lock_owner(const struct extent_buffer *eb)
{
	if (eb->lock_owner == current->pid) {
		btrfs_err_rl(eb->fs_info,
"tree block %llu owner %llu already locked by pid=%d, extent tree corruption detected",
			     eb->start, btrfs_header_owner(eb), current->pid);
		return true;
	}
	return false;
}
#else
static bool check_eb_lock_owner(struct extent_buffer *eb)
{
	return false;
}
#endif

static struct extent_buffer *
btrfs_init_new_buffer(struct btrfs_trans_handle *trans, struct btrfs_root *root,
		      u64 bytenr, int level, u64 owner,
		      enum btrfs_lock_nesting nest)
{
	struct btrfs_fs_info *fs_info = root->fs_info;
	struct extent_buffer *buf;
	u64 lockdep_owner = owner;

	buf = btrfs_find_create_tree_block(fs_info, bytenr, owner, level);
	if (IS_ERR(buf))
		return buf;

	if (check_eb_lock_owner(buf)) {
		free_extent_buffer(buf);
		return ERR_PTR(-EUCLEAN);
	}

	/*
	 * The reloc trees are just snapshots, so we need them to appear to be
	 * just like any other fs tree WRT lockdep.
	 *
	 * The exception however is in replace_path() in relocation, where we
	 * hold the lock on the original fs root and then search for the reloc
	 * root.  At that point we need to make sure any reloc root buffers are
	 * set to the BTRFS_TREE_RELOC_OBJECTID lockdep class in order to make
	 * lockdep happy.
	 */
	if (lockdep_owner == BTRFS_TREE_RELOC_OBJECTID &&
	    !test_bit(BTRFS_ROOT_RESET_LOCKDEP_CLASS, &root->state))
		lockdep_owner = BTRFS_FS_TREE_OBJECTID;

	/* btrfs_clear_buffer_dirty() accesses generation field. */
	btrfs_set_header_generation(buf, trans->transid);

	/*
	 * This needs to stay, because we could allocate a freed block from an
	 * old tree into a new tree, so we need to make sure this new block is
	 * set to the appropriate level and owner.
	 */
	btrfs_set_buffer_lockdep_class(lockdep_owner, buf, level);

	btrfs_tree_lock_nested(buf, nest);
	btrfs_clear_buffer_dirty(trans, buf);
	clear_bit(EXTENT_BUFFER_STALE, &buf->bflags);
	clear_bit(EXTENT_BUFFER_ZONED_ZEROOUT, &buf->bflags);

	set_extent_buffer_uptodate(buf);

	memzero_extent_buffer(buf, 0, sizeof(struct btrfs_header));
	btrfs_set_header_level(buf, level);
	btrfs_set_header_bytenr(buf, buf->start);
	btrfs_set_header_generation(buf, trans->transid);
	btrfs_set_header_backref_rev(buf, BTRFS_MIXED_BACKREF_REV);
	btrfs_set_header_owner(buf, owner);
	write_extent_buffer_fsid(buf, fs_info->fs_devices->metadata_uuid);
	write_extent_buffer_chunk_tree_uuid(buf, fs_info->chunk_tree_uuid);
	if (btrfs_root_id(root) == BTRFS_TREE_LOG_OBJECTID) {
		buf->log_index = root->log_transid % 2;
		/*
		 * we allow two log transactions at a time, use different
		 * EXTENT bit to differentiate dirty pages.
		 */
		if (buf->log_index == 0)
			btrfs_set_extent_bit(&root->dirty_log_pages, buf->start,
					     buf->start + buf->len - 1,
					     EXTENT_DIRTY_LOG1, NULL);
		else
			btrfs_set_extent_bit(&root->dirty_log_pages, buf->start,
					     buf->start + buf->len - 1,
					     EXTENT_DIRTY_LOG2, NULL);
	} else {
		buf->log_index = -1;
		btrfs_set_extent_bit(&trans->transaction->dirty_pages, buf->start,
				     buf->start + buf->len - 1, EXTENT_DIRTY, NULL);
	}
	/* this returns a buffer locked for blocking */
	return buf;
}

/*
 * finds a free extent and does all the dirty work required for allocation
 * returns the tree buffer or an ERR_PTR on error.
 */
struct extent_buffer *btrfs_alloc_tree_block(struct btrfs_trans_handle *trans,
					     struct btrfs_root *root,
					     u64 parent, u64 root_objectid,
					     const struct btrfs_disk_key *key,
					     int level, u64 hint,
					     u64 empty_size,
					     u64 reloc_src_root,
					     enum btrfs_lock_nesting nest)
{
	struct btrfs_fs_info *fs_info = root->fs_info;
	struct btrfs_key ins;
	struct btrfs_block_rsv *block_rsv;
	struct extent_buffer *buf;
	u64 flags = 0;
	int ret;
	u32 blocksize = fs_info->nodesize;
	bool skinny_metadata = btrfs_fs_incompat(fs_info, SKINNY_METADATA);
	u64 owning_root;

#ifdef CONFIG_BTRFS_FS_RUN_SANITY_TESTS
	if (btrfs_is_testing(fs_info)) {
		buf = btrfs_init_new_buffer(trans, root, root->alloc_bytenr,
					    level, root_objectid, nest);
		if (!IS_ERR(buf))
			root->alloc_bytenr += blocksize;
		return buf;
	}
#endif

	block_rsv = btrfs_use_block_rsv(trans, root, blocksize);
	if (IS_ERR(block_rsv))
		return ERR_CAST(block_rsv);

	ret = btrfs_reserve_extent(root, blocksize, blocksize, blocksize,
				   empty_size, hint, &ins, 0, 0);
	if (ret)
		goto out_unuse;

	buf = btrfs_init_new_buffer(trans, root, ins.objectid, level,
				    root_objectid, nest);
	if (IS_ERR(buf)) {
		ret = PTR_ERR(buf);
		goto out_free_reserved;
	}
	owning_root = btrfs_header_owner(buf);

	if (root_objectid == BTRFS_TREE_RELOC_OBJECTID) {
		if (parent == 0)
			parent = ins.objectid;
		flags |= BTRFS_BLOCK_FLAG_FULL_BACKREF;
		owning_root = reloc_src_root;
	} else
		BUG_ON(parent > 0);

	if (root_objectid != BTRFS_TREE_LOG_OBJECTID) {
		struct btrfs_delayed_extent_op *extent_op;
		struct btrfs_ref generic_ref = {
			.action = BTRFS_ADD_DELAYED_EXTENT,
			.bytenr = ins.objectid,
			.num_bytes = ins.offset,
			.parent = parent,
			.owning_root = owning_root,
			.ref_root = root_objectid,
		};

		if (!skinny_metadata || flags != 0) {
			extent_op = btrfs_alloc_delayed_extent_op();
			if (!extent_op) {
				ret = -ENOMEM;
				goto out_free_buf;
			}
			if (key)
				memcpy(&extent_op->key, key, sizeof(extent_op->key));
			else
				memset(&extent_op->key, 0, sizeof(extent_op->key));
			extent_op->flags_to_set = flags;
			extent_op->update_key = (skinny_metadata ? false : true);
			extent_op->update_flags = (flags != 0);
		} else {
			extent_op = NULL;
		}

		btrfs_init_tree_ref(&generic_ref, level, btrfs_root_id(root), false);
		btrfs_ref_tree_mod(fs_info, &generic_ref);
		ret = btrfs_add_delayed_tree_ref(trans, &generic_ref, extent_op);
		if (ret) {
			btrfs_free_delayed_extent_op(extent_op);
			goto out_free_buf;
		}
	}
	return buf;

out_free_buf:
	btrfs_tree_unlock(buf);
	free_extent_buffer(buf);
out_free_reserved:
	btrfs_free_reserved_extent(fs_info, ins.objectid, ins.offset, false);
out_unuse:
	btrfs_unuse_block_rsv(fs_info, block_rsv, blocksize);
	return ERR_PTR(ret);
}

struct walk_control {
	u64 refs[BTRFS_MAX_LEVEL];
	u64 flags[BTRFS_MAX_LEVEL];
	struct btrfs_key update_progress;
	struct btrfs_key drop_progress;
	int drop_level;
	int stage;
	int level;
	int shared_level;
	int update_ref;
	int keep_locks;
	int reada_slot;
	int reada_count;
	int restarted;
	/* Indicate that extent info needs to be looked up when walking the tree. */
	int lookup_info;
};

/*
 * This is our normal stage.  We are traversing blocks the current snapshot owns
 * and we are dropping any of our references to any children we are able to, and
 * then freeing the block once we've processed all of the children.
 */
#define DROP_REFERENCE	1

/*
 * We enter this stage when we have to walk into a child block (meaning we can't
 * simply drop our reference to it from our current parent node) and there are
 * more than one reference on it.  If we are the owner of any of the children
 * blocks from the current parent node then we have to do the FULL_BACKREF dance
 * on them in order to drop our normal ref and add the shared ref.
 */
#define UPDATE_BACKREF	2

/*
 * Decide if we need to walk down into this node to adjust the references.
 *
 * @root:	the root we are currently deleting
 * @wc:		the walk control for this deletion
 * @eb:		the parent eb that we're currently visiting
 * @refs:	the number of refs for wc->level - 1
 * @flags:	the flags for wc->level - 1
 * @slot:	the slot in the eb that we're currently checking
 *
 * This is meant to be called when we're evaluating if a node we point to at
 * wc->level should be read and walked into, or if we can simply delete our
 * reference to it.  We return true if we should walk into the node, false if we
 * can skip it.
 *
 * We have assertions in here to make sure this is called correctly.  We assume
 * that sanity checking on the blocks read to this point has been done, so any
 * corrupted file systems must have been caught before calling this function.
 */
static bool visit_node_for_delete(struct btrfs_root *root, struct walk_control *wc,
				  struct extent_buffer *eb, u64 flags, int slot)
{
	struct btrfs_key key;
	u64 generation;
	int level = wc->level;

	ASSERT(level > 0);
	ASSERT(wc->refs[level - 1] > 0);

	/*
	 * The update backref stage we only want to skip if we already have
	 * FULL_BACKREF set, otherwise we need to read.
	 */
	if (wc->stage == UPDATE_BACKREF) {
		if (level == 1 && flags & BTRFS_BLOCK_FLAG_FULL_BACKREF)
			return false;
		return true;
	}

	/*
	 * We're the last ref on this block, we must walk into it and process
	 * any refs it's pointing at.
	 */
	if (wc->refs[level - 1] == 1)
		return true;

	/*
	 * If we're already FULL_BACKREF then we know we can just drop our
	 * current reference.
	 */
	if (level == 1 && flags & BTRFS_BLOCK_FLAG_FULL_BACKREF)
		return false;

	/*
	 * This block is older than our creation generation, we can drop our
	 * reference to it.
	 */
	generation = btrfs_node_ptr_generation(eb, slot);
	if (!wc->update_ref || generation <= btrfs_root_origin_generation(root))
		return false;

	/*
	 * This block was processed from a previous snapshot deletion run, we
	 * can skip it.
	 */
	btrfs_node_key_to_cpu(eb, &key, slot);
	if (btrfs_comp_cpu_keys(&key, &wc->update_progress) < 0)
		return false;

	/* All other cases we need to wander into the node. */
	return true;
}

static noinline void reada_walk_down(struct btrfs_trans_handle *trans,
				     struct btrfs_root *root,
				     struct walk_control *wc,
				     struct btrfs_path *path)
{
	struct btrfs_fs_info *fs_info = root->fs_info;
	u64 bytenr;
	u64 generation;
	u64 refs;
	u64 flags;
	u32 nritems;
	struct extent_buffer *eb;
	int ret;
	int slot;
	int nread = 0;

	if (path->slots[wc->level] < wc->reada_slot) {
		wc->reada_count = wc->reada_count * 2 / 3;
		wc->reada_count = max(wc->reada_count, 2);
	} else {
		wc->reada_count = wc->reada_count * 3 / 2;
		wc->reada_count = min_t(int, wc->reada_count,
					BTRFS_NODEPTRS_PER_BLOCK(fs_info));
	}

	eb = path->nodes[wc->level];
	nritems = btrfs_header_nritems(eb);

	for (slot = path->slots[wc->level]; slot < nritems; slot++) {
		if (nread >= wc->reada_count)
			break;

		cond_resched();
		bytenr = btrfs_node_blockptr(eb, slot);
		generation = btrfs_node_ptr_generation(eb, slot);

		if (slot == path->slots[wc->level])
			goto reada;

		if (wc->stage == UPDATE_BACKREF &&
		    generation <= btrfs_root_origin_generation(root))
			continue;

		/* We don't lock the tree block, it's OK to be racy here */
		ret = btrfs_lookup_extent_info(trans, fs_info, bytenr,
					       wc->level - 1, 1, &refs,
					       &flags, NULL);
		/* We don't care about errors in readahead. */
		if (ret < 0)
			continue;

		/*
		 * This could be racey, it's conceivable that we raced and end
		 * up with a bogus refs count, if that's the case just skip, if
		 * we are actually corrupt we will notice when we look up
		 * everything again with our locks.
		 */
		if (refs == 0)
			continue;

		/* If we don't need to visit this node don't reada. */
		if (!visit_node_for_delete(root, wc, eb, flags, slot))
			continue;
reada:
		btrfs_readahead_node_child(eb, slot);
		nread++;
	}
	wc->reada_slot = slot;
}

/*
 * helper to process tree block while walking down the tree.
 *
 * when wc->stage == UPDATE_BACKREF, this function updates
 * back refs for pointers in the block.
 *
 * NOTE: return value 1 means we should stop walking down.
 */
static noinline int walk_down_proc(struct btrfs_trans_handle *trans,
				   struct btrfs_root *root,
				   struct btrfs_path *path,
				   struct walk_control *wc)
{
	struct btrfs_fs_info *fs_info = root->fs_info;
	int level = wc->level;
	struct extent_buffer *eb = path->nodes[level];
	u64 flag = BTRFS_BLOCK_FLAG_FULL_BACKREF;
	int ret;

	if (wc->stage == UPDATE_BACKREF && btrfs_header_owner(eb) != btrfs_root_id(root))
		return 1;

	/*
	 * when reference count of tree block is 1, it won't increase
	 * again. once full backref flag is set, we never clear it.
	 */
	if (wc->lookup_info &&
	    ((wc->stage == DROP_REFERENCE && wc->refs[level] != 1) ||
	     (wc->stage == UPDATE_BACKREF && !(wc->flags[level] & flag)))) {
		ASSERT(path->locks[level]);
		ret = btrfs_lookup_extent_info(trans, fs_info,
					       eb->start, level, 1,
					       &wc->refs[level],
					       &wc->flags[level],
					       NULL);
		if (ret)
			return ret;
		if (unlikely(wc->refs[level] == 0)) {
			btrfs_err(fs_info, "bytenr %llu has 0 references, expect > 0",
				  eb->start);
			return -EUCLEAN;
		}
	}

	if (wc->stage == DROP_REFERENCE) {
		if (wc->refs[level] > 1)
			return 1;

		if (path->locks[level] && !wc->keep_locks) {
			btrfs_tree_unlock_rw(eb, path->locks[level]);
			path->locks[level] = 0;
		}
		return 0;
	}

	/* wc->stage == UPDATE_BACKREF */
	if (!(wc->flags[level] & flag)) {
		ASSERT(path->locks[level]);
		ret = btrfs_inc_ref(trans, root, eb, 1);
		if (ret) {
			btrfs_abort_transaction(trans, ret);
			return ret;
		}
		ret = btrfs_dec_ref(trans, root, eb, 0);
		if (ret) {
			btrfs_abort_transaction(trans, ret);
			return ret;
		}
		ret = btrfs_set_disk_extent_flags(trans, eb, flag);
		if (ret) {
			btrfs_abort_transaction(trans, ret);
			return ret;
		}
		wc->flags[level] |= flag;
	}

	/*
	 * the block is shared by multiple trees, so it's not good to
	 * keep the tree lock
	 */
	if (path->locks[level] && level > 0) {
		btrfs_tree_unlock_rw(eb, path->locks[level]);
		path->locks[level] = 0;
	}
	return 0;
}

/*
 * This is used to verify a ref exists for this root to deal with a bug where we
 * would have a drop_progress key that hadn't been updated properly.
 */
static int check_ref_exists(struct btrfs_trans_handle *trans,
			    struct btrfs_root *root, u64 bytenr, u64 parent,
			    int level)
{
	struct btrfs_delayed_ref_root *delayed_refs;
	struct btrfs_delayed_ref_head *head;
	BTRFS_PATH_AUTO_FREE(path);
	struct btrfs_extent_inline_ref *iref;
	int ret;
	bool exists = false;

	path = btrfs_alloc_path();
	if (!path)
		return -ENOMEM;
again:
	ret = lookup_extent_backref(trans, path, &iref, bytenr,
				    root->fs_info->nodesize, parent,
				    btrfs_root_id(root), level, 0);
	if (ret != -ENOENT) {
		/*
		 * If we get 0 then we found our reference, return 1, else
		 * return the error if it's not -ENOENT;
		 */
		return (ret < 0 ) ? ret : 1;
	}

	/*
	 * We could have a delayed ref with this reference, so look it up while
	 * we're holding the path open to make sure we don't race with the
	 * delayed ref running.
	 */
	delayed_refs = &trans->transaction->delayed_refs;
	spin_lock(&delayed_refs->lock);
	head = btrfs_find_delayed_ref_head(root->fs_info, delayed_refs, bytenr);
	if (!head)
		goto out;
	if (!mutex_trylock(&head->mutex)) {
		/*
		 * We're contended, means that the delayed ref is running, get a
		 * reference and wait for the ref head to be complete and then
		 * try again.
		 */
		refcount_inc(&head->refs);
		spin_unlock(&delayed_refs->lock);

		btrfs_release_path(path);

		mutex_lock(&head->mutex);
		mutex_unlock(&head->mutex);
		btrfs_put_delayed_ref_head(head);
		goto again;
	}

	exists = btrfs_find_delayed_tree_ref(head, btrfs_root_id(root), parent);
	mutex_unlock(&head->mutex);
out:
	spin_unlock(&delayed_refs->lock);
	return exists ? 1 : 0;
}

/*
 * We may not have an uptodate block, so if we are going to walk down into this
 * block we need to drop the lock, read it off of the disk, re-lock it and
 * return to continue dropping the snapshot.
 */
static int check_next_block_uptodate(struct btrfs_trans_handle *trans,
				     struct btrfs_root *root,
				     struct btrfs_path *path,
				     struct walk_control *wc,
				     struct extent_buffer *next)
{
	struct btrfs_tree_parent_check check = { 0 };
	u64 generation;
	int level = wc->level;
	int ret;

	btrfs_assert_tree_write_locked(next);

	generation = btrfs_node_ptr_generation(path->nodes[level], path->slots[level]);

	if (btrfs_buffer_uptodate(next, generation, 0))
		return 0;

	check.level = level - 1;
	check.transid = generation;
	check.owner_root = btrfs_root_id(root);
	check.has_first_key = true;
	btrfs_node_key_to_cpu(path->nodes[level], &check.first_key, path->slots[level]);

	btrfs_tree_unlock(next);
	if (level == 1)
		reada_walk_down(trans, root, wc, path);
	ret = btrfs_read_extent_buffer(next, &check);
	if (ret) {
		free_extent_buffer(next);
		return ret;
	}
	btrfs_tree_lock(next);
	wc->lookup_info = 1;
	return 0;
}

/*
 * If we determine that we don't have to visit wc->level - 1 then we need to
 * determine if we can drop our reference.
 *
 * If we are UPDATE_BACKREF then we will not, we need to update our backrefs.
 *
 * If we are DROP_REFERENCE this will figure out if we need to drop our current
 * reference, skipping it if we dropped it from a previous incompleted drop, or
 * dropping it if we still have a reference to it.
 */
static int maybe_drop_reference(struct btrfs_trans_handle *trans, struct btrfs_root *root,
				struct btrfs_path *path, struct walk_control *wc,
				struct extent_buffer *next, u64 owner_root)
{
	struct btrfs_ref ref = {
		.action = BTRFS_DROP_DELAYED_REF,
		.bytenr = next->start,
		.num_bytes = root->fs_info->nodesize,
		.owning_root = owner_root,
		.ref_root = btrfs_root_id(root),
	};
	int level = wc->level;
	int ret;

	/* We are UPDATE_BACKREF, we're not dropping anything. */
	if (wc->stage == UPDATE_BACKREF)
		return 0;

	if (wc->flags[level] & BTRFS_BLOCK_FLAG_FULL_BACKREF) {
		ref.parent = path->nodes[level]->start;
	} else {
		ASSERT(btrfs_root_id(root) == btrfs_header_owner(path->nodes[level]));
		if (btrfs_root_id(root) != btrfs_header_owner(path->nodes[level])) {
			btrfs_err(root->fs_info, "mismatched block owner");
			return -EIO;
		}
	}

	/*
	 * If we had a drop_progress we need to verify the refs are set as
	 * expected.  If we find our ref then we know that from here on out
	 * everything should be correct, and we can clear the
	 * ->restarted flag.
	 */
	if (wc->restarted) {
		ret = check_ref_exists(trans, root, next->start, ref.parent,
				       level - 1);
		if (ret <= 0)
			return ret;
		ret = 0;
		wc->restarted = 0;
	}

	/*
	 * Reloc tree doesn't contribute to qgroup numbers, and we have already
	 * accounted them at merge time (replace_path), thus we could skip
	 * expensive subtree trace here.
	 */
	if (btrfs_root_id(root) != BTRFS_TREE_RELOC_OBJECTID &&
	    wc->refs[level - 1] > 1) {
		u64 generation = btrfs_node_ptr_generation(path->nodes[level],
							   path->slots[level]);

		ret = btrfs_qgroup_trace_subtree(trans, next, generation, level - 1);
		if (ret) {
			btrfs_err_rl(root->fs_info,
"error %d accounting shared subtree, quota is out of sync, rescan required",
				     ret);
		}
	}

	/*
	 * We need to update the next key in our walk control so we can update
	 * the drop_progress key accordingly.  We don't care if find_next_key
	 * doesn't find a key because that means we're at the end and are going
	 * to clean up now.
	 */
	wc->drop_level = level;
	find_next_key(path, level, &wc->drop_progress);

	btrfs_init_tree_ref(&ref, level - 1, 0, false);
	return btrfs_free_extent(trans, &ref);
}

/*
 * helper to process tree block pointer.
 *
 * when wc->stage == DROP_REFERENCE, this function checks
 * reference count of the block pointed to. if the block
 * is shared and we need update back refs for the subtree
 * rooted at the block, this function changes wc->stage to
 * UPDATE_BACKREF. if the block is shared and there is no
 * need to update back, this function drops the reference
 * to the block.
 *
 * NOTE: return value 1 means we should stop walking down.
 */
static noinline int do_walk_down(struct btrfs_trans_handle *trans,
				 struct btrfs_root *root,
				 struct btrfs_path *path,
				 struct walk_control *wc)
{
	struct btrfs_fs_info *fs_info = root->fs_info;
	u64 bytenr;
	u64 generation;
	u64 owner_root = 0;
	struct extent_buffer *next;
	int level = wc->level;
	int ret = 0;

	generation = btrfs_node_ptr_generation(path->nodes[level],
					       path->slots[level]);
	/*
	 * if the lower level block was created before the snapshot
	 * was created, we know there is no need to update back refs
	 * for the subtree
	 */
	if (wc->stage == UPDATE_BACKREF &&
	    generation <= btrfs_root_origin_generation(root)) {
		wc->lookup_info = 1;
		return 1;
	}

	bytenr = btrfs_node_blockptr(path->nodes[level], path->slots[level]);

	next = btrfs_find_create_tree_block(fs_info, bytenr, btrfs_root_id(root),
					    level - 1);
	if (IS_ERR(next))
		return PTR_ERR(next);

	btrfs_tree_lock(next);

	ret = btrfs_lookup_extent_info(trans, fs_info, bytenr, level - 1, 1,
				       &wc->refs[level - 1],
				       &wc->flags[level - 1],
				       &owner_root);
	if (ret < 0)
		goto out_unlock;

	if (unlikely(wc->refs[level - 1] == 0)) {
		btrfs_err(fs_info, "bytenr %llu has 0 references, expect > 0",
			  bytenr);
		ret = -EUCLEAN;
		goto out_unlock;
	}
	wc->lookup_info = 0;

	/* If we don't have to walk into this node skip it. */
	if (!visit_node_for_delete(root, wc, path->nodes[level],
				   wc->flags[level - 1], path->slots[level]))
		goto skip;

	/*
	 * We have to walk down into this node, and if we're currently at the
	 * DROP_REFERNCE stage and this block is shared then we need to switch
	 * to the UPDATE_BACKREF stage in order to convert to FULL_BACKREF.
	 */
	if (wc->stage == DROP_REFERENCE && wc->refs[level - 1] > 1) {
		wc->stage = UPDATE_BACKREF;
		wc->shared_level = level - 1;
	}

	ret = check_next_block_uptodate(trans, root, path, wc, next);
	if (ret)
		return ret;

	level--;
	ASSERT(level == btrfs_header_level(next));
	if (level != btrfs_header_level(next)) {
		btrfs_err(root->fs_info, "mismatched level");
		ret = -EIO;
		goto out_unlock;
	}
	path->nodes[level] = next;
	path->slots[level] = 0;
	path->locks[level] = BTRFS_WRITE_LOCK;
	wc->level = level;
	if (wc->level == 1)
		wc->reada_slot = 0;
	return 0;
skip:
	ret = maybe_drop_reference(trans, root, path, wc, next, owner_root);
	if (ret)
		goto out_unlock;
	wc->refs[level - 1] = 0;
	wc->flags[level - 1] = 0;
	wc->lookup_info = 1;
	ret = 1;

out_unlock:
	btrfs_tree_unlock(next);
	free_extent_buffer(next);

	return ret;
}

/*
 * helper to process tree block while walking up the tree.
 *
 * when wc->stage == DROP_REFERENCE, this function drops
 * reference count on the block.
 *
 * when wc->stage == UPDATE_BACKREF, this function changes
 * wc->stage back to DROP_REFERENCE if we changed wc->stage
 * to UPDATE_BACKREF previously while processing the block.
 *
 * NOTE: return value 1 means we should stop walking up.
 */
static noinline int walk_up_proc(struct btrfs_trans_handle *trans,
				 struct btrfs_root *root,
				 struct btrfs_path *path,
				 struct walk_control *wc)
{
	struct btrfs_fs_info *fs_info = root->fs_info;
	int ret = 0;
	int level = wc->level;
	struct extent_buffer *eb = path->nodes[level];
	u64 parent = 0;

	if (wc->stage == UPDATE_BACKREF) {
		ASSERT(wc->shared_level >= level);
		if (level < wc->shared_level)
			goto out;

		ret = find_next_key(path, level + 1, &wc->update_progress);
		if (ret > 0)
			wc->update_ref = 0;

		wc->stage = DROP_REFERENCE;
		wc->shared_level = -1;
		path->slots[level] = 0;

		/*
		 * check reference count again if the block isn't locked.
		 * we should start walking down the tree again if reference
		 * count is one.
		 */
		if (!path->locks[level]) {
			ASSERT(level > 0);
			btrfs_tree_lock(eb);
			path->locks[level] = BTRFS_WRITE_LOCK;

			ret = btrfs_lookup_extent_info(trans, fs_info,
						       eb->start, level, 1,
						       &wc->refs[level],
						       &wc->flags[level],
						       NULL);
			if (ret < 0) {
				btrfs_tree_unlock_rw(eb, path->locks[level]);
				path->locks[level] = 0;
				return ret;
			}
			if (unlikely(wc->refs[level] == 0)) {
				btrfs_tree_unlock_rw(eb, path->locks[level]);
				btrfs_err(fs_info, "bytenr %llu has 0 references, expect > 0",
					  eb->start);
				return -EUCLEAN;
			}
			if (wc->refs[level] == 1) {
				btrfs_tree_unlock_rw(eb, path->locks[level]);
				path->locks[level] = 0;
				return 1;
			}
		}
	}

	/* wc->stage == DROP_REFERENCE */
	ASSERT(path->locks[level] || wc->refs[level] == 1);

	if (wc->refs[level] == 1) {
		if (level == 0) {
			if (wc->flags[level] & BTRFS_BLOCK_FLAG_FULL_BACKREF) {
				ret = btrfs_dec_ref(trans, root, eb, 1);
				if (ret) {
					btrfs_abort_transaction(trans, ret);
					return ret;
				}
			} else {
				ret = btrfs_dec_ref(trans, root, eb, 0);
				if (ret) {
					btrfs_abort_transaction(trans, ret);
					return ret;
				}
			}
			if (btrfs_is_fstree(btrfs_root_id(root))) {
				ret = btrfs_qgroup_trace_leaf_items(trans, eb);
				if (ret) {
					btrfs_err_rl(fs_info,
	"error %d accounting leaf items, quota is out of sync, rescan required",
					     ret);
				}
			}
		}
		/* Make block locked assertion in btrfs_clear_buffer_dirty happy. */
		if (!path->locks[level]) {
			btrfs_tree_lock(eb);
			path->locks[level] = BTRFS_WRITE_LOCK;
		}
		btrfs_clear_buffer_dirty(trans, eb);
	}

	if (eb == root->node) {
		if (wc->flags[level] & BTRFS_BLOCK_FLAG_FULL_BACKREF)
			parent = eb->start;
		else if (btrfs_root_id(root) != btrfs_header_owner(eb))
			goto owner_mismatch;
	} else {
		if (wc->flags[level + 1] & BTRFS_BLOCK_FLAG_FULL_BACKREF)
			parent = path->nodes[level + 1]->start;
		else if (btrfs_root_id(root) !=
			 btrfs_header_owner(path->nodes[level + 1]))
			goto owner_mismatch;
	}

	ret = btrfs_free_tree_block(trans, btrfs_root_id(root), eb, parent,
				    wc->refs[level] == 1);
	if (ret < 0)
		btrfs_abort_transaction(trans, ret);
out:
	wc->refs[level] = 0;
	wc->flags[level] = 0;
	return ret;

owner_mismatch:
	btrfs_err_rl(fs_info, "unexpected tree owner, have %llu expect %llu",
		     btrfs_header_owner(eb), btrfs_root_id(root));
	return -EUCLEAN;
}

/*
 * walk_down_tree consists of two steps.
 *
 * walk_down_proc().  Look up the reference count and reference of our current
 * wc->level.  At this point path->nodes[wc->level] should be populated and
 * uptodate, and in most cases should already be locked.  If we are in
 * DROP_REFERENCE and our refcount is > 1 then we've entered a shared node and
 * we can walk back up the tree.  If we are UPDATE_BACKREF we have to set
 * FULL_BACKREF on this node if it's not already set, and then do the
 * FULL_BACKREF conversion dance, which is to drop the root reference and add
 * the shared reference to all of this nodes children.
 *
 * do_walk_down().  This is where we actually start iterating on the children of
 * our current path->nodes[wc->level].  For DROP_REFERENCE that means dropping
 * our reference to the children that return false from visit_node_for_delete(),
 * which has various conditions where we know we can just drop our reference
 * without visiting the node.  For UPDATE_BACKREF we will skip any children that
 * visit_node_for_delete() returns false for, only walking down when necessary.
 * The bulk of the work for UPDATE_BACKREF occurs in the walk_up_tree() part of
 * snapshot deletion.
 */
static noinline int walk_down_tree(struct btrfs_trans_handle *trans,
				   struct btrfs_root *root,
				   struct btrfs_path *path,
				   struct walk_control *wc)
{
	int level = wc->level;
	int ret = 0;

	wc->lookup_info = 1;
	while (level >= 0) {
		ret = walk_down_proc(trans, root, path, wc);
		if (ret)
			break;

		if (level == 0)
			break;

		if (path->slots[level] >=
		    btrfs_header_nritems(path->nodes[level]))
			break;

		ret = do_walk_down(trans, root, path, wc);
		if (ret > 0) {
			path->slots[level]++;
			continue;
		} else if (ret < 0)
			break;
		level = wc->level;
	}
	return (ret == 1) ? 0 : ret;
}

/*
 * walk_up_tree() is responsible for making sure we visit every slot on our
 * current node, and if we're at the end of that node then we call
 * walk_up_proc() on our current node which will do one of a few things based on
 * our stage.
 *
 * UPDATE_BACKREF.  If we wc->level is currently less than our wc->shared_level
 * then we need to walk back up the tree, and then going back down into the
 * other slots via walk_down_tree to update any other children from our original
 * wc->shared_level.  Once we're at or above our wc->shared_level we can switch
 * back to DROP_REFERENCE, lookup the current nodes refs and flags, and carry on.
 *
 * DROP_REFERENCE. If our refs == 1 then we're going to free this tree block.
 * If we're level 0 then we need to btrfs_dec_ref() on all of the data extents
 * in our current leaf.  After that we call btrfs_free_tree_block() on the
 * current node and walk up to the next node to walk down the next slot.
 */
static noinline int walk_up_tree(struct btrfs_trans_handle *trans,
				 struct btrfs_root *root,
				 struct btrfs_path *path,
				 struct walk_control *wc, int max_level)
{
	int level = wc->level;
	int ret;

	path->slots[level] = btrfs_header_nritems(path->nodes[level]);
	while (level < max_level && path->nodes[level]) {
		wc->level = level;
		if (path->slots[level] + 1 <
		    btrfs_header_nritems(path->nodes[level])) {
			path->slots[level]++;
			return 0;
		} else {
			ret = walk_up_proc(trans, root, path, wc);
			if (ret > 0)
				return 0;
			if (ret < 0)
				return ret;

			if (path->locks[level]) {
				btrfs_tree_unlock_rw(path->nodes[level],
						     path->locks[level]);
				path->locks[level] = 0;
			}
			free_extent_buffer(path->nodes[level]);
			path->nodes[level] = NULL;
			level++;
		}
	}
	return 1;
}

/*
 * drop a subvolume tree.
 *
 * this function traverses the tree freeing any blocks that only
 * referenced by the tree.
 *
 * when a shared tree block is found. this function decreases its
 * reference count by one. if update_ref is true, this function
 * also make sure backrefs for the shared block and all lower level
 * blocks are properly updated.
 *
 * If called with for_reloc == 0, may exit early with -EAGAIN
 */
int btrfs_drop_snapshot(struct btrfs_root *root, int update_ref, int for_reloc)
{
	const bool is_reloc_root = (btrfs_root_id(root) == BTRFS_TREE_RELOC_OBJECTID);
	struct btrfs_fs_info *fs_info = root->fs_info;
	struct btrfs_path *path;
	struct btrfs_trans_handle *trans;
	struct btrfs_root *tree_root = fs_info->tree_root;
	struct btrfs_root_item *root_item = &root->root_item;
	struct walk_control *wc;
	struct btrfs_key key;
	const u64 rootid = btrfs_root_id(root);
	int ret = 0;
	int level;
	bool root_dropped = false;
	bool unfinished_drop = false;

	btrfs_debug(fs_info, "Drop subvolume %llu", btrfs_root_id(root));

	path = btrfs_alloc_path();
	if (!path) {
		ret = -ENOMEM;
		goto out;
	}

	wc = kzalloc(sizeof(*wc), GFP_NOFS);
	if (!wc) {
		btrfs_free_path(path);
		ret = -ENOMEM;
		goto out;
	}

	/*
	 * Use join to avoid potential EINTR from transaction start. See
	 * wait_reserve_ticket and the whole reservation callchain.
	 */
	if (for_reloc)
		trans = btrfs_join_transaction(tree_root);
	else
		trans = btrfs_start_transaction(tree_root, 0);
	if (IS_ERR(trans)) {
		ret = PTR_ERR(trans);
		goto out_free;
	}

	ret = btrfs_run_delayed_items(trans);
	if (ret)
		goto out_end_trans;

	/*
	 * This will help us catch people modifying the fs tree while we're
	 * dropping it.  It is unsafe to mess with the fs tree while it's being
	 * dropped as we unlock the root node and parent nodes as we walk down
	 * the tree, assuming nothing will change.  If something does change
	 * then we'll have stale information and drop references to blocks we've
	 * already dropped.
	 */
	set_bit(BTRFS_ROOT_DELETING, &root->state);
	unfinished_drop = test_bit(BTRFS_ROOT_UNFINISHED_DROP, &root->state);

	if (btrfs_disk_key_objectid(&root_item->drop_progress) == 0) {
		level = btrfs_header_level(root->node);
		path->nodes[level] = btrfs_lock_root_node(root);
		path->slots[level] = 0;
		path->locks[level] = BTRFS_WRITE_LOCK;
		memset(&wc->update_progress, 0,
		       sizeof(wc->update_progress));
	} else {
		btrfs_disk_key_to_cpu(&key, &root_item->drop_progress);
		memcpy(&wc->update_progress, &key,
		       sizeof(wc->update_progress));

		level = btrfs_root_drop_level(root_item);
		BUG_ON(level == 0);
		path->lowest_level = level;
		ret = btrfs_search_slot(NULL, root, &key, path, 0, 0);
		path->lowest_level = 0;
		if (ret < 0)
			goto out_end_trans;

		WARN_ON(ret > 0);
		ret = 0;

		/*
		 * unlock our path, this is safe because only this
		 * function is allowed to delete this snapshot
		 */
		btrfs_unlock_up_safe(path, 0);

		level = btrfs_header_level(root->node);
		while (1) {
			btrfs_tree_lock(path->nodes[level]);
			path->locks[level] = BTRFS_WRITE_LOCK;

			/*
			 * btrfs_lookup_extent_info() returns 0 for success,
			 * or < 0 for error.
			 */
			ret = btrfs_lookup_extent_info(trans, fs_info,
						path->nodes[level]->start,
						level, 1, &wc->refs[level],
						&wc->flags[level], NULL);
			if (ret < 0)
				goto out_end_trans;

			BUG_ON(wc->refs[level] == 0);

			if (level == btrfs_root_drop_level(root_item))
				break;

			btrfs_tree_unlock(path->nodes[level]);
			path->locks[level] = 0;
			WARN_ON(wc->refs[level] != 1);
			level--;
		}
	}

	wc->restarted = test_bit(BTRFS_ROOT_DEAD_TREE, &root->state);
	wc->level = level;
	wc->shared_level = -1;
	wc->stage = DROP_REFERENCE;
	wc->update_ref = update_ref;
	wc->keep_locks = 0;
	wc->reada_count = BTRFS_NODEPTRS_PER_BLOCK(fs_info);

	while (1) {

		ret = walk_down_tree(trans, root, path, wc);
		if (ret < 0) {
			btrfs_abort_transaction(trans, ret);
			break;
		}

		ret = walk_up_tree(trans, root, path, wc, BTRFS_MAX_LEVEL);
		if (ret < 0) {
			btrfs_abort_transaction(trans, ret);
			break;
		}

		if (ret > 0) {
			BUG_ON(wc->stage != DROP_REFERENCE);
			ret = 0;
			break;
		}

		if (wc->stage == DROP_REFERENCE) {
			wc->drop_level = wc->level;
			btrfs_node_key_to_cpu(path->nodes[wc->drop_level],
					      &wc->drop_progress,
					      path->slots[wc->drop_level]);
		}
		btrfs_cpu_key_to_disk(&root_item->drop_progress,
				      &wc->drop_progress);
		btrfs_set_root_drop_level(root_item, wc->drop_level);

		BUG_ON(wc->level == 0);
		if (btrfs_should_end_transaction(trans) ||
		    (!for_reloc && btrfs_need_cleaner_sleep(fs_info))) {
			ret = btrfs_update_root(trans, tree_root,
						&root->root_key,
						root_item);
			if (ret) {
				btrfs_abort_transaction(trans, ret);
				goto out_end_trans;
			}

			if (!is_reloc_root)
				btrfs_set_last_root_drop_gen(fs_info, trans->transid);

			btrfs_end_transaction_throttle(trans);
			if (!for_reloc && btrfs_need_cleaner_sleep(fs_info)) {
				btrfs_debug(fs_info,
					    "drop snapshot early exit");
				ret = -EAGAIN;
				goto out_free;
			}

		       /*
			* Use join to avoid potential EINTR from transaction
			* start. See wait_reserve_ticket and the whole
			* reservation callchain.
			*/
			if (for_reloc)
				trans = btrfs_join_transaction(tree_root);
			else
				trans = btrfs_start_transaction(tree_root, 0);
			if (IS_ERR(trans)) {
				ret = PTR_ERR(trans);
				goto out_free;
			}
		}
	}
	btrfs_release_path(path);
	if (ret)
		goto out_end_trans;

	ret = btrfs_del_root(trans, &root->root_key);
	if (ret) {
		btrfs_abort_transaction(trans, ret);
		goto out_end_trans;
	}

	if (!is_reloc_root) {
		ret = btrfs_find_root(tree_root, &root->root_key, path,
				      NULL, NULL);
		if (ret < 0) {
			btrfs_abort_transaction(trans, ret);
			goto out_end_trans;
		} else if (ret > 0) {
			ret = 0;
			/*
			 * If we fail to delete the orphan item this time
			 * around, it'll get picked up the next time.
			 *
			 * The most common failure here is just -ENOENT.
			 */
			btrfs_del_orphan_item(trans, tree_root, btrfs_root_id(root));
		}
	}

	/*
	 * This subvolume is going to be completely dropped, and won't be
	 * recorded as dirty roots, thus pertrans meta rsv will not be freed at
	 * commit transaction time.  So free it here manually.
	 */
	btrfs_qgroup_convert_reserved_meta(root, INT_MAX);
	btrfs_qgroup_free_meta_all_pertrans(root);

	if (test_bit(BTRFS_ROOT_IN_RADIX, &root->state))
		btrfs_add_dropped_root(trans, root);
	else
		btrfs_put_root(root);
	root_dropped = true;
out_end_trans:
	if (!is_reloc_root)
		btrfs_set_last_root_drop_gen(fs_info, trans->transid);

	btrfs_end_transaction_throttle(trans);
out_free:
	kfree(wc);
	btrfs_free_path(path);
out:
	if (!ret && root_dropped) {
		ret = btrfs_qgroup_cleanup_dropped_subvolume(fs_info, rootid);
		if (ret < 0)
			btrfs_warn_rl(fs_info,
				      "failed to cleanup qgroup 0/%llu: %d",
				      rootid, ret);
		ret = 0;
	}
	/*
	 * We were an unfinished drop root, check to see if there are any
	 * pending, and if not clear and wake up any waiters.
	 */
	if (!ret && unfinished_drop)
		btrfs_maybe_wake_unfinished_drop(fs_info);

	/*
	 * So if we need to stop dropping the snapshot for whatever reason we
	 * need to make sure to add it back to the dead root list so that we
	 * keep trying to do the work later.  This also cleans up roots if we
	 * don't have it in the radix (like when we recover after a power fail
	 * or unmount) so we don't leak memory.
	 */
	if (!for_reloc && !root_dropped)
		btrfs_add_dead_root(root);
	return ret;
}

/*
 * drop subtree rooted at tree block 'node'.
 *
 * NOTE: this function will unlock and release tree block 'node'
 * only used by relocation code
 */
int btrfs_drop_subtree(struct btrfs_trans_handle *trans,
			struct btrfs_root *root,
			struct extent_buffer *node,
			struct extent_buffer *parent)
{
	struct btrfs_fs_info *fs_info = root->fs_info;
	BTRFS_PATH_AUTO_FREE(path);
	struct walk_control *wc;
	int level;
	int parent_level;
	int ret = 0;

	BUG_ON(btrfs_root_id(root) != BTRFS_TREE_RELOC_OBJECTID);

	path = btrfs_alloc_path();
	if (!path)
		return -ENOMEM;

	wc = kzalloc(sizeof(*wc), GFP_NOFS);
	if (!wc)
		return -ENOMEM;

	btrfs_assert_tree_write_locked(parent);
	parent_level = btrfs_header_level(parent);
	refcount_inc(&parent->refs);
	path->nodes[parent_level] = parent;
	path->slots[parent_level] = btrfs_header_nritems(parent);

	btrfs_assert_tree_write_locked(node);
	level = btrfs_header_level(node);
	path->nodes[level] = node;
	path->slots[level] = 0;
	path->locks[level] = BTRFS_WRITE_LOCK;

	wc->refs[parent_level] = 1;
	wc->flags[parent_level] = BTRFS_BLOCK_FLAG_FULL_BACKREF;
	wc->level = level;
	wc->shared_level = -1;
	wc->stage = DROP_REFERENCE;
	wc->update_ref = 0;
	wc->keep_locks = 1;
	wc->reada_count = BTRFS_NODEPTRS_PER_BLOCK(fs_info);

	while (1) {
		ret = walk_down_tree(trans, root, path, wc);
		if (ret < 0)
			break;

		ret = walk_up_tree(trans, root, path, wc, parent_level);
		if (ret) {
			if (ret > 0)
				ret = 0;
			break;
		}
	}

	kfree(wc);
	return ret;
}

/*
 * Unpin the extent range in an error context and don't add the space back.
 * Errors are not propagated further.
 */
void btrfs_error_unpin_extent_range(struct btrfs_fs_info *fs_info, u64 start, u64 end)
{
	unpin_extent_range(fs_info, start, end, false);
}

/*
 * It used to be that old block groups would be left around forever.
 * Iterating over them would be enough to trim unused space.  Since we
 * now automatically remove them, we also need to iterate over unallocated
 * space.
 *
 * We don't want a transaction for this since the discard may take a
 * substantial amount of time.  We don't require that a transaction be
 * running, but we do need to take a running transaction into account
 * to ensure that we're not discarding chunks that were released or
 * allocated in the current transaction.
 *
 * Holding the chunks lock will prevent other threads from allocating
 * or releasing chunks, but it won't prevent a running transaction
 * from committing and releasing the memory that the pending chunks
 * list head uses.  For that, we need to take a reference to the
 * transaction and hold the commit root sem.  We only need to hold
 * it while performing the free space search since we have already
 * held back allocations.
 */
static int btrfs_trim_free_extents(struct btrfs_device *device, u64 *trimmed)
{
	u64 start = BTRFS_DEVICE_RANGE_RESERVED, len = 0, end = 0;
	int ret;

	*trimmed = 0;

	/* Discard not supported = nothing to do. */
	if (!bdev_max_discard_sectors(device->bdev))
		return 0;

	/* Not writable = nothing to do. */
	if (!test_bit(BTRFS_DEV_STATE_WRITEABLE, &device->dev_state))
		return 0;

	/* No free space = nothing to do. */
	if (device->total_bytes <= device->bytes_used)
		return 0;

	ret = 0;

	while (1) {
		struct btrfs_fs_info *fs_info = device->fs_info;
		u64 bytes;

		ret = mutex_lock_interruptible(&fs_info->chunk_mutex);
		if (ret)
			break;

		btrfs_find_first_clear_extent_bit(&device->alloc_state, start,
						  &start, &end,
						  CHUNK_TRIMMED | CHUNK_ALLOCATED);

		/* Check if there are any CHUNK_* bits left */
		if (start > device->total_bytes) {
			DEBUG_WARN();
			btrfs_warn(fs_info,
"ignoring attempt to trim beyond device size: offset %llu length %llu device %s device size %llu",
					  start, end - start + 1,
					  btrfs_dev_name(device),
					  device->total_bytes);
			mutex_unlock(&fs_info->chunk_mutex);
			ret = 0;
			break;
		}

		/* Ensure we skip the reserved space on each device. */
		start = max_t(u64, start, BTRFS_DEVICE_RANGE_RESERVED);

		/*
		 * If find_first_clear_extent_bit find a range that spans the
		 * end of the device it will set end to -1, in this case it's up
		 * to the caller to trim the value to the size of the device.
		 */
		end = min(end, device->total_bytes - 1);

		len = end - start + 1;

		/* We didn't find any extents */
		if (!len) {
			mutex_unlock(&fs_info->chunk_mutex);
			ret = 0;
			break;
		}

		ret = btrfs_issue_discard(device->bdev, start, len,
					  &bytes);
		if (!ret)
			btrfs_set_extent_bit(&device->alloc_state, start,
					     start + bytes - 1, CHUNK_TRIMMED, NULL);
		mutex_unlock(&fs_info->chunk_mutex);

		if (ret)
			break;

		start += len;
		*trimmed += bytes;

		if (btrfs_trim_interrupted()) {
			ret = -ERESTARTSYS;
			break;
		}

		cond_resched();
	}

	return ret;
}

/*
 * Trim the whole filesystem by:
 * 1) trimming the free space in each block group
 * 2) trimming the unallocated space on each device
 *
 * This will also continue trimming even if a block group or device encounters
 * an error.  The return value will be the last error, or 0 if nothing bad
 * happens.
 */
int btrfs_trim_fs(struct btrfs_fs_info *fs_info, struct fstrim_range *range)
{
	struct btrfs_fs_devices *fs_devices = fs_info->fs_devices;
	struct btrfs_block_group *cache = NULL;
	struct btrfs_device *device;
	u64 group_trimmed;
	u64 range_end = U64_MAX;
	u64 start;
	u64 end;
	u64 trimmed = 0;
	u64 bg_failed = 0;
	u64 dev_failed = 0;
	int bg_ret = 0;
	int dev_ret = 0;
	int ret = 0;

	if (range->start == U64_MAX)
		return -EINVAL;

	/*
	 * Check range overflow if range->len is set.
	 * The default range->len is U64_MAX.
	 */
	if (range->len != U64_MAX &&
	    check_add_overflow(range->start, range->len, &range_end))
		return -EINVAL;

	cache = btrfs_lookup_first_block_group(fs_info, range->start);
	for (; cache; cache = btrfs_next_block_group(cache)) {
		if (cache->start >= range_end) {
			btrfs_put_block_group(cache);
			break;
		}

		start = max(range->start, cache->start);
		end = min(range_end, cache->start + cache->length);

		if (end - start >= range->minlen) {
			if (!btrfs_block_group_done(cache)) {
				ret = btrfs_cache_block_group(cache, true);
				if (ret) {
					bg_failed++;
					bg_ret = ret;
					continue;
				}
			}
			ret = btrfs_trim_block_group(cache,
						     &group_trimmed,
						     start,
						     end,
						     range->minlen);

			trimmed += group_trimmed;
			if (ret) {
				bg_failed++;
				bg_ret = ret;
				continue;
			}
		}
	}

	if (bg_failed)
		btrfs_warn(fs_info,
			"failed to trim %llu block group(s), last error %d",
			bg_failed, bg_ret);

	mutex_lock(&fs_devices->device_list_mutex);
	list_for_each_entry(device, &fs_devices->devices, dev_list) {
		if (test_bit(BTRFS_DEV_STATE_MISSING, &device->dev_state))
			continue;

		ret = btrfs_trim_free_extents(device, &group_trimmed);

		trimmed += group_trimmed;
		if (ret) {
			dev_failed++;
			dev_ret = ret;
			break;
		}
	}
	mutex_unlock(&fs_devices->device_list_mutex);

	if (dev_failed)
		btrfs_warn(fs_info,
			"failed to trim %llu device(s), last error %d",
			dev_failed, dev_ret);
	range->len = trimmed;
	if (bg_ret)
		return bg_ret;
	return dev_ret;
}
