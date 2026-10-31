/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (c) 2000-2003,2005 Silicon Graphics, Inc.
 * Copyright (c) 2013 Red Hat, Inc. All Rights Reserved. */
/* $NetBSD$ */
/* Read-side adaptations of xfs_dir2_leaf, xfs_dir2_node and xfs_da_btree.
 * Verify a bounded, caller-owned directory image before exposing any entry.
 * No Linux buffer cache, transaction or write-side operation is emulated. */
#include "xfs_read_internal.h"
#include "xfs_dir_layout.h"

_Static_assert(sizeof(struct xfs_dir2_leaf_hdr) == 16, "v2 leaf header");
_Static_assert(sizeof(struct xfs_dir3_leaf_hdr) == 64, "v3 leaf header");
_Static_assert(sizeof(struct xfs_dir2_free_hdr) == 16, "v2 free header");
_Static_assert(sizeof(struct xfs_dir3_free_hdr) == 64, "v3 free header");

struct dir_block {
	uint8_t *data, *starts;
	uint64_t address;
	uint32_t logical;
	unsigned int role;
};
struct dir_tree {
	const struct xfs_reader *reader;
	const struct xfs_inode_image *inode;
	struct dir_block *blocks;
	size_t nblocks, live, indexed;
	uint32_t last[XFS_DA_NODE_MAXDEPTH + 1], forward[XFS_DA_NODE_MAXDEPTH + 1];
	bool seen[XFS_DA_NODE_MAXDEPTH + 1], crc, single;
	unsigned int bs, bytes, fsblocks, info, header, free_max;
	uint32_t leaf_base, free_base;
	const char **why;
};

static int
bad(struct dir_tree *t, const char *why)
{
	return xfs_read_fail(t->why, EINVAL, why);
}

static struct dir_block *
find_block(struct dir_tree *t, uint32_t logical)
{
	size_t low = 0, high = t->nblocks, mid;
	while (low < high) {
		mid = low + (high - low) / 2;
		if (t->blocks[mid].logical < logical) low = mid + 1;
		else high = mid;
	}
	return low < t->nblocks && t->blocks[low].logical == logical ? &t->blocks[low] : NULL;
}

static int
load(struct dir_tree *t, void *workspace, size_t capacity)
{
	uint8_t *start = workspace, *end, *map;
	size_t padding = (-(uintptr_t)start) & (_Alignof(struct dir_block) - 1), maxblocks;
	uint64_t logical = 0, fsb, limit, address;
	unsigned int i, count, data_end;
	struct dir_block *b;
	bool zero;
	int error;
	if (capacity < padding + t->bs)
		return xfs_read_fail(t->why, EMSGSIZE, "directory index workspace too small");
	start += padding;
	end = start + capacity - padding;
	end -= (uintptr_t)end % _Alignof(struct dir_block);
	if ((size_t)(end - start) < t->bs)
		return xfs_read_fail(t->why, EMSGSIZE, "aligned directory workspace too small");
	map = end -= t->bs;
	maxblocks = (size_t)(end - start) / (sizeof(*b) + t->bytes + t->bytes / 64);
	t->blocks = (void *)start;
	start += maxblocks * sizeof(*b);
	/* Scan the complete fork, including holes and the free-space region.
	 * Do not silently ignore mappings outside the three directory spaces. */
	while (logical < (UINT64_C(1) << 54)) {
		error = xfs_file_mapping(t->reader, t->inode, logical, map, t->bs,
		    &fsb, &limit, &zero, t->why);
		if (error != 0) return error;
		if (limit <= logical || limit > (UINT64_C(1) << 54) || (zero && fsb))
			return bad(t, "invalid or unwritten directory mapping");
		if (zero) {
			if (logical == 0 || limit % t->fsblocks)
				return bad(t, "missing first block or unaligned directory hole");
			logical = limit;
			continue;
		}
		if (logical >= (uint64_t)t->leaf_base * 3)
			return bad(t, "directory mapping outside data, index and free spaces");
		if (t->nblocks == maxblocks || (size_t)(end - start) < t->bytes + t->bytes / 64)
			return xfs_read_fail(t->why, EMSGSIZE, "directory data and indexes exceed workspace");
		b = &t->blocks[t->nblocks++];
		*b = (struct dir_block){ .logical = (uint32_t)logical };
		b->data = end -= t->bytes;
		b->starts = end -= t->bytes / 64;
		for (i = 0; i < t->fsblocks; i++) {
			if (i != 0) {
				error = xfs_file_mapping(t->reader, t->inode, logical + i, map,
				    t->bs, &fsb, &limit, &zero, t->why);
				if (error != 0) return error;
			}
			if (zero || xfs_data_address(&t->reader->sb, fsb, 1, &address) != 0)
				return bad(t, "partial or unwritten directory block");
			if (i == 0) b->address = address;
			error = xfs_read_blocks(t->reader, fsb, 1,
			    b->data + i * t->bs, t->bs, t->why);
			if (error != 0) return error;
		}
		if (logical < t->leaf_base) {
			if ((logical << t->reader->sb.sb_blocklog) >= t->inode->size ||
			    xfs_get32(b->data) != (t->crc ? XFS_DIR3_DATA_MAGIC : XFS_DIR2_DATA_MAGIC))
				return bad(t, "external directory data size or magic mismatch");
			error = xfs_dir_verify_data(t->reader, t->inode, b->data, t->bytes,
			    logical << t->reader->sb.sb_blocklog, b->address,
			    &data_end, b->starts, &count, t->why);
			if (error != 0) return error;
			t->live += count;
			b->role = 1;
		}
		logical += t->fsblocks;
	}
	return 0;
}

static int
identity(struct dir_tree *t, struct dir_block *b, bool da)
{
	const uint8_t *p = b->data;
	unsigned int crc = da ? 12 : 4, addr = da ? 16 : 8;
	if (!t->crc) return 0;
	if (xfs_verify_metadata_crc(p, t->bytes, crc) != 0)
		return bad(t, "directory index CRC32C mismatch");
	if (xfs_get64(p + addr) != b->address / 512 ||
	    xfs_get64(p + addr + 32) != t->inode->number ||
	    memcmp(p + addr + 16, &t->reader->sb.sb_meta_uuid, 16) != 0)
		return bad(t, "directory index owner, address or UUID mismatch");
	return 0;
}

static uint16_t
best(struct dir_tree *t, uint32_t db)
{
	struct dir_block *b = find_block(t, db * t->fsblocks);
	return b != NULL && b->role == 1 ? xfs_get16(b->data + (t->crc ? 50 : 6)) : UINT16_MAX;
}

static int
claim(struct dir_tree *t, uint32_t hash, uint32_t ptr)
{
	uint64_t position = (uint64_t)ptr * 8;
	unsigned int off = position % t->bytes, bit = (off / 8) & 7;
	struct dir_block *b;
	if (position >= t->inode->size)
		return bad(t, "directory leaf pointer exceeds data size");
	b = find_block(t, (position / t->bytes) * t->fsblocks);
	if (b == NULL || b->role != 1 || !(b->starts[off / 64] & (1U << bit)))
		return bad(t, "invalid or duplicate directory leaf pointer");
	if (hash != xfs_dir_name_hash(b->data + off + 9, b->data[off + 8]))
		return bad(t, "directory leaf hash disagrees with data entry");
	b->starts[off / 64] &= ~(1U << bit);
	t->indexed++;
	return 0;
}

static int
walk(struct dir_tree *t, uint32_t logical, int expected, uint32_t low,
    uint32_t high, bool root, uint32_t *maximum)
{
	struct dir_block *b = find_block(t, logical);
	const uint8_t *p;
	unsigned int magic, level, count, i, stale = 0, end = t->bytes;
	uint32_t hash = low, ptr, childmax, bestcount;
	int error;
	bool leaf1, node;
	if (logical < t->leaf_base || logical >= t->free_base ||
	    logical % t->fsblocks || b == NULL || b->role != 0)
		return bad(t, "missing, shared or cyclic directory index block");
	b->role = 2;
	error = identity(t, b, true);
	if (error != 0) return error;
	p = b->data;
	magic = xfs_get16(p + 8);
	leaf1 = magic == (t->crc ? XFS_DIR3_LEAF1_MAGIC : XFS_DIR2_LEAF1_MAGIC);
	node = magic == (t->crc ? XFS_DA3_NODE_MAGIC : XFS_DA_NODE_MAGIC);
	if (!node && !leaf1 && magic != (t->crc ? XFS_DIR3_LEAFN_MAGIC : XFS_DIR2_LEAFN_MAGIC))
		return bad(t, "invalid directory index magic");
	if (leaf1 && !root) return bad(t, "leaf1 below directory root");
	if (root) t->single = leaf1;
	level = node ? xfs_get16(p + t->info + 2) : 0;
	if ((node && level == 0) || level > XFS_DA_NODE_MAXDEPTH ||
	    (!root && (int)level != expected))
		return bad(t, "invalid directory index level");
	if (xfs_get32(p + 4) != (t->seen[level] ? t->last[level] : 0) ||
	    (t->seen[level] && t->forward[level] != logical))
		return bad(t, "directory index sibling link mismatch");
	t->seen[level] = true;
	t->last[level] = logical;
	t->forward[level] = xfs_get32(p);
	if (leaf1) {
		bestcount = xfs_get32(p + t->bytes - 4);
		if (bestcount != t->inode->size / t->bytes ||
		    bestcount > (t->bytes - t->header - 4) / 2)
			return bad(t, "invalid directory leaf bestcount");
		end -= 4 + bestcount * 2;
		for (i = 0; i < bestcount; i++)
			if (xfs_get16(p + end + i * 2) != best(t, i))
				return bad(t, "directory leaf bests disagree with data");
	}
	count = xfs_get16(p + t->info);
	if (count == 0 || count > (end - t->header) / 8)
		return bad(t, "invalid directory index count");
	for (i = 0; i < count; i++) {
		ptr = xfs_get32(p + t->header + i * 8 + 4);
		childmax = xfs_get32(p + t->header + i * 8);
		if (childmax < hash || childmax > high)
			return bad(t, "directory index hashes out of order or parent range");
		if (node) {
			error = walk(t, ptr, level - 1, hash, childmax, false, maximum);
			if (error != 0) return error;
			if (*maximum != childmax)
				return bad(t, "directory node hash disagrees with child");
		} else if (ptr == 0) {
			stale++;
		} else {
			error = claim(t, childmax, ptr);
			if (error != 0) return error;
		}
		hash = childmax;
	}
	if (!node && stale != xfs_get16(p + t->info + 2))
		return bad(t, "directory leaf stale count mismatch");
	*maximum = hash;
	return 0;
}

static int
free_index(struct dir_tree *t, struct dir_block *b)
{
	const uint8_t *p = b->data;
	uint64_t first = (uint64_t)(b->logical - t->free_base) / t->fsblocks * t->free_max;
	unsigned int h = t->crc ? 48 : 4, i, valid, used = 0;
	uint16_t value;
	int error;
	if (t->single || xfs_get32(p) != (t->crc ? XFS_DIR3_FREE_MAGIC : XFS_DIR2_FREE_MAGIC))
		return bad(t, "unexpected directory free index or magic");
	error = identity(t, b, false);
	if (error != 0) return error;
	valid = xfs_get32(p + h + 4);
	if (first > UINT32_MAX || first != xfs_get32(p + h) ||
	    valid > t->free_max || first + valid > t->inode->size / t->bytes)
		return bad(t, "invalid directory free index range");
	for (i = 0; i < valid; i++) {
		value = xfs_get16(p + t->header + i * 2);
		if (value != best(t, first + i))
			return bad(t, "directory free index disagrees with data");
		used += value != UINT16_MAX;
	}
	if (used != xfs_get32(p + h + 8))
		return bad(t, "directory free index used count mismatch");
	b->role = 3;
	return 0;
}

static int
emit(struct dir_tree *t, const uint8_t *p, xfs_directory_emit callback, void *cookie)
{
	unsigned int type;
	/* File type availability follows the data verifier, including v4 ftype. */
	bool ftype = t->crc ? (t->reader->sb.sb_features_incompat & XFS_SB_FEAT_INCOMPAT_FTYPE) != 0 :
	    (t->reader->sb.sb_versionnum & XFS_SB_VERSION_MOREBITSBIT) &&
	    (t->reader->sb.sb_features2 & XFS_SB_VERSION2_FTYPE);
	int error;
	type = ftype ? p[9 + p[8]] : 0;
	error = callback(cookie, xfs_get64(p), (uint8_t)type, p + 9, p[8]);
	return error ? xfs_read_fail(t->why, error > 0 ? error : EIO,
	    "directory callback failed") : 0;
}

/* Lower-bound search must include every equal-hash child and leaf entry;
 * collisions may cross a leaf boundary. The entire tree is already verified. */
static int
lookup(struct dir_tree *t, struct dir_block *b, uint32_t hash,
    const void *name, size_t length, xfs_directory_emit callback, void *cookie)
{
	const uint8_t *p = b->data, *entry;
	unsigned int low = 0, high = xfs_get16(p + t->info), count = high, mid, off;
	bool node = xfs_get16(p + 8) == (t->crc ? XFS_DA3_NODE_MAGIC : XFS_DA_NODE_MAGIC);
	uint32_t value, ptr;
	struct dir_block *data;
	int error;
	while (low < high) {
		mid = low + (high - low) / 2;
		if (xfs_get32(p + t->header + mid * 8) < hash) low = mid + 1;
		else high = mid;
	}
	for (; low < count; low++) {
		value = xfs_get32(p + t->header + low * 8);
		ptr = xfs_get32(p + t->header + low * 8 + 4);
		if (node) {
			error = lookup(t, find_block(t, ptr), hash, name, length, callback, cookie);
			if (error != 0) return error;
			if (value > hash) break;
		} else {
			if (value != hash) break;
			if (ptr == 0) continue;
			off = ((uint64_t)ptr * 8) % t->bytes;
			data = find_block(t, ((uint64_t)ptr * 8 / t->bytes) * t->fsblocks);
			entry = data->data + off;
			if (entry[8] != length || memcmp(entry + 9, name, length) != 0) continue;
			error = emit(t, entry, callback, cookie);
			if (error != 0) return error;
		}
	}
	return 0;
}

int
xfs_read_dir_index(const struct xfs_reader *r, const struct xfs_inode_image *dir,
    void *workspace, size_t capacity, xfs_directory_emit callback, void *cookie,
    const void *name, size_t length, const char **why)
{
	struct dir_tree t = { .reader = r, .inode = dir, .why = why,
	    .bs = r->sb.sb_blocksize, .bytes = r->sb.sb_blocksize << r->sb.sb_dirblklog,
	    .fsblocks = 1U << r->sb.sb_dirblklog, .crc = xfs_crc_enabled(&r->sb) };
	struct dir_block *b, *freeblock;
	size_t i;
	unsigned int off, len, db;
	uint32_t maximum;
	int error;
	t.info = t.crc ? 56 : 12;
	t.header = t.crc ? 64 : 16;
	t.free_max = (t.bytes - t.header) / 2;
	t.leaf_base = XFS_DIR2_SPACE_SIZE >> r->sb.sb_blocklog;
	t.free_base = t.leaf_base * 2;
	error = load(&t, workspace, capacity);
	if (error != 0) return error;
	error = walk(&t, t.leaf_base, -1, 0, UINT32_MAX, true, &maximum);
	if (error != 0) return error;
	if (t.live != t.indexed) return bad(&t, "directory index omits live data entries");
	for (i = 0; i <= XFS_DA_NODE_MAXDEPTH; i++)
		if (t.forward[i]) return bad(&t, "unterminated directory sibling chain");
	for (i = 0; i < t.nblocks; i++) {
		b = &t.blocks[i];
		if (b->logical >= t.free_base) {
			error = free_index(&t, b);
			if (error != 0) return error;
		} else if (b->role == 0) return bad(&t, "unreferenced mapped directory index block");
	}
	if (!t.single) {
		for (i = 0; i < t.nblocks && t.blocks[i].role == 1; i++) {
			db = t.blocks[i].logical / t.fsblocks;
			freeblock = find_block(&t, t.free_base + db / t.free_max * t.fsblocks);
			if (freeblock == NULL || freeblock->role != 3 ||
			    db % t.free_max >= xfs_get32(freeblock->data + (t.crc ? 52 : 8)))
				return bad(&t, "directory data block missing from free index");
		}
	}
	if (name != NULL)
		return lookup(&t, find_block(&t, t.leaf_base), xfs_dir_name_hash(name, length),
		    name, length, callback, cookie);
	/* Preserve data-order enumeration and hence existing PUFFS cookies. */
	for (i = 0; i < t.nblocks && t.blocks[i].role == 1; i++) {
		b = &t.blocks[i];
		for (off = t.header; off < t.bytes; off += len) {
			if (xfs_get16(b->data + off) == XFS_DIR2_DATA_FREE_TAG) {
				len = xfs_get16(b->data + off + 2);
				continue;
			}
			len = xfs_dir_entry_size(&r->sb, b->data[off + 8]);
			error = emit(&t, b->data + off, callback, cookie);
			if (error != 0) return error;
		}
	}
	return 0;
}
