/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (c) 2000-2005 Silicon Graphics, Inc.
 * Copyright (c) 2013 Red Hat, Inc. All Rights Reserved. */
/* $NetBSD$ */
/* Read-side adaptations of xfs_attr_leaf, xfs_da_btree and xfs_attr_remote. */
#include "xfs_read_internal.h"
#include "xfs_attr.h"
#include "xfs_attr_layout.h"

_Static_assert(sizeof(struct xfs_attr_leaf_hdr) == 32, "v4 leaf header");
_Static_assert(sizeof(struct xfs_attr3_leaf_hdr) == 80, "v5 leaf header");
_Static_assert(sizeof(struct xfs_attr3_node_hdr) == 64, "v5 node header");
_Static_assert(sizeof(struct xfs_attr3_rmt_hdr) == 56, "remote header");

struct attr_block {
	uint64_t fsb;
	uint8_t *data;
	uint32_t logical;
	unsigned int role;
};
struct attr_entry {
	const uint8_t *name, *local;
	uint32_t remote, length;
	uint8_t namelen, flags;
};
struct attr_tree {
	const struct xfs_reader *reader;
	const struct xfs_inode_image *inode;
	struct attr_block *blocks;
	struct attr_entry *entries;
	size_t nblocks, nentries, maxentries;
	uint32_t last[XFS_DA_NODE_MAXDEPTH + 1], forward[XFS_DA_NODE_MAXDEPTH + 1];
	bool seen[XFS_DA_NODE_MAXDEPTH + 1], crc;
	unsigned int bs;
	uint8_t *scratch, *value;
	const char **why;
};

static int
bad(struct attr_tree *t, const char *why)
{
	return xfs_read_fail(t->why, EINVAL, why);
}

static struct attr_block *
find_block(struct attr_tree *t, uint32_t logical)
{
	size_t low = 0, high = t->nblocks, mid;
	while (low < high) {
		mid = low + (high - low) / 2;
		if (t->blocks[mid].logical < logical)
			low = mid + 1;
		else
			high = mid;
	}
	return low < t->nblocks && t->blocks[low].logical == logical ? &t->blocks[low] : NULL;
}

static uint32_t
namehash(const uint8_t *name, size_t length)
{
	uint32_t hash = 0;
	while (length-- != 0)
		hash = (hash << 7 | hash >> 25) ^ *name++;
	return hash;
}

static int
claim_bytes(struct attr_tree *t, unsigned int offset, unsigned int length)
{
	unsigned int i;
	if ((offset | length) & 3 || offset > t->bs || length > t->bs - offset)
		return bad(t, "attribute leaf region out of bounds or alignment");
	for (i = offset / 4; i < (offset + length) / 4; i++) {
		if (t->scratch[i])
			return bad(t, "overlapping attribute leaf regions");
		t->scratch[i] = 1;
	}
	return 0;
}

static int
leaf(struct attr_tree *t, struct attr_block *block, uint32_t low, uint32_t high,
    bool root, uint32_t *maximum)
{
	const uint8_t *data = block->data, *entry, *name;
	unsigned int info = t->crc ? sizeof(struct xfs_attr3_blkinfo) : sizeof(struct xfs_attr_blkinfo);
	unsigned int h = t->crc ? sizeof(struct xfs_attr3_leaf_hdr) : sizeof(struct xfs_attr_leaf_hdr);
	unsigned int count = xfs_get16(data + info), first = xfs_get16(data + info + 4);
	unsigned int used = 0, minimum = t->bs, i, offset, length, nlen, vlen, flags;
	uint32_t hash = low, remote;
	struct attr_entry *out;
	int error;
	if (first == 0 && t->bs == 65536 && count == 0)
		first = t->bs;
	if ((!root && count == 0) || first > t->bs || first < h || count > (first - h) / 8)
		return bad(t, "invalid attribute leaf count or firstused");
	memset(t->scratch, 0, t->bs / 4);
	error = claim_bytes(t, 0, h + count * 8);
	if (error != 0)
		return error;
	for (i = 0; i < count; i++) {
		entry = data + h + i * 8;
		if (xfs_get32(entry) < hash || xfs_get32(entry) > high)
			return bad(t, "attribute hashes out of order or parent range");
		hash = xfs_get32(entry);
		offset = xfs_get16(entry + 4);
		flags = entry[6];
		if ((flags & ~(XFS_ATTR_LOCAL | XFS_ATTR_ROOT | XFS_ATTR_SECURE |
		    XFS_ATTR_PARENT | XFS_ATTR_INCOMPLETE)) ||
		    ((flags & 14) != 0 && ((flags & 14) & ((flags & 14) - 1)) != 0))
			return bad(t, "invalid external attribute namespace flags");
		if (flags & (XFS_ATTR_PARENT | XFS_ATTR_INCOMPLETE))
			return xfs_read_fail(t->why, EOPNOTSUPP, "parent or incomplete external attribute is unsupported");
		length = flags & XFS_ATTR_LOCAL ? 3 : 9;
		if (offset < first || offset > t->bs || length > t->bs - offset)
			return bad(t, "attribute name record out of bounds");
		if (flags & XFS_ATTR_LOCAL) {
			vlen = xfs_get16(data + offset);
			nlen = data[offset + 2];
			name = data + offset + 3;
			remote = 0;
			length = (3 + nlen + vlen + 3) & ~3U;
		} else {
			remote = xfs_get32(data + offset);
			vlen = xfs_get32(data + offset + 4);
			nlen = data[offset + 8];
			name = data + offset + 9;
			/* The historical on-disk size includes two extra padding bytes. */
			length = (11 + nlen + 3) & ~3U;
			if (remote == 0 || vlen == 0 || vlen > XFS_XATTR_SIZE_MAX)
				return bad(t, "invalid remote attribute location or length");
		}
		error = claim_bytes(t, offset, length);
		if (error != 0)
			return error;
		if (nlen == 0 || memchr(name, 0, nlen) != NULL || namehash(name, nlen) != hash)
			return bad(t, "invalid attribute name or hash");
		used += length;
		if (offset < minimum)
			minimum = offset;
		if (t->nentries == t->maxentries)
			return xfs_read_fail(t->why, EMSGSIZE, "attribute entry index exceeds workspace");
		out = &t->entries[t->nentries++];
		*out = (struct attr_entry){ .name = name, .namelen = nlen,
		    .length = vlen, .flags = flags & ~XFS_ATTR_LOCAL, .remote = remote,
		    .local = flags & XFS_ATTR_LOCAL ? name + nlen : NULL };
	}
	/* Splitting/moving entries can leave firstused below the first live
	 * record when holes is set (xfs_attr3_leaf_moveents). */
	if (used != xfs_get16(data + info + 2) ||
	    (data[info + 6] == 0 && first != minimum))
		return bad(t, "attribute leaf usedbytes or firstused mismatch");
	for (i = 0; i < 3; i++) {
		offset = xfs_get16(data + info + 8 + i * 4);
		length = xfs_get16(data + info + 10 + i * 4);
		error = claim_bytes(t, offset, length);
		if (error != 0)
			return error;
	}
	*maximum = hash;
	return 0;
}

static int
walk(struct attr_tree *t, uint32_t logical, int expected_level, uint32_t low,
    uint32_t high, bool root, uint32_t *maximum)
{
	struct attr_block *block = find_block(t, logical);
	const uint8_t *data;
	uint64_t physical;
	uint32_t hash, previous = low, childmax;
	unsigned int magic, level, info, h, count, i;
	int error;
	if (block == NULL || block->role != 0)
		return bad(t, "missing, shared or cyclic attribute tree block");
	block->role = 1;
	data = block->data;
	if (t->crc) {
		error = xfs_verify_metadata_crc(data, t->bs, offsetof(struct xfs_attr3_blkinfo, crc));
		if (error != 0)
			return bad(t, "attribute tree CRC32C mismatch");
		if (xfs_data_address(&t->reader->sb, block->fsb, 1, &physical) != 0 ||
		    xfs_get64(data + 16) != physical / 512 ||
		    memcmp(data + 32, &t->reader->sb.sb_meta_uuid, 16) != 0 ||
		    xfs_get64(data + 48) != t->inode->number)
			return bad(t, "attribute tree identity mismatch");
	}
	magic = xfs_get16(data + 8);
	info = t->crc ? sizeof(struct xfs_attr3_blkinfo) : sizeof(struct xfs_attr_blkinfo);
	if (magic == (t->crc ? XFS_ATTR3_LEAF_MAGIC : XFS_ATTR_LEAF_MAGIC))
		level = 0;
	else if (magic == (t->crc ? XFS_DA3_NODE_MAGIC : XFS_DA_NODE_MAGIC)) {
		level = xfs_get16(data + info + 2);
		if (level == 0 || level > XFS_DA_NODE_MAXDEPTH)
			return bad(t, "invalid attribute node level");
	} else
		return bad(t, "invalid attribute tree magic");
	if (!root && (int)level != expected_level)
		return bad(t, "attribute tree level mismatch");
	if (xfs_get32(data + 4) != (t->seen[level] ? t->last[level] : 0) ||
	    (t->seen[level] && t->forward[level] != logical))
		return bad(t, "attribute tree sibling link mismatch");
	t->seen[level] = true;
	t->last[level] = logical;
	t->forward[level] = xfs_get32(data);
	if (level == 0)
		return leaf(t, block, low, high, root, maximum);
	h = t->crc ? sizeof(struct xfs_attr3_node_hdr) : sizeof(struct xfs_attr_node_hdr);
	count = xfs_get16(data + info);
	if (count == 0 || count > (t->bs - h) / 8)
		return bad(t, "invalid attribute node count");
	for (i = 0; i < count; i++) {
		hash = xfs_get32(data + h + i * 8);
		if (hash < previous || hash > high)
			return bad(t, "attribute node hashes out of order or range");
		error = walk(t, xfs_get32(data + h + i * 8 + 4), level - 1,
		    previous, hash, false, &childmax);
		if (error != 0)
			return error;
		if (childmax != hash)
			return bad(t, "attribute node hash disagrees with child");
		previous = hash;
	}
	*maximum = previous;
	return 0;
}

static int
compare(const struct attr_entry *a, const struct attr_entry *b)
{
	if (a->flags != b->flags)
		return a->flags < b->flags ? -1 : 1;
	if (a->namelen != b->namelen)
		return a->namelen < b->namelen ? -1 : 1;
	return memcmp(a->name, b->name, a->namelen);
}

/* Iterative heapsort avoids hidden libc allocations and quadratic collisions. */
static void
sift(struct attr_entry *a, size_t root, size_t count)
{
	struct attr_entry tmp;
	size_t child;
	while (root < count / 2) {
		child = root * 2 + 1;
		if (child + 1 < count && compare(&a[child], &a[child + 1]) < 0)
			child++;
		if (compare(&a[root], &a[child]) >= 0)
			break;
		tmp = a[root]; a[root] = a[child]; a[child] = tmp;
		root = child;
	}
}

static int
remote_value(struct attr_tree *t, const struct attr_entry *entry, bool validate)
{
	unsigned int h = t->crc ? sizeof(struct xfs_attr3_rmt_hdr) : 0;
	unsigned int done = 0, part;
	uint64_t logical = entry->remote, physical;
	struct attr_block *block;
	const uint8_t *data;
	while (done < entry->length) {
		if (logical > UINT32_MAX || (block = find_block(t, (uint32_t)logical)) == NULL)
			return bad(t, "remote attribute hole or range overflow");
		data = block->data;
		part = t->bs - h;
		if (part > entry->length - done)
			part = entry->length - done;
		if (validate) {
			if (block->role != 0)
				return bad(t, "overlapping remote attribute or tree blocks");
			block->role = 2;
			if (t->crc) {
				if (xfs_verify_metadata_crc(data, t->bs,
				    offsetof(struct xfs_attr3_rmt_hdr, crc)) != 0)
					return bad(t, "remote attribute CRC32C mismatch");
				if (xfs_data_address(&t->reader->sb, block->fsb, 1, &physical) != 0 ||
				    xfs_get32(data) != XFS_ATTR3_RMT_MAGIC ||
				    xfs_get32(data + 4) != done || xfs_get32(data + 8) != part ||
				    memcmp(data + 16, &t->reader->sb.sb_meta_uuid, 16) != 0 ||
				    xfs_get64(data + 32) != t->inode->number ||
				    xfs_get64(data + 40) != physical / 512)
					return bad(t, "remote attribute identity, offset or length mismatch");
			}
		} else
			memcpy(t->value + done, data + h, part);
		done += part;
		logical++;
	}
	return 0;
}

int
xfs_read_attrs_external(const struct xfs_reader *reader, const struct xfs_inode_image *inode,
    unsigned int offset, unsigned int size, unsigned int format, uint64_t nextents,
    void *workspace, size_t capacity, xfs_attr_emit_fn emit, void *cookie, const char **why)
{
	struct attr_tree t = { .reader = reader, .inode = inode, .why = why,
	    .bs = reader->sb.sb_blocksize, .crc = xfs_crc_enabled(&reader->sb) };
	struct attr_entry tmp;
	uint8_t *start = workspace, *end;
	size_t padding, maxblocks, i, j;
	uint64_t logical = 0, fsb, limit, seen_extents = 0;
	uint32_t maximum;
	bool zero;
	int error;
	padding = (-(uintptr_t)start) & (_Alignof(struct attr_block) - 1);
	if (workspace == NULL || capacity < padding + t.bs + XFS_XATTR_SIZE_MAX)
		return xfs_read_fail(why, EMSGSIZE, "external attributes need workspace");
	start += padding;
	capacity -= padding;
	end = start + capacity;
	/* Both ends stay aligned; extra trailing bytes are deliberately unused. */
	end -= (uintptr_t)end % _Alignof(struct attr_block);
	if ((size_t)(end - start) < t.bs + XFS_XATTR_SIZE_MAX)
		return xfs_read_fail(why, EMSGSIZE, "external attributes need aligned workspace");
	t.scratch = end -= t.bs;
	t.value = end -= XFS_XATTR_SIZE_MAX;
	maxblocks = (size_t)(end - start) / (t.bs + sizeof(struct attr_block));
	t.blocks = (void *)start;
	start += maxblocks * sizeof(struct attr_block);
	while (logical < (UINT64_C(1) << 32)) {
		error = xfs_attr_mapping(reader, inode, offset, size, format, nextents,
		    logical, t.scratch, t.bs, &fsb, &limit, &zero, why);
		if (error != 0)
			return error;
		if (limit <= logical || limit > (UINT64_C(1) << 32) || (zero && fsb != 0))
			return bad(&t, "invalid or unwritten attribute mapping");
		if (zero) {
			logical = limit;
			continue;
		}
		seen_extents++;
		if (limit - logical > maxblocks - t.nblocks)
			return xfs_read_fail(why, EMSGSIZE, "attribute blocks exceed workspace");
		while (logical < limit) {
			for (i = 0; i < t.nblocks; i++)
				if (t.blocks[i].fsb == fsb)
					return bad(&t, "overlapping physical attribute mappings");
			if ((size_t)(end - start) < t.bs)
				return xfs_read_fail(why, EMSGSIZE, "attribute blocks exceed workspace");
			end -= t.bs;
			error = xfs_read_blocks(reader, fsb, 1, end, t.bs, why);
			if (error != 0)
				return error;
			t.blocks[t.nblocks++] = (struct attr_block){ .fsb = fsb++, .data = end,
			    .logical = (uint32_t)logical++ };
		}
	}
	if (seen_extents != nextents || t.nblocks > xfs_get64(inode->disk + offsetof(struct xfs_dinode, di_nblocks)))
		return bad(&t, "attribute mapping count disagrees with inode");
	t.entries = (void *)start;
	t.maxentries = (size_t)(end - start) / sizeof(struct attr_entry);
	error = walk(&t, 0, -1, 0, UINT32_MAX, true, &maximum);
	if (error != 0)
		return error;
	for (i = 0; i <= XFS_DA_NODE_MAXDEPTH; i++)
		if (t.forward[i] != 0)
			return bad(&t, "unterminated attribute sibling chain");
	for (i = t.nentries / 2; i != 0; i--)
		sift(t.entries, i - 1, t.nentries);
	for (i = t.nentries; i > 1; i--) {
		tmp = t.entries[0]; t.entries[0] = t.entries[i - 1]; t.entries[i - 1] = tmp;
		sift(t.entries, 0, i - 1);
	}
	for (i = 0; i < t.nentries; i++) {
		if (i != 0 && compare(&t.entries[i - 1], &t.entries[i]) == 0)
			return bad(&t, "duplicate external attribute name in namespace");
		if (t.entries[i].local == NULL) {
			error = remote_value(&t, &t.entries[i], true);
			if (error != 0)
				return error;
		}
	}
	for (j = 0; j < t.nblocks; j++)
		if (t.blocks[j].role == 0)
			return bad(&t, "unreferenced mapped attribute block");
	/* All I/O and verification are complete before the first callback. */
	for (i = 0; i < t.nentries; i++) {
		const struct attr_entry *entry = &t.entries[i];
		if (entry->local == NULL) {
			error = remote_value(&t, entry, false);
			if (error != 0)
				return error;
		}
		error = emit(cookie, entry->flags, entry->name, entry->namelen,
		    entry->local != NULL ? entry->local : t.value, entry->length);
		if (error != 0)
			return xfs_read_fail(why, error, "attribute callback failed");
	}
	return 0;
}
