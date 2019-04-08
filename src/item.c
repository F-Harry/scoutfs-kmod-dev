/*
 * Copyright (C) 2016 Versity Software, Inc.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License v2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/rbtree_augmented.h>

#include "super.h"
#include "format.h"
#include "kvec.h"
#include "manifest.h"
#include "item.h"
#include "seg.h"
#include "counters.h"
#include "scoutfs_trace.h"
#include "trans.h"

/*
 * A simple rbtree of cached items isolates the item API callers from
 * the relatively expensive segment searches.
 *
 * The item cache uses an rbtree of key ranges to record regions of keys
 * that are completely described by the items.  This lets it return
 * negative lookups cache hits for items that don't exist without having
 * to constantly perform expensive segment searches.
 *
 * Deletions of persistent items are recorded with items in the rbtree
 * which record the key of the deletion.  They're removed once they're
 * written to a level0 segment.  While they're present in the cache we
 * have to be careful to clobber them in creation and skip them in
 * lookups.  We only need deletion items for keys that exist in
 * segments.  We can immediately free non-persistent items when they're
 * deleted.
 */

static bool invalid_key_val(struct scoutfs_key *key, struct kvec *val)
{
	return WARN_ON_ONCE(val && (val->iov_len > SCOUTFS_MAX_VAL_SIZE));
}

struct item_cache {
	struct super_block *sb;

	spinlock_t lock;
	struct rb_root items;
	struct rb_root ranges;

	long nr_dirty_items;
	long dirty_val_bytes;

	struct shrinker shrinker;
	struct list_head lru_list;
	unsigned long lru_nr;
};

/*
 * The dirty bits track if the given item is dirty and if its child
 * subtrees contain any dirty items.
 *
 * The entry list_head typically stores clean items on an lru for shrinking.
 * It's also briefly used to track items in a batch after they're
 * allocated but before they're inserted for the first time.
 *
 * The persistent bit indicates that the item's key is present in
 * segments.   If we delete persistent items we have to write a deletion
 * item to delete to remove the existing item.  We can free deleted items
 * that aren't persistent without writing them.
 */
struct cached_item {
	struct rb_node node;
	struct list_head entry;

	long dirty;
	unsigned deletion:1,
		 persistent:1;

	struct scoutfs_key key;
	void *val;
	unsigned int val_len;
};

struct cached_range {
	struct rb_node node;

	struct scoutfs_key start;
	struct scoutfs_key end;
};

#define trace_range(which, sb, rng) \
	trace_scoutfs_item_range_##which(sb, (rng), &(rng)->start, &(rng)->end)

static u8 item_flags(struct cached_item *item)
{
	return item->deletion ? SCOUTFS_ITEM_FLAG_DELETION : 0;
}

static void free_item(struct super_block *sb, struct cached_item *item)
{
	if (!IS_ERR_OR_NULL(item)) {
		scoutfs_inc_counter(sb, item_free);
		WARN_ON_ONCE(!list_empty(&item->entry));
		WARN_ON_ONCE(!RB_EMPTY_NODE(&item->node));
		kfree(item->val);
		kfree(item);
	}
}

/*
 * The value vec may be null if the item has no value.  Values are
 * allocated separately so that we can free them when deleting or swap
 * them in place when updating items.
 */
static struct cached_item *alloc_item(struct super_block *sb,
				      struct scoutfs_key *key,
				      struct kvec *val)
{
	struct cached_item *item;

	item = kzalloc(sizeof(struct cached_item), GFP_NOFS);
	if (!item)
		goto out;

	item->key = *key;
	RB_CLEAR_NODE(&item->node);
	INIT_LIST_HEAD(&item->entry);

	if (val) {
		item->val = kmalloc(val->iov_len, GFP_NOFS);
		if (!item->val) {
			free_item(sb, item);
			item = NULL;
			goto out;
		}
		item->val_len = val->iov_len;
		memcpy(item->val, val->iov_base, val->iov_len);
	}

	scoutfs_inc_counter(sb, item_alloc);
out:
	return item;
}

/*
 * Copy the cached item's value into the caller's single value vector.
 * The number of bytes that fit in the vec and were copied is returned.
 * A null val returns 0.
 */
static int copy_item_val(struct kvec *val, struct cached_item *item)
{
	int ret;

	if (val) {
		ret = min_t(size_t, item->val_len, val->iov_len);
		memcpy(val->iov_base, item->val, ret);
	} else {
		ret = 0;
	}

	return ret;
}

/*
 * Walk the item rbtree and return the item found and the next and
 * prev items.
 */
static struct cached_item *walk_items(struct rb_root *root,
				      struct scoutfs_key *key,
				      struct cached_item **prev,
				      struct cached_item **next)
{
	struct rb_node *node = root->rb_node;
	struct cached_item *item;
	int cmp;

	*prev = NULL;
	*next = NULL;

	while (node) {
		item = container_of(node, struct cached_item, node);

		cmp = scoutfs_key_compare(key, &item->key);
		if (cmp < 0) {
			*next = item;
			node = node->rb_left;
		} else if (cmp > 0) {
			*prev = item;
			node = node->rb_right;
		} else {
			return item;
		}
	}

	return NULL;
}

/*
 * Look for the item with the given key.  Callers of this are looking
 * for existing items.  They would just return -ENOENT from a deletion
 * item if we gave it to them so we return null for deletion items.
 * Callers that would remove a deletion item before inserting a new
 * version of the item do so by having insert_item() replace existing
 * deleted items on their behalf.
 */
static struct cached_item *find_item(struct super_block *sb,
				     struct rb_root *root,
				     struct scoutfs_key *key)
{
	struct cached_item *prev;
	struct cached_item *next;
	struct cached_item *item;

	item = walk_items(root, key, &prev, &next);

	if (item && item->deletion)
		item = NULL;

	if (item)
		scoutfs_inc_counter(sb, item_lookup_hit);
	else
		scoutfs_inc_counter(sb, item_lookup_miss);

	return item;
}

static struct cached_item *next_item(struct rb_root *root,
				     struct scoutfs_key *key)
{
	struct cached_item *prev;
	struct cached_item *next;

	return walk_items(root, key, &prev, &next) ?: next;
}

static struct cached_item *prev_item(struct rb_root *root,
				     struct scoutfs_key *key)
{
	struct cached_item *prev;
	struct cached_item *next;

	return walk_items(root, key, &prev, &next) ?: prev;
}

/*
 * We store the dirty bits in a single value so that the simple
 * augmented rbtree implementation gets a single scalar value to compare
 * and store.
 */
#define ITEM_DIRTY 0x1
#define LEFT_DIRTY 0x2
#define RIGHT_DIRTY 0x4

static bool item_is_dirty(struct cached_item *item)
{
	return (item->dirty & ITEM_DIRTY) != 0;
}

/*
 * Return the given dirty bit if the item with the given node is dirty
 * or has dirty children.
 */
static long node_dirty_bit(struct rb_node *node, long dirty)
{
	struct cached_item *item;

	if (node) {
		item = container_of(node, struct cached_item, node);
		if (item->dirty)
			return dirty;
	}

	return 0;
}

static long compute_item_dirty(struct cached_item *item)
{
	return (item->dirty & ITEM_DIRTY) |
	       node_dirty_bit(item->node.rb_left, LEFT_DIRTY) |
	       node_dirty_bit(item->node.rb_right, RIGHT_DIRTY);
}

static void scoutfs_item_rb_propagate(struct rb_node *node,
				      struct rb_node *stop)
{
	struct cached_item *item;
	long dirty;

	while (node != stop) {
		item = container_of(node, struct cached_item, node);
		dirty = compute_item_dirty(item);

		if (item->dirty == dirty)
			break;

		item->dirty = dirty;
		node = rb_parent(&item->node);
	}
}

static void scoutfs_item_rb_copy(struct rb_node *old, struct rb_node *new)
{
	struct cached_item *n = container_of(new, struct cached_item, node);

	n->dirty = compute_item_dirty(n);
}

/* calculate the new parent last as it depends on the old parent */
static void scoutfs_item_rb_rotate(struct rb_node *old, struct rb_node *new)
{
	struct cached_item *o = container_of(old, struct cached_item, node);
	struct cached_item *n = container_of(new, struct cached_item, node);

	o->dirty = compute_item_dirty(o);
	n->dirty = compute_item_dirty(n);
}

/*
 * The generic RB_DECLARE_CALLBACKS() helpers are built for augmented
 * values that are simple commutative function of the left and right
 * children's augmented values.  During rotation the new parent just
 * gets the old parent's augmented value and then the old parent's value
 * is calculated.
 *
 * Our dirty bits don't work that way.  They are not just an or of the
 * child's bits, the bits depend on the left and right children
 * specifically.  During rotation both parents need to be specifically
 * recalculated.  (They could be masked and asigned based on the
 * direction of the rotation but that's annoying, let's just
 * recalculate.)
 */
static const struct rb_augment_callbacks scoutfs_item_rb_cb  = {
	.propagate = scoutfs_item_rb_propagate,
	.copy = scoutfs_item_rb_copy,
	.rotate = scoutfs_item_rb_rotate,
};

/*
 * The caller has changed an item's dirty bit.  Its child dirty bits are
 * still consistent.  But its parent's bits might need to be updated.
 * Its bits are consistent so we don't propagate from the node itself
 * because it would immediately terminate.
 */
static void update_dirty_parents(struct cached_item *item)
{
	scoutfs_item_rb_propagate(rb_parent(&item->node), NULL);
}

static void update_dirty_item_counts(struct super_block *sb, signed items,
				     signed vals)
{
	struct scoutfs_sb_info *sbi = SCOUTFS_SB(sb);
	struct item_cache *cac = sbi->item_cache;

	cac->nr_dirty_items += items;
	cac->dirty_val_bytes += vals;

	scoutfs_trans_track_item(sb, items, vals);
}

static void mark_item_dirty(struct super_block *sb, struct item_cache *cac,
			    struct cached_item *item)
{
	if (WARN_ON_ONCE(RB_EMPTY_NODE(&item->node)))
		return;

	if (item_is_dirty(item))
		return;

	item->dirty |= ITEM_DIRTY;
	list_del_init(&item->entry);
	cac->lru_nr--;

	update_dirty_item_counts(sb, 1, item->val_len);
	update_dirty_parents(item);
}

static void clear_item_dirty(struct super_block *sb, struct item_cache *cac,
			     struct cached_item *item)
{
	if (WARN_ON_ONCE(RB_EMPTY_NODE(&item->node)))
		return;

	if (!item_is_dirty(item))
		return;

	item->dirty &= ~ITEM_DIRTY;
	list_add_tail(&item->entry, &cac->lru_list);
	cac->lru_nr++;

	update_dirty_item_counts(sb, -1, -item->val_len);

	WARN_ON_ONCE(cac->nr_dirty_items < 0 || cac->dirty_val_bytes < 0);

	update_dirty_parents(item);
}

static void item_referenced(struct item_cache *cac, struct cached_item *item)
{
	if (!item->dirty)
		list_move_tail(&item->entry, &cac->lru_list);
}

/* remove the item from its tracking data structures */
static void unlink_item(struct super_block *sb, struct item_cache *cac,
		        struct cached_item *item)
{
	clear_item_dirty(sb, cac, item);
	rb_erase_augmented(&item->node, &cac->items, &scoutfs_item_rb_cb);
	RB_CLEAR_NODE(&item->node);
	if (!list_empty(&item->entry)) {
		list_del_init(&item->entry);
		cac->lru_nr--;
	}
}

/*
 * Safely erase an item from the tree.  Make sure to remove its dirty
 * accounting, use the augmented erase, and free it.
 */
static void erase_item(struct super_block *sb, struct item_cache *cac,
		       struct cached_item *item)
{
	trace_scoutfs_erase_item(sb, item);

	unlink_item(sb, cac, item);
	free_item(sb, item);
}

/*
 * Delete an item from the cache.  If it wasn't persistent we can just
 * free the item.  The caller must not try to use the item after calling
 * this.
 *
 * If it was persistent we have to write a deletion item so that
 * compaction will remove the old item.  We only need the key for the
 * deletion item so we can free the value.
 */
static void delete_item(struct super_block *sb, struct item_cache *cac,
			struct cached_item *item)
{
	if (!item->persistent) {
		erase_item(sb, cac, item);
		return;
	}

	/* uses val_len to update item accounting */
	clear_item_dirty(sb, cac, item);

	kfree(item->val);
	item->val = NULL;
	item->val_len = 0;

	item->deletion = 1;
	mark_item_dirty(sb, cac, item);
	scoutfs_inc_counter(sb, item_delete);
}

/*
 * Try to add an item to the cache.  The caller is responsible for
 * marking the newly inserted item dirty.
 *
 * We distinguish between callers seeing trying to insert a new logical
 * item and others trying to populate the cache.
 *
 * New logical item creators have made sure the items are participating
 * in consistent locking.  It's safe for them to clobber dirty deletion
 * items with a new version of the item.  The newly inserted item needs
 * to retain the persistence of the item it replaces so that if it is later
 * deleted it will still write a deletion item.
 *
 * Cache readers can only populate items that weren't present already.
 * In particular, they absolutely cannot replace dirty old inode index items
 * with the old version that was just deleted (outside of range caching and
 * locking consistency).
 */
static int insert_item(struct super_block *sb, struct item_cache *cac,
		       struct cached_item *ins, bool logical_overwrite,
		       bool cache_populate)
{
	struct rb_root *root = &cac->items;
	struct cached_item *item;
	struct rb_node *parent;
	struct rb_node **node;
	int cmp;

restart:
	node = &root->rb_node;
	parent = NULL;
	while (*node) {
		parent = *node;
		item = container_of(*node, struct cached_item, node);

		cmp = scoutfs_key_compare(&ins->key, &item->key);
		if (cmp < 0) {
			if (ins->dirty)
				item->dirty |= LEFT_DIRTY;
			node = &(*node)->rb_left;
		} else if (cmp > 0) {
			if (ins->dirty)
				item->dirty |= RIGHT_DIRTY;
			node = &(*node)->rb_right;
		} else {
			if (cache_populate ||
			    (!item->deletion && !logical_overwrite))
				return -EEXIST;

			/* sadly there's no augmented replace */
			erase_item(sb, cac, item);
			if (item->persistent)
				ins->persistent = 1;
			goto restart;
		}
	}

	trace_scoutfs_item_insertion(sb, &ins->key);

	rb_link_node(&ins->node, parent, node);
	rb_insert_augmented(&ins->node, root, &scoutfs_item_rb_cb);

	BUG_ON(item_is_dirty(ins));
	list_add_tail(&ins->entry, &cac->lru_list);
	cac->lru_nr++;

	return 0;
}

static struct cached_range *rb_first_rng(struct rb_root *root)
{
	struct rb_node *node;

	if ((node = rb_first(root)))
		return container_of(node, struct cached_range, node);

	return NULL;
}

static struct cached_range *rb_next_rng(struct cached_range *rng)
{
	struct rb_node *node;

	if (rng && (node = rb_next(&rng->node)))
		return container_of(node, struct cached_range, node);

	return NULL;
}

static struct cached_range *walk_ranges(struct rb_root *root,
					struct scoutfs_key *key,
					struct cached_range **prev,
					struct cached_range **next)
{
	struct rb_node *node = root->rb_node;
	struct cached_range *rng;
	int cmp;

	if (prev)
		*prev = NULL;
	if (next)
		*next = NULL;

	while (node) {
		rng = container_of(node, struct cached_range, node);

		cmp = scoutfs_key_compare_ranges(key, key,
						 &rng->start, &rng->end);
		if (cmp < 0) {
			if (next)
				*next = rng;
			node = node->rb_left;
		} else if (cmp > 0) {
			if (prev)
				*prev = rng;
			node = node->rb_right;
		} else {
			return rng;
		}
	}

	return NULL;
}

/*
 * Return true if the given key is covered by a cached range.  start and
 * end are set to the existing cached range.
 *
 * Return false if the key is not covered by a range.  start and end are
 * set to zero.  (Nothing uses these today, this is to avoid tracing
 * uninitialized keys in this case.)
 */
static bool check_range(struct super_block *sb, struct rb_root *root,
			struct scoutfs_key *key, struct scoutfs_key *start,
			struct scoutfs_key *end)
{
	struct scoutfs_sb_info *sbi = SCOUTFS_SB(sb);
	struct item_cache *cac = sbi->item_cache;
	struct cached_range *rng;

	rng = walk_ranges(&cac->ranges, key, NULL, NULL);
	if (rng) {
		scoutfs_inc_counter(sb, item_range_hit);
		if (start)
			*start = rng->start;
		if (end)
			*end = rng->end;
		return true;
	}

	if (start)
		scoutfs_key_set_zeros(start);
	if (end)
		scoutfs_key_set_zeros(end);

	scoutfs_inc_counter(sb, item_range_miss);
	return false;
}

static void free_range(struct super_block *sb, struct cached_range *rng)
{
	if (!IS_ERR_OR_NULL(rng)) {
		scoutfs_inc_counter(sb, item_range_free);
		trace_range(free, sb, rng);
		kfree(rng);
	}
}

/*
 * Insert a new cached range.  It might overlap with any number of
 * existing cached ranges.  As we descend we combine with and free any
 * overlapping ranges before restarting the descent.
 *
 * We're responsible for the ins allocation.  We free it if we don't
 * insert it in the tree.
 */
static void insert_range(struct super_block *sb, struct rb_root *root,
			 struct cached_range *ins)
{
	struct cached_range *rng;
	struct rb_node *parent;
	struct rb_node **node;
	int start_cmp;
	int end_cmp;
	int cmp;

	scoutfs_inc_counter(sb, item_range_insert);

restart:
	parent = NULL;
	node = &root->rb_node;
	while (*node) {
		parent = *node;
		rng = container_of(*node, struct cached_range, node);

		cmp = scoutfs_key_compare_ranges(&ins->start, &ins->end,
						 &rng->start, &rng->end);
		/* simple iteration until we overlap */
		if (cmp < 0) {
			node = &(*node)->rb_left;
			continue;
		} else if (cmp > 0) {
			node = &(*node)->rb_right;
			continue;
		}

		start_cmp = scoutfs_key_compare(&ins->start, &rng->start);
		end_cmp = scoutfs_key_compare(&ins->end, &rng->end);

		/* free our insertion if we're entirely within an existing */
		if (start_cmp >= 0 && end_cmp <= 0) {
			free_range(sb, ins);
			return;
		}

		/* expand to cover partial overlap before freeing */
		if (start_cmp < 0 && end_cmp < 0)
			swap(ins->end, rng->end);
		else if (start_cmp > 0 && end_cmp > 0)
			swap(ins->start, rng->start);

		/* remove and free all overlaps and restart the descent */
		rb_erase(&rng->node, root);
		free_range(sb, rng);
		goto restart;
	}

	trace_range(ins_rb_insert, sb, ins);
	rb_link_node(&ins->node, parent, node);
	rb_insert_color(&ins->node, root);
}

/*
 * Remove a given cached range.  The caller has already removed all the
 * items that fell within the range.  There can be any number of
 * existing cached ranges that overlap with the range that should be
 * removed.
 *
 * The caller's range has full precision keys that specify the endpoints
 * that will not be considered cached.  If we use them to set the new
 * bounds of existing ranges then we have to dec/inc them into the range
 * to have them represent the last/first valid key, not the first/last
 * key to be removed.
 *
 * Like insert_, we're responsible for freeing the caller's range.  We
 * might insert it into the tree to track the other half of a range
 * that's split by the removal.
 */
static void remove_range(struct super_block *sb, struct rb_root *root,
			 struct cached_range *rem)
{
	struct cached_range *rng;
	struct rb_node *parent;
	struct rb_node **node;
	bool insert = false;
	int start_cmp;
	int end_cmp;
	int cmp;

restart:
	parent = NULL;
	node = &root->rb_node;
	while (*node) {
		parent = *node;
		rng = container_of(*node, struct cached_range, node);

		cmp = scoutfs_key_compare_ranges(&rem->start, &rem->end,
						 &rng->start, &rng->end);
		/* simple iteration until we overlap */
		if (cmp < 0) {
			node = &(*node)->rb_left;
			continue;
		} else if (cmp > 0) {
			node = &(*node)->rb_right;
			continue;
		}

		start_cmp = scoutfs_key_compare(&rem->start, &rng->start);
		end_cmp = scoutfs_key_compare(&rem->end, &rng->end);

		/* remove the middle of an existing range, insert other half */
		if (start_cmp > 0 && end_cmp < 0) {
			swap(rng->end, rem->start);
			scoutfs_key_dec(&rng->end);
			trace_range(remove_mid_left, sb, rng);

			swap(rem->start, rem->end);
			scoutfs_key_inc(&rem->start);
			insert = true;
			goto restart;
		}

		/* remove partial overlap from existing */
		if (start_cmp < 0 && end_cmp < 0) {
			swap(rem->end, rng->start);
			scoutfs_key_inc(&rng->start);
			trace_range(remove_start, sb, rng);
			continue;
		}

		if (start_cmp > 0 && end_cmp > 0) {
			swap(rem->start, rng->end);
			scoutfs_key_dec(&rng->end);
			trace_range(remove_end, sb, rng);
			continue;
		}

		/* erase and free existing surrounded by removal */
		rb_erase(&rng->node, root);
		free_range(sb, rng);
		goto restart;
	}

	if (insert) {
		trace_range(rem_rb_insert, sb, rem);
		rb_link_node(&rem->node, parent, node);
		rb_insert_color(&rem->node, root);
	} else {
		free_range(sb, rem);
	}
}

/* Return true if the lock protects the use of the key. */
static bool lock_coverage(struct scoutfs_lock *lock,
			  struct scoutfs_key *key, int op_mode)
{
	signed char mode = ACCESS_ONCE(lock->mode);

	return ((op_mode == mode) ||
		(op_mode == SCOUTFS_LOCK_READ &&
		 mode == SCOUTFS_LOCK_WRITE)) &&
		scoutfs_key_compare_ranges(key, key,
					   &lock->start, &lock->end) == 0;
}

/*
 * Find an item with the given key and copy its value into the caller's
 * value vector.  The amount of bytes copied is returned which can be 0
 * or truncated if the caller's buffer isn't big enough or if val is null.
 *
 * The end key limits how many keys after the search key can be read
 * and inserted into the cache.
 */
int scoutfs_item_lookup(struct super_block *sb, struct scoutfs_key *key,
			struct kvec *val, struct scoutfs_lock *lock)
{
	struct scoutfs_sb_info *sbi = SCOUTFS_SB(sb);
	struct item_cache *cac = sbi->item_cache;
	struct cached_item *item;
	unsigned long flags;
	int ret;

	if (WARN_ON_ONCE(!lock_coverage(lock, key, SCOUTFS_LOCK_READ)))
		return -EINVAL;

	trace_scoutfs_item_lookup(sb, key);

	do {
		spin_lock_irqsave(&cac->lock, flags);

		item = find_item(sb, &cac->items, key);
		if (item) {
			item_referenced(cac, item);
			if (val)
				ret = copy_item_val(val, item);
			else
				ret = 0;
		} else if (check_range(sb, &cac->ranges, key, NULL, NULL)) {
			ret = -ENOENT;
		} else {
			ret = -ENODATA;
		}

		spin_unlock_irqrestore(&cac->lock, flags);

	} while (ret == -ENODATA &&
		 (ret = scoutfs_manifest_read_items(sb, key, &lock->start,
						    &lock->end)) == 0);

	trace_scoutfs_item_lookup_ret(sb, ret);
	return ret;
}

/*
 * This requires that the item at the specified key has a value of the
 * same length as the caller's value buffer.  Callers are asserting that
 * mismatched size are corruption so it returns -EIO if the sizes don't
 * match.  This isn't the fast path so we don't mind the copying
 * overhead that comes from only detecting the size mismatch after the
 * copy by reusing the more permissive _lookup().
 *
 * The end key limits how many keys after the search key can be read and
 * inserted into the cache.
 *
 * Returns 0 or -errno.
 */
int scoutfs_item_lookup_exact(struct super_block *sb,
			      struct scoutfs_key *key, struct kvec *val,
			      struct scoutfs_lock *lock)
{
	int ret;

	ret = scoutfs_item_lookup(sb, key, val, lock);
	if (ret == val->iov_len)
		ret = 0;
	else if (ret >= 0)
		ret = -EIO;

	return ret;
}

/*
 * Return the next linked node in the tree that isn't a deletion item
 * and which is still within the last allowed key value.
 */
static struct cached_item *next_item_node(struct rb_root *root,
					  struct cached_item *item,
					  struct scoutfs_key *last)
{
	struct rb_node *node;

	while (item) {
		node = rb_next(&item->node);
		if (!node) {
			item = NULL;
			break;
		}

		item = container_of(node, struct cached_item, node);

		if (scoutfs_key_compare(&item->key, last) > 0) {
			item = NULL;
			break;
		}

		if (!item->deletion)
			break;
	}

	return item;
}

/*
 * Find the next item to return from the "_next" item interface.  It's the
 * next item from the key that isn't a deletion item and is within the
 * bounds of the end of the cache and the caller's last key.
 */
static struct cached_item *item_for_next(struct rb_root *root,
				         struct scoutfs_key *key,
					 struct scoutfs_key *range_end,
					 struct scoutfs_key *last)
{
	struct cached_item *item;

	/* limit by the lesser of the two */
	if (range_end && scoutfs_key_compare(range_end, last) < 0)
		last = range_end;

	item = next_item(root, key);
	if (item) {
		if (scoutfs_key_compare(&item->key, last) > 0)
			item = NULL;
		else if (item->deletion)
			item = next_item_node(root, item, last);
	}

	return item;
}

/*
 * Return the next item starting with the given key and returning the
 * last key at most.
 *
 * The range covered by the lock also limits the last item that can be
 * returned.  -ENOENT can be returned when there are no next items
 * covered by the lock but there are still items before the last key
 * outside of the lock.  The caller needs to know to reacquire the next
 * lock to continue iteration.
 *
 * -ENOENT is returned if there are no items between the given and last
 * keys inside the range covered by the lock.
 *
 * The next item's key is copied to the caller's key.  The caller is
 * responsible for dealing with key lengths and truncation.
 *
 * The next item's value is copied into the callers value.  The number
 * of value bytes copied is returned.  The copied value can be truncated
 * by the caller's value buffer length.
 */
int scoutfs_item_next(struct super_block *sb, struct scoutfs_key *key,
		      struct scoutfs_key *last, struct kvec *val,
		      struct scoutfs_lock *lock)
{
	struct scoutfs_sb_info *sbi = SCOUTFS_SB(sb);
	struct item_cache *cac = sbi->item_cache;
	struct scoutfs_key pos;
	struct scoutfs_key range_end;
	struct cached_item *item;
	unsigned long flags;
	bool cached;
	int ret;

	/* use the end key as the last key if it's closer to reduce compares */
	if (scoutfs_key_compare(&lock->end, last) < 0)
		last = &lock->end;

	/* convenience to avoid searching if caller iterates past their last */
	if (scoutfs_key_compare(key, last) > 0) {
		ret = -ENOENT;
		goto out;
	}

	if (WARN_ON_ONCE(!lock_coverage(lock, key, SCOUTFS_LOCK_READ))) {
		ret = -EINVAL;
		goto out;
	}

	pos = *key;

	spin_lock_irqsave(&cac->lock, flags);

	for(;;) {
		/* see if we have cache coverage of our iterator pos */
		cached = check_range(sb, &cac->ranges, &pos, NULL, &range_end);

		trace_scoutfs_item_next_range_check(sb, !!cached, key,
						    &pos, last, &lock->end,
						    &range_end);

		if (!cached) {
			/* populate missing cached range starting at pos */
			spin_unlock_irqrestore(&cac->lock, flags);

			ret = scoutfs_manifest_read_items(sb, &pos,
							  &lock->start,
							  &lock->end);

			spin_lock_irqsave(&cac->lock, flags);
			if (ret)
				break;
			else
				continue;
		}

		/* see if there's an item in the cached range from pos */
		item = item_for_next(&cac->items, &pos, &range_end, last);
		if (!item) {
			if (scoutfs_key_compare(&range_end, last) < 0) {
				/* keep searching after empty cached range */
				pos = range_end;
				scoutfs_key_inc(&pos);
				continue;
			}

			/* no item and cache covers last, done */
			ret = -ENOENT;
			break;
		}

		/* we have a next item inside the cached range, done */
		*key = item->key;
		if (val) {
			item_referenced(cac, item);
			ret = copy_item_val(val, item);
		} else {
			ret = 0;
		}
		break;
	}

	spin_unlock_irqrestore(&cac->lock, flags);
out:

	trace_scoutfs_item_next_ret(sb, ret);
	return ret;
}

/*
 * Return the prev linked node in the tree that isn't a deletion item
 * and which is still within the first allowed key value.
 */
static struct cached_item *prev_item_node(struct rb_root *root,
					  struct cached_item *item,
					  struct scoutfs_key *first)
{
	struct rb_node *node;

	while (item) {
		node = rb_prev(&item->node);
		if (!node) {
			item = NULL;
			break;
		}

		item = container_of(node, struct cached_item, node);

		if (scoutfs_key_compare(&item->key, first) < 0) {
			item = NULL;
			break;
		}

		if (!item->deletion)
			break;
	}

	return item;
}

/*
 * Find the prev item to return from the "_prev" item interface.  It's the
 * prev item from the key that isn't a deletion item and is within the
 * bounds of the start of the cache and the caller's first key.
 */
static struct cached_item *item_for_prev(struct rb_root *root,
				         struct scoutfs_key *key,
					 struct scoutfs_key *range_start,
					 struct scoutfs_key *first)
{
	struct cached_item *item;

	/* limit by the greater of the two */
	if (range_start && scoutfs_key_compare(range_start, first) > 0)
		first = range_start;

	item = prev_item(root, key);
	if (item) {
		if (scoutfs_key_compare(&item->key, first) < 0)
			item = NULL;
		else if (item->deletion)
			item = prev_item_node(root, item, first);
	}

	return item;
}

/*
 * Return the prev item starting with the given key and returning the
 * first key at least.
 *
 * The range covered by the lock also limits the first item that can be
 * returned.  -ENOENT can be returned when there are no prev items
 * covered by the lock but there are still items after the first key
 * outside of the lock.  The caller needs to know to reacquire the next
 * lock to continue iteration.
 *
 * -ENOENT is returned if there are no items between the given and
 * first key inside the range covered by the lock.
 *
 * The prev item's key is copied to the caller's key.  The caller is
 * responsible for dealing with key lengths and truncation.
 *
 * The prev item's value is copied into the callers value.  The number
 * of value bytes copied is returned.  The copied value can be truncated
 * by the caller's value buffer length.
 */
int scoutfs_item_prev(struct super_block *sb, struct scoutfs_key *key,
		      struct scoutfs_key *first, struct kvec *val,
		      struct scoutfs_lock *lock)
{
	struct scoutfs_sb_info *sbi = SCOUTFS_SB(sb);
	struct item_cache *cac = sbi->item_cache;
	struct scoutfs_key range_start;
	struct scoutfs_key pos;
	struct cached_item *item;
	unsigned long flags;
	bool cached;
	int ret;

	/* use the start key as the first key if it's closer */
	if (scoutfs_key_compare(&lock->start, first) > 0)
		first = &lock->start;

	/* convenience to avoid searching if caller iterates past their last */
	if (scoutfs_key_compare(key, first) < 0) {
		ret = -ENOENT;
		goto out;
	}

	if (WARN_ON_ONCE(!lock_coverage(lock, key, SCOUTFS_LOCK_READ))) {
		ret = -EINVAL;
		goto out;
	}

	pos = *key;

	spin_lock_irqsave(&cac->lock, flags);

	for(;;) {
		/* see if we have cache coverage of our iterator pos */
		cached = check_range(sb, &cac->ranges, &pos,
				     &range_start, NULL);

		trace_scoutfs_item_prev_range_check(sb, !!cached, key,
						    &pos, first, &lock->start,
						    &range_start);

		if (!cached) {
			/* populate missing cached range starting at pos */
			spin_unlock_irqrestore(&cac->lock, flags);

			ret = scoutfs_manifest_read_items(sb, &pos,
							  &lock->start,
							  &lock->end);

			spin_lock_irqsave(&cac->lock, flags);
			if (ret)
				break;
			else
				continue;
		}

		/* see if there's an item in the cached range from pos */
		item = item_for_prev(&cac->items, &pos, &range_start, first);
		if (!item) {
			if (scoutfs_key_compare(&range_start, first) > 0) {
				/* keep searching before empty cached range */
				pos = range_start;
				scoutfs_key_dec(&pos);
				continue;
			}

			/* no item and cache covers first, done */
			ret = -ENOENT;
			break;
		}

		/* we have a prev item inside the cached range, done */
		*key = item->key;
		if (val) {
			item_referenced(cac, item);
			ret = copy_item_val(val, item);
		} else {
			ret = 0;
		}
		break;
	}

	spin_unlock_irqrestore(&cac->lock, flags);
out:

	trace_scoutfs_item_prev_ret(sb, ret);
	return ret;
}

/*
 * Create a new dirty item in the cache.  Returns -EEXIST if an item
 * already exists with the given key.
 */
int scoutfs_item_create(struct super_block *sb, struct scoutfs_key *key,
			struct kvec *val, struct scoutfs_lock *lock)
{
	struct scoutfs_sb_info *sbi = SCOUTFS_SB(sb);
	struct item_cache *cac = sbi->item_cache;
	struct cached_item *item;
	unsigned long flags;
	int ret;

	if (invalid_key_val(key, val) ||
	    WARN_ON_ONCE(!lock_coverage(lock, key, SCOUTFS_LOCK_WRITE))) {
		ret = -EINVAL;
		goto out;
	}

	item = alloc_item(sb, key, val);
	if (!item) {
		ret = -ENOMEM;
		goto out;
	}

	do {
		spin_lock_irqsave(&cac->lock, flags);

		if (!check_range(sb, &cac->ranges, key, NULL, NULL)) {
			ret = -ENODATA;
		} else {
			ret = insert_item(sb, cac, item, false, false);
			if (!ret) {
				scoutfs_inc_counter(sb, item_create);
				mark_item_dirty(sb, cac, item);
			}
		}

		spin_unlock_irqrestore(&cac->lock, flags);

	} while (ret == -ENODATA &&
		 (ret = scoutfs_manifest_read_items(sb, key, &lock->start,
						    &lock->end)) == 0);

	if (ret)
		free_item(sb, item);

out:
	trace_scoutfs_item_create(sb, key, ret);
	return ret;
}

/*
 * "force" an item creation without first reading to see if the item
 * exist.  The caller is asserting that they know it's correct to
 * overwrite a possibly existing item with this newly created item.
 *
 * Because this can be overwriting an existing item we need to be sure
 * that we write a deletion item if it's deleted so we force its
 * persistent flag.
 */
int scoutfs_item_create_force(struct super_block *sb,
			      struct scoutfs_key *key,
			      struct kvec *val, struct scoutfs_lock *lock)
{
	struct scoutfs_sb_info *sbi = SCOUTFS_SB(sb);
	struct item_cache *cac = sbi->item_cache;
	struct cached_item *item;
	unsigned long flags;
	int ret;

	if (invalid_key_val(key, val))
		return -EINVAL;

	if (WARN_ON_ONCE(!lock_coverage(lock, key, SCOUTFS_LOCK_WRITE_ONLY)))
		return -EINVAL;

	item = alloc_item(sb, key, val);
	if (!item)
		return -ENOMEM;

	item->persistent = 1;

	spin_lock_irqsave(&cac->lock, flags);

	ret = insert_item(sb, cac, item, true, false);
	if (ret) {
		printk(KERN_EMERG "Scoutfs: corrupted item cache found while"
		       " creating item "SK_FMT" on fs %llu\n", SK_ARG(key),
		       le64_to_cpu(SCOUTFS_SB(sb)->super.hdr.fsid));
		BUG_ON(ret);
	}
	scoutfs_inc_counter(sb, item_create);
	mark_item_dirty(sb, cac, item);

	spin_unlock_irqrestore(&cac->lock, flags);

	if (ret)
		free_item(sb, item);

	return ret;
}

/*
 * Allocate an item with the key and value and add it to the list of
 * items to be inserted as a batch later.  The caller adds in sort order
 * and we add with _tail to maintain that order.
 */
int scoutfs_item_add_batch(struct super_block *sb, struct list_head *list,
			   struct scoutfs_key *key, struct kvec *val)
{
	struct cached_item *item;
	int ret;

	if (invalid_key_val(key, val))
		return -EINVAL;

	item = alloc_item(sb, key, val);
	if (item) {
		list_add_tail(&item->entry, list);
		ret = 0;
	} else {
		ret = -ENOMEM;
	}

	return ret;
}


/*
 * Insert a batch of clean read items from segments into the item cache.
 *
 * The caller hasn't been locked so the cached items could have changed
 * since they were asked to read.  If there are duplicates in the item
 * cache they might be newer than what was read so we must drop them on
 * the floor.
 *
 * The batch atomically adds the items and updates the cached range to
 * include the callers range that covers the items.
 *
 * It's safe to re-add items to the batch list after they aren't
 * inserted because _safe iteration will always be past the head entry
 * that will be inserted.
 */
int scoutfs_item_insert_batch(struct super_block *sb, struct list_head *list,
			      struct scoutfs_key *start,
			      struct scoutfs_key *end)
{
	struct scoutfs_sb_info *sbi = SCOUTFS_SB(sb);
	struct item_cache *cac = sbi->item_cache;
	struct cached_range *rng;
	struct cached_item *item;
	struct cached_item *tmp;
	unsigned long flags;
	int ret;

	trace_scoutfs_item_insert_batch(sb, start, end);

	if (WARN_ON_ONCE(scoutfs_key_compare(start, end) > 0))
		return -EINVAL;

	scoutfs_inc_counter(sb, item_range_alloc);
	rng = kzalloc(sizeof(struct cached_range), GFP_NOFS);
	if (!rng) {
		free_range(sb, rng);
		ret = -ENOMEM;
		goto out;
	}

	rng->start = *start;
	rng->end = *end;

	spin_lock_irqsave(&cac->lock, flags);

	insert_range(sb, &cac->ranges, rng);

	list_for_each_entry_safe(item, tmp, list, entry) {
		list_del_init(&item->entry);
		item->persistent = 1;
		if (insert_item(sb, cac, item, false, true)) {
			scoutfs_inc_counter(sb, item_batch_duplicate);
			list_add(&item->entry, list);
		} else {
			scoutfs_inc_counter(sb, item_batch_inserted);
		}
	}

	spin_unlock_irqrestore(&cac->lock, flags);

	ret = 0;
out:
	scoutfs_item_free_batch(sb, list);
	return ret;
}

void scoutfs_item_free_batch(struct super_block *sb, struct list_head *list)
{
	struct cached_item *item;
	struct cached_item *tmp;

	list_for_each_entry_safe(item, tmp, list, entry) {
		list_del_init(&item->entry);
		free_item(sb, item);
	}
}


/*
 * If the item exists make sure it's dirty and pinned.  It can be read
 * if it wasn't cached.  -ENOENT is returned if the item doesn't exist.
 */
int scoutfs_item_dirty(struct super_block *sb, struct scoutfs_key *key,
		       struct scoutfs_lock *lock)
{
	struct scoutfs_sb_info *sbi = SCOUTFS_SB(sb);
	struct item_cache *cac = sbi->item_cache;
	struct cached_item *item;
	unsigned long flags;
	int ret;

	if (WARN_ON_ONCE(!lock_coverage(lock, key, SCOUTFS_LOCK_WRITE)))
		return -EINVAL;

	do {
		spin_lock_irqsave(&cac->lock, flags);

		item = find_item(sb, &cac->items, key);
		if (item) {
			mark_item_dirty(sb, cac, item);
			ret = 0;
		} else if (check_range(sb, &cac->ranges, key, NULL, NULL)) {
			ret = -ENOENT;
		} else {
			ret = -ENODATA;
		}

		spin_unlock_irqrestore(&cac->lock, flags);

	} while (ret == -ENODATA &&
		 (ret = scoutfs_manifest_read_items(sb, key, &lock->start,
						    &lock->end)) == 0);

	trace_scoutfs_item_dirty_ret(sb, ret);
	return ret;
}

/*
 * Set the value of an existing item in the tree.  The item is marked dirty
 * and the previous value is freed.  The provided value may be null.
 *
 * Returns -ENOENT if the item doesn't exist.
 */
int scoutfs_item_update(struct super_block *sb, struct scoutfs_key *key,
			struct kvec *val, struct scoutfs_lock *lock)
{
	struct scoutfs_sb_info *sbi = SCOUTFS_SB(sb);
	struct item_cache *cac = sbi->item_cache;
	struct cached_item *item;
	unsigned long flags;
	void *up_val = NULL;
	int ret;

	if (invalid_key_val(key, val))
		return -EINVAL;

	if (WARN_ON_ONCE(!lock_coverage(lock, key, SCOUTFS_LOCK_WRITE)))
		return -EINVAL;

	if (val) {
		up_val = kmalloc(val->iov_len, GFP_NOFS);
		if (!up_val) {
			ret = -ENOMEM;
			goto out;
		}
		memcpy(up_val, val->iov_base, val->iov_len);
	}

	do {
		spin_lock_irqsave(&cac->lock, flags);

		item = find_item(sb, &cac->items, key);
		if (item) {
			clear_item_dirty(sb, cac, item);
			swap(up_val, item->val);
			item->val_len = val ? val->iov_len : 0;
			mark_item_dirty(sb, cac, item);
			ret = 0;
		} else if (check_range(sb, &cac->ranges, key, NULL, NULL)) {
			ret = -ENOENT;
		} else {
			ret = -ENODATA;
		}

		spin_unlock_irqrestore(&cac->lock, flags);

	} while (ret == -ENODATA &&
		 (ret = scoutfs_manifest_read_items(sb, key, &lock->start,
						    &lock->end)) == 0);
out:
	kfree(up_val);

	trace_scoutfs_item_update_ret(sb, ret);
	return ret;
}

/*
 * Delete an existing item with the given key.
 *
 * If a non-deletion item is present then we mark it dirty and deleted
 * and free it's value.
 *
 * Returns -ENOENT if an item doesn't exist at the key.  This forces us
 * to read the item before creating a deletion item for it.  XXX If we
 * relaxed this we'd need to see if callers make use of -ENOENT and if
 * there are any ways for userspace to overwhelm the system with
 * deletion items for items that didn't exist in the first place.
 */
int scoutfs_item_delete(struct super_block *sb, struct scoutfs_key *key,
			struct scoutfs_lock *lock)
{
	struct scoutfs_sb_info *sbi = SCOUTFS_SB(sb);
	struct item_cache *cac = sbi->item_cache;
	struct cached_item *item;
	unsigned long flags;
	int ret;

	if (WARN_ON_ONCE(!lock_coverage(lock, key, SCOUTFS_LOCK_WRITE))) {
		ret = -EINVAL;
		goto out;
	}

	do {
		spin_lock_irqsave(&cac->lock, flags);

		item = find_item(sb, &cac->items, key);
		if (item) {
			delete_item(sb, cac, item);
			ret = 0;
		} else if (check_range(sb, &cac->ranges, key, NULL, NULL)) {
			ret = -ENOENT;
		} else {
			ret = -ENODATA;
		}

		spin_unlock_irqrestore(&cac->lock, flags);

	} while (ret == -ENODATA &&
		 (ret = scoutfs_manifest_read_items(sb, key, &lock->start,
						    &lock->end)) == 0);

out:
	trace_scoutfs_item_delete(sb, key, ret);
	return ret;
}

/*
 * "force" a deletion by creating a deletion item without first reading
 * the existing item.
 *
 * The caller knows that there is an existing item but doesn't want to
 * pay the cost of reading it before writing a deletion item.  We mark
 * the allocated deletion item persistent to ensure that it's written.
 */
int scoutfs_item_delete_force(struct super_block *sb,
			      struct scoutfs_key *key,
			      struct scoutfs_lock *lock)
{
	struct scoutfs_sb_info *sbi = SCOUTFS_SB(sb);
	struct item_cache *cac = sbi->item_cache;
	struct cached_item *item;
	unsigned long flags;
	int ret;

	if (WARN_ON_ONCE(!lock_coverage(lock, key, SCOUTFS_LOCK_WRITE_ONLY)))
		return -EINVAL;

	item = alloc_item(sb, key, NULL);
	if (!item)
		return -ENOMEM;

	item->persistent = 1;

	spin_lock_irqsave(&cac->lock, flags);
	ret = insert_item(sb, cac, item, true, false);
	if (ret) {
		printk(KERN_EMERG "Scoutfs: corrupted item cache found while"
		       " deleting item "SK_FMT" on fs %llu\n", SK_ARG(key),
		       le64_to_cpu(SCOUTFS_SB(sb)->super.hdr.fsid));
		BUG_ON(ret);
	}
	scoutfs_inc_counter(sb, item_create);
	mark_item_dirty(sb, cac, item);

	delete_item(sb, cac, item);
	spin_unlock_irqrestore(&cac->lock, flags);

	return ret;
}

/*
 * Delete an item and give it to the caller so that they can restore it
 * later.
 *
 * The deleted items can be dirty or not.. we maintain an accurate dirty
 * count as we remove the deleted items and leave their dirty flag set
 * so that restore can mark them dirty again.
 *
 * Returns -ENOENT if the item didn't exist and couldn't be deleted.
 */
int scoutfs_item_delete_save(struct super_block *sb,
			     struct scoutfs_key *key,
			     struct list_head *list,
			     struct scoutfs_lock *lock)
{
	struct scoutfs_sb_info *sbi = SCOUTFS_SB(sb);
	struct item_cache *cac = sbi->item_cache;
	struct cached_item *item;
	struct cached_item *del;
	unsigned long flags;
	bool was_dirty;
	int ret;

	if (WARN_ON_ONCE(!lock_coverage(lock, key, SCOUTFS_LOCK_WRITE))) {
		ret = -EINVAL;
		goto out;
	}

	del = alloc_item(sb, key, NULL);
	if (!del) {
		ret = -ENOMEM;
		goto out;
	}

	do {
		spin_lock_irqsave(&cac->lock, flags);

		item = find_item(sb, &cac->items, key);
		if (item) {
			was_dirty = item_is_dirty(item);
			unlink_item(sb, cac, item);
			list_add_tail(&item->entry, list);
			if (was_dirty)
				item->dirty |= ITEM_DIRTY;

			del->persistent = item->persistent;
			ret = insert_item(sb, cac, del, false, false);
			BUG_ON(ret);
			delete_item(sb, cac, del);
			del = NULL;
			ret = 0;
		} else if (check_range(sb, &cac->ranges, key, NULL, NULL)) {
			ret = -ENOENT;
		} else {
			ret = -ENODATA;
		}

		spin_unlock_irqrestore(&cac->lock, flags);

	} while (ret == -ENODATA &&
		 (ret = scoutfs_manifest_read_items(sb, key, &lock->start,
						    &lock->end)) == 0);

	free_item(sb, del);
out:
	trace_scoutfs_item_delete_save(sb, key, ret);
	return ret;
}

/*
 * Restore a set of previousl saved items.  They're returned to the
 * cached and marked dirty if they were dirty when they were saved.
 * Restored items completely overwrite any existing cached items.
 *
 * The caller must have held locks covering the save and restore so that
 * the cached ranges still exist.
 */
int scoutfs_item_restore(struct super_block *sb, struct list_head *list,
			 struct scoutfs_lock *lock)
{
	struct scoutfs_sb_info *sbi = SCOUTFS_SB(sb);
	struct item_cache *cac = sbi->item_cache;
	struct cached_item *existing;
	struct cached_item *item;
	struct cached_item *tmp;
	unsigned long flags;
	bool was_dirty;
	int mode;
	int ret;

	if (list_empty(list))
		return 0;

	spin_lock_irqsave(&cac->lock, flags);

	/* make sure all the items are locked and cached */
	list_for_each_entry(item, list, entry) {
		mode = item_is_dirty(item) ? SCOUTFS_LOCK_WRITE :
					     SCOUTFS_LOCK_READ;
		if (WARN_ON_ONCE(!lock_coverage(lock, &item->key, mode)) ||
		    WARN_ON_ONCE(!check_range(sb, &cac->ranges, &item->key,
				 NULL, NULL))) {
			ret = -EINVAL;
			goto out;
		}
	}

	list_for_each_entry_safe(item, tmp, list, entry) {
		was_dirty = item_is_dirty(item);
		item->dirty &= ~ITEM_DIRTY;
		list_del_init(&item->entry);

		existing = find_item(sb, &cac->items, &item->key);
		if (existing)
			erase_item(sb, cac, existing);
		insert_item(sb, cac, item, false, false);
		if (was_dirty)
			mark_item_dirty(sb, cac, item);
	}

	ret = 0;
out:
	spin_unlock_irqrestore(&cac->lock, flags);

	return ret;
}

/*
 * Delete an item that the caller knows must be dirty because they hold
 * locks and the transaction and have created or dirtied it.  This can't
 * fail.
 */
void scoutfs_item_delete_dirty(struct super_block *sb,
			       struct scoutfs_key *key)
{
	struct scoutfs_sb_info *sbi = SCOUTFS_SB(sb);
	struct item_cache *cac = sbi->item_cache;
	struct cached_item *item;
	unsigned long flags;

	spin_lock_irqsave(&cac->lock, flags);

	item = find_item(sb, &cac->items, key);
	if (item)
		delete_item(sb, cac, item);

	spin_unlock_irqrestore(&cac->lock, flags);
}

/*
 * Copy the callers value into the dirty item and truncate its value if
 * the existing value is longer.  The caller must have ensured that the
 * item was dirty and had a large enough value.  If the updated value is
 * smaller then it will sit in the larger item allocation until the
 * value is eventually freed along with the item.
 */
void scoutfs_item_update_dirty(struct super_block *sb,
			       struct scoutfs_key *key, struct kvec *val)
{
	struct scoutfs_sb_info *sbi = SCOUTFS_SB(sb);
	struct item_cache *cac = sbi->item_cache;
	struct cached_item *item;
	unsigned long flags;
	unsigned int new_len = val ? val->iov_len : 0;
	signed delta;

	spin_lock_irqsave(&cac->lock, flags);

	item = find_item(sb, &cac->items, key);

	BUG_ON(!item || !item_is_dirty(item) || new_len > item->val_len);

	delta = new_len - item->val_len;
	if (val)
		memcpy(item->val, val->iov_base, new_len);
	item->val_len = new_len;
	update_dirty_item_counts(sb, 0, delta);

	spin_unlock_irqrestore(&cac->lock, flags);
}

/*
 * Return the first dirty node in the subtree starting at the given node.
 */
static struct cached_item *first_dirty(struct rb_node *node)
{
	struct cached_item *ret = NULL;
	struct cached_item *item;

	while (node) {
		item = container_of(node, struct cached_item, node);

		if (item->dirty & LEFT_DIRTY) {
			node = item->node.rb_left;
		} else if (item_is_dirty(item)) {
			ret = item;
			break;
		} else if (item->dirty & RIGHT_DIRTY) {
			node = item->node.rb_right;
		} else {
			break;
		}
	}

	return ret;
}

/*
 * Find the next dirty item after a given item.  First we see if we have
 * a dirty item in our right subtree.  If not we ascend through parents
 * skipping those that are less than us.  If we find a parent that's
 * greater than us then we see if it's dirty, if not we start the search
 * all over again by checking its right subtree then ascending.
 */
static struct cached_item *next_dirty(struct cached_item *item)
{
	struct rb_node *parent;
	struct rb_node *node;

	while (item) {
		if (item->dirty & RIGHT_DIRTY)
			return first_dirty(item->node.rb_right);

		/* find next greatest parent */
		node = &item->node;
		while ((parent = rb_parent(node)) && parent->rb_right == node)
			node = parent;
		if (!parent)
			break;

		/* done if our next greatest parent itself is dirty */
		item = container_of(parent, struct cached_item, node);
		if (item_is_dirty(item))
			return item;

		/* continue to check right subtree */
	}

	return NULL;
}

static bool dirty_item_within(struct rb_root *root,
			      struct scoutfs_key *from,
			      struct scoutfs_key *end)
{
	struct cached_item *item;

	item = next_item(root, from);
	if (item && !item_is_dirty(item))
		item = next_dirty(item);

	return item && scoutfs_key_compare(&item->key, end) <= 0;
}

bool scoutfs_item_has_dirty(struct super_block *sb)
{
	struct scoutfs_sb_info *sbi = SCOUTFS_SB(sb);
	struct item_cache *cac = sbi->item_cache;
	unsigned long flags;
	bool has;

	spin_lock_irqsave(&cac->lock, flags);
	has = cac->nr_dirty_items != 0;
	spin_unlock_irqrestore(&cac->lock, flags);

	return has;
}

/*
 * Return true if the item cache covers the given range.  If dirty is
 * provided then we only return true if there are dirty items in the
 * range.
 *
 * If the start of the query range doesn't overlap a cached range then
 * we see if the next cached range starts before the end of the query range.
 */
bool scoutfs_item_range_cached(struct super_block *sb,
			       struct scoutfs_key *start,
			       struct scoutfs_key *end, bool dirty)
{
	struct scoutfs_sb_info *sbi = SCOUTFS_SB(sb);
	struct item_cache *cac = sbi->item_cache;
	struct cached_range *next;
	struct cached_range *rng;
	unsigned long flags;
	bool cached = false;

	spin_lock_irqsave(&cac->lock, flags);

	if (dirty) {
		if (dirty_item_within(&cac->items, start, end))
			cached = true;
	} else {
		rng = walk_ranges(&cac->ranges, start, NULL, &next);
		if (rng ||
		    (next && scoutfs_key_compare(&next->start, end) <= 0))
			cached = true;
	}

	spin_unlock_irqrestore(&cac->lock, flags);

	return cached;
}

/*
 * Returns true if adding more items with the given count, keys, and values
 * still fits in a single item along with the current dirty items.
 */
bool scoutfs_item_dirty_fits_single(struct super_block *sb, u32 nr_items,
			            u32 val_bytes)
{
	struct scoutfs_sb_info *sbi = SCOUTFS_SB(sb);
	struct item_cache *cac = sbi->item_cache;
	unsigned long flags;
	bool fits;

	spin_lock_irqsave(&cac->lock, flags);
	fits = scoutfs_seg_fits_single(nr_items + cac->nr_dirty_items,
				       val_bytes + cac->dirty_val_bytes);
	spin_unlock_irqrestore(&cac->lock, flags);

	return fits;
}

/*
 * Fill the given segment with sorted dirty items.
 *
 * The caller is responsible for the consistency of the dirty items once
 * they're in its seg.  We can consider them clean once we store them.
 *
 * XXX this first/append pattern will go away once we can write a stream
 * of items to a segment without needing to know the item count to
 * find the starting key and value offsets.
 */
int scoutfs_item_dirty_seg(struct super_block *sb, struct scoutfs_segment *seg)
{
	struct scoutfs_sb_info *sbi = SCOUTFS_SB(sb);
	struct item_cache *cac = sbi->item_cache;
	__le32 *links[SCOUTFS_MAX_SKIP_LINKS];
	struct cached_item *item = NULL;
	struct cached_item *del;
	unsigned long flags;
	struct kvec val;
	bool appended;

	spin_lock_irqsave(&cac->lock, flags);

	item = first_dirty(cac->items.rb_node);
	while (item) {
		kvec_init(&val, item->val, item->val_len);
		appended = scoutfs_seg_append_item(sb, seg, &item->key, &val,
						   item_flags(item), links);
		/* trans reservation should have limited dirty */
		BUG_ON(!appended);

		if (item->deletion)
			scoutfs_inc_counter(sb, trans_write_deletion_item);
		else
			scoutfs_inc_counter(sb, trans_write_item);

		/* non-persistent should have been freed (safe to write) */
		WARN_ON_ONCE(item->deletion && !item->persistent);

		clear_item_dirty(sb, cac, item);
		item->persistent = 1;

		del = item;
		item = next_dirty(item);

		if (del->deletion)
			erase_item(sb, cac, del);
	}

	spin_unlock_irqrestore(&cac->lock, flags);

	return 0;
}

/*
 * The caller wants us to write out any dirty items within the given
 * range.  We look for any dirty items within the range and if we find
 * any we issue a sync which writes out all the dirty items.
 *
 * Returns a sync error or the number of dirty items written.
 */
int scoutfs_item_writeback(struct super_block *sb,
			   struct scoutfs_key *start,
			   struct scoutfs_key *end)
{
	struct scoutfs_sb_info *sbi = SCOUTFS_SB(sb);
	struct item_cache *cac = sbi->item_cache;
	unsigned long flags;
	bool sync = false;
	int count = 0;
	int ret = 0;

	/* XXX think about racing with trans write */

	spin_lock_irqsave(&cac->lock, flags);

	if (cac->nr_dirty_items && dirty_item_within(&cac->items, start, end)) {
		sync = true;
		count = cac->nr_dirty_items;
	}

	spin_unlock_irqrestore(&cac->lock, flags);

	if (sync) {
		scoutfs_inc_counter(sb, trans_commit_item_flush);
		ret = scoutfs_trans_sync(sb, 1);
	}

	return ret ?: count;
}

/*
 * The caller wants us to drop any items within the range on the floor.
 * They should have ensured that items in this range won't be dirty.
 *
 * Returns errors or the count of the items invalidated.
 */
int scoutfs_item_invalidate(struct super_block *sb,
			    struct scoutfs_key *start,
			    struct scoutfs_key *end)
{
	struct scoutfs_sb_info *sbi = SCOUTFS_SB(sb);
	struct item_cache *cac = sbi->item_cache;
	struct cached_range *rng;
	struct cached_item *next;
	struct cached_item *item;
	struct rb_node *node;
	unsigned long flags;
	int count = 0;
	int ret;

	trace_scoutfs_item_invalidate_range(sb, start, end);

	/* XXX think about racing with trans write */

	scoutfs_inc_counter(sb, item_range_alloc);
	rng = kzalloc(sizeof(struct cached_range), GFP_NOFS);
	if (!rng) {
		free_range(sb, rng);
		ret = -ENOMEM;
		goto out;
	}

	rng->start = *start;
	rng->end = *end;

	spin_lock_irqsave(&cac->lock, flags);

	for (item = next_item(&cac->items, start);
	     item && scoutfs_key_compare(&item->key, end) <= 0;
	     item = next) {

		/* XXX seems like this should be a helper? */
		node = rb_next(&item->node);
		if (node)
			next = container_of(node, struct cached_item, node);
		else
			next = NULL;

		WARN_ON_ONCE(item_is_dirty(item));
		erase_item(sb, cac, item);
		count++;
	}

	remove_range(sb, &cac->ranges, rng);

	spin_unlock_irqrestore(&cac->lock, flags);

	ret = 0;
out:
	return ret ?: count;
}

static struct cached_item *rb_next_item(struct cached_item *item)
{
	struct rb_node *node;

	if (item && (node = rb_next(&item->node)))
		return container_of(node, struct cached_item, node);

	return NULL;
}

static struct cached_item *rb_prev_item(struct cached_item *item)
{
	struct rb_node *node;

	if (item && (node = rb_prev(&item->node)))
		return container_of(node, struct cached_item, node);

	return NULL;
}

/*
 * Find the bounds of an item cache shrinking operation.  Starting from
 * an item, walk through either next items to the right or prev items to
 * the left.  Record items that are valid final shrinking points because
 * using their key for a new range end doesn't cross the remaining
 * existing item.  We stop if we check enough items, hit a dirty item,
 * or run out of items in the range.
 *
 * We only can't use an item as a new range end point if moving its key
 * crosses the next item in the cache.  This only happens when smaller
 * items share a prefix with the next larger item.  This only happens
 * for item populations with names (dirents, xattrs) that share
 * prefixes.  We really don't want to be unable to reclaim so we
 * aggressively try to walk past all of them.
 */
#define BOUNDARY_MIN 32
#define BOUNDARY_MAX 300
static struct cached_item *shrink_boundary(struct super_block *sb,
					   struct cached_item *item,
					   struct cached_item **next_ret,
					   struct scoutfs_key *end,
					   bool right)
{
	struct cached_item *found = NULL;
	struct cached_item *next;
	bool cmp;
	int i;

	*next_ret = NULL;

	for (i = 0; i < BOUNDARY_MAX; i++) {
		if (right)
			next = rb_next_item(item);
		else
			next = rb_prev_item(item);

		if (next) {
			if (right)
				cmp = scoutfs_key_compare(&next->key, end) > 0;
			else
				cmp = scoutfs_key_compare(&next->key, end) < 0;
		} else {
			cmp = true;
		}
		if (cmp) {
			scoutfs_inc_counter(sb, item_shrink_range_end);
			found = item;
			*next_ret = NULL;
			break;
		}

		if (right) {
			scoutfs_key_inc(&item->key);
			cmp = scoutfs_key_compare(&item->key, &next->key) <= 0;
			scoutfs_key_dec(&item->key);
		} else {
			scoutfs_key_dec(&item->key);
			cmp = scoutfs_key_compare(&item->key, &next->key) >= 0;
			scoutfs_key_inc(&item->key);
		}
		if (cmp) {
			found = item;
			*next_ret = next;
			if (i >= BOUNDARY_MIN)
				break;
		}

		if (item_is_dirty(next)) {
			scoutfs_inc_counter(sb, item_shrink_next_dirty);
			break;
		}

		item = next;
	}

	return found;
}

/*
 * The caller found an item in the lru and the range it falls within.
 * This frees items around the item.  After finding the boundaries we
 * have to either update the ranges if items remain or free the item.
 *
 * We're in the context of a shrinker so we can't allocate.  If we
 * remove items from the middle of a range we use the memory from some
 * removed items to store the new split range.
 */
static int shrink_around(struct super_block *sb, struct cached_range *rng,
			 struct cached_item *item)
{
	struct item_cache *cac = SCOUTFS_SB(sb)->item_cache;
	struct scoutfs_key rng_end;
	struct scoutfs_key key;
	struct cached_range *new_rng;
	struct cached_item *first;
	struct cached_item *last;
	struct cached_item *prev;
	struct cached_item *next;
	int nr = 0;

	/* we're re-using item memory as ranges :P */
	BUILD_BUG_ON(sizeof(struct cached_item) < sizeof(struct cached_range));

	first = shrink_boundary(sb, item, &prev, &rng->start, false);
	last = shrink_boundary(sb, item, &next, &rng->end, true);

	trace_scoutfs_item_shrink_around(sb, &rng->start, &rng->end, &item->key,
					 prev ? &prev->key : NULL,
					 first ? &first->key : NULL,
					 last ? &last->key : NULL,
					 next ? &next->key : NULL);

	/* can't shrink if we can't use neighbours */
	if (!first || !last) {
		scoutfs_inc_counter(sb, item_shrink_alone);
		return 0;
	}

	/* can't split if we don't have an item to use for the range */
	if (next && prev && (first == last)) {
		scoutfs_inc_counter(sb, item_shrink_small_split);
		return 0;
	}

	/* set end of remaining existing range, save old for split or freeing */
	if (prev) {
		rng_end = rng->end;
		rng->end = first->key;
		scoutfs_key_dec(&rng->end);
		trace_range(shrink_end, sb, rng);
	}

	/* set start of remaining existing range */
	if (next && !prev) {
		rng->start = last->key;
		scoutfs_key_inc(&rng->start);
		trace_range(shrink_start, sb, rng);
	}

	/* add new range, stealing existing end */
	if (next && prev) {
		item = last;
		last = rb_prev_item(last);

		unlink_item(sb, cac, item);
		key = item->key;
		kfree(item->val);
		nr++;

		new_rng = (void *)item;
		item = NULL;
		memset(new_rng, 0, sizeof(struct cached_range));

		new_rng->end = rng_end;
		new_rng->start = key;
		scoutfs_key_inc(&new_rng->start);
		insert_range(sb, &cac->ranges, new_rng);

		scoutfs_inc_counter(sb, item_shrink_split_range);
	}

	/* totally emptied the range */
	if (!prev && !next) {
		rb_erase(&rng->node, &cac->ranges);
		free_range(sb, rng);
	}

	/* and finally shrink all the surrounding items */
	for (item = first;
	     item && (next = item == last ? NULL : rb_next_item(item), 1);
	     item = next) {
		trace_scoutfs_item_shrink(sb, &item->key);
		scoutfs_inc_counter(sb, item_shrink);
		erase_item(sb, cac, item);
		nr++;
	}

	return nr;
}

/*
 * Shrink the item cache. 
 *
 * Unfortunately this is complicated by the rbtree of ranges that track
 * the validity of the cache.  If we free items we have to make sure
 * they're not covered by ranges or else they'd be considered a valid
 * negative cache hit.  We aggressively try to free items because if we
 * have a structural pattern of keys that we can't free then those build
 * up and fill memory.
 *
 * We can also hit items in the lru which aren't covered by ranges, we
 * free those immediately.
 */
static int item_lru_shrink(struct shrinker *shrink, struct shrink_control *sc)
{
	struct item_cache *cac = container_of(shrink, struct item_cache,
					      shrinker);
	struct super_block *sb = cac->sb;
	struct cached_range *rng;
	struct cached_item *item;
	struct cached_item *first_moved = NULL;
	unsigned long flags;
	unsigned long nr;
	int ret;

	nr = sc->nr_to_scan;
	if (nr == 0)
		goto out;

	spin_lock_irqsave(&cac->lock, flags);

	while (nr &&
	       (item = list_first_entry_or_null(&cac->lru_list,
						struct cached_item, entry))) {

		/* can't have dirty items on the lru */
		BUG_ON(item_is_dirty(item));

		/* if we're not in a range just shrink the item */
		rng = walk_ranges(&cac->ranges, &item->key, NULL, NULL);
		if (!rng) {
			scoutfs_inc_counter(sb, item_shrink_outside);
			erase_item(sb, cac, item);
			nr--;
			continue;
		}

		ret = shrink_around(sb, rng, item);
		if (ret == 0) {
			if (first_moved && first_moved == item)
				break;
			else if (!first_moved)
				first_moved = item;
			list_move_tail(&item->entry, &cac->lru_list);
			continue;
		}

		nr -= min_t(unsigned long, nr, ret);
	}

	/* always try to free empty ranges */
	while (RB_EMPTY_ROOT(&cac->items) &&
	       (rng = rb_first_rng(&cac->ranges))) {
		scoutfs_inc_counter(sb, item_shrink_empty_range);
		rb_erase(&rng->node, &cac->ranges);
		free_range(sb, rng);
	}

	spin_unlock_irqrestore(&cac->lock, flags);

out:
	ret = min_t(unsigned long, cac->lru_nr, INT_MAX);
	trace_scoutfs_item_shrink_exit(sb, sc->nr_to_scan, ret);
	return ret;
}

/*
 * Copy the keys of the sorted cached ranges starting with the search
 * key into the caller's key array.  The number of copied range keys is
 * returned which will always be a multiple of two.
 */
int scoutfs_item_copy_range_keys(struct super_block *sb,
				 struct scoutfs_key *key,
				 struct scoutfs_key *keys, unsigned nr)
{
	struct scoutfs_sb_info *sbi = SCOUTFS_SB(sb);
	struct item_cache *cac = sbi->item_cache;
	struct rb_node *node = cac->ranges.rb_node;
	struct cached_range *next = NULL;
	struct cached_range *rng;
	unsigned long flags;
	int ret = 0;
	int cmp;

	spin_lock_irqsave(&cac->lock, flags);

	while (node) {
		rng = container_of(node, struct cached_range, node);

		cmp = scoutfs_key_compare_ranges(key, key,
						 &rng->start, &rng->end);
		if (cmp < 0) {
			next = rng;
			node = node->rb_left;
		} else if (cmp > 0) {
			node = node->rb_right;
		} else {
			next = rng;
			break;
		}
	}

	for (rng = next; rng; rng = rb_next_rng(rng)) {
		if (ret + 2 > nr)
			break;

		keys[ret++] = rng->start;
		keys[ret++] = rng->end;
	}

	spin_unlock_irqrestore(&cac->lock, flags);

	return ret;
}

/*
 * Copy keys for the sorted cached items starting with the search key
 * into the caller's key array.  The number of copied keys is returned.
 */
int scoutfs_item_copy_keys(struct super_block *sb, struct scoutfs_key *key,
			   struct scoutfs_key *keys, unsigned nr)
{
	struct scoutfs_sb_info *sbi = SCOUTFS_SB(sb);
	struct item_cache *cac = sbi->item_cache;
	struct cached_item *item = NULL;
	unsigned long flags;
	int ret = 0;

	spin_lock_irqsave(&cac->lock, flags);

	for (item = next_item(&cac->items, key); item;
	     item = rb_next_item(item)) {

		if (ret == nr)
			break;

		if (item->deletion)
			continue;

		keys[ret++] = item->key;
	}

	spin_unlock_irqrestore(&cac->lock, flags);

	return ret;
}

int scoutfs_item_setup(struct super_block *sb)
{
	struct scoutfs_sb_info *sbi = SCOUTFS_SB(sb);
	struct item_cache *cac;

	cac = kzalloc(sizeof(struct item_cache), GFP_KERNEL);
	if (!cac)
		return -ENOMEM;
	sbi->item_cache = cac;

	cac->sb = sb;
	spin_lock_init(&cac->lock);
	cac->items = RB_ROOT;
	cac->ranges = RB_ROOT;
	cac->shrinker.shrink = item_lru_shrink;
	cac->shrinker.seeks = DEFAULT_SEEKS;
	register_shrinker(&cac->shrinker);
	INIT_LIST_HEAD(&cac->lru_list);

	return 0;
}

/*
 * There's no more users of the items and ranges at this point.  We can
 * destroy them without locking and ignoring augmentation.
 */
void scoutfs_item_destroy(struct super_block *sb)
{
	struct scoutfs_sb_info *sbi = SCOUTFS_SB(sb);
	struct item_cache *cac = sbi->item_cache;
	struct cached_item *item;
	struct cached_item *pos_item;
	struct cached_range *rng;
	struct cached_range *pos_rng;

	if (cac) {
		if (cac->shrinker.shrink == item_lru_shrink)
			unregister_shrinker(&cac->shrinker);

		rbtree_postorder_for_each_entry_safe(item, pos_item,
						     &cac->items, node) {
			RB_CLEAR_NODE(&item->node);
			INIT_LIST_HEAD(&item->entry);
			free_item(sb, item);
		}

		rbtree_postorder_for_each_entry_safe(rng, pos_rng,
						     &cac->ranges, node) {
			free_range(sb, rng);
		}

		kfree(cac);
	}
}
