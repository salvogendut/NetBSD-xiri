/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2000-2002,2005 Silicon Graphics, Inc.
 * Copyright (c) 2000-2003,2005 Silicon Graphics, Inc.
 * Copyright (c) 2000-2001,2005 Silicon Graphics, Inc.
 * Copyright (c) 2000-2005 Silicon Graphics, Inc.
 * Copyright (c) 2013 Red Hat, Inc.
 * All Rights Reserved.
 */
/* $NetBSD$ */
/* Selected shortform/data verifiers and the data-only readdir scan. */
#include "xfs_read_internal.h"
#include "xfs_directory.h"
#include "xfs_dir_layout.h"

_Static_assert(sizeof(struct xfs_dir3_data_hdr) == 64, "v3 directory header");
_Static_assert(sizeof(struct xfs_dir2_data_hdr) == 16, "v2 directory header");
_Static_assert(XFS_DIR3_DATA_CRC_OFF == 4, "directory checksum offset");

static bool
has_ftype(const struct xfs_sb *sb)
{
	return xfs_crc_enabled(sb) ?
	    (sb->sb_features_incompat & XFS_SB_FEAT_INCOMPAT_FTYPE) != 0 :
	    (sb->sb_versionnum & XFS_SB_VERSION_MOREBITSBIT) &&
	    (sb->sb_features2 & XFS_SB_VERSION2_FTYPE);
}

static unsigned int
entry_size(const struct xfs_sb *sb, unsigned int namelen)
{
	return (9 + namelen + has_ftype(sb) + 2 + 7) & ~7U;
}

static bool
valid_name(const uint8_t *name, size_t length)
{
	return length != 0 && length <= 255 &&
	    memchr(name, 0, length) == NULL && memchr(name, '/', length) == NULL;
}

static unsigned int
dot_name(const uint8_t *name, size_t length)
{
	return length == 1 && name[0] == '.' ? 1 :
	    length == 2 && name[0] == '.' && name[1] == '.' ? 2 : 0;
}

static bool
valid_ino(const struct xfs_sb *sb, uint64_t ino)
{
	uint64_t offset, mask = (UINT64_C(1) <<
	    (sb->sb_agblklog + sb->sb_inopblog)) - 1;
	return ino != 0 && ino <= XFS_MAXINUMBER && (ino & mask) < NULLAGINO &&
	    xfs_data_address(sb, ino >> sb->sb_inopblog, 1, &offset) == 0;
}

static uint32_t
name_hash(const uint8_t *p, unsigned int n)
{
	uint32_t hash = 0;
	while (n-- != 0)
		hash = ((hash << 7) | (hash >> 25)) ^ *p++;
	return hash;
}

static int
emit_entry(xfs_directory_emit emit, void *cookie, uint64_t ino,
    unsigned int type, const uint8_t *name, size_t length, const char **why)
{
	int error = emit(cookie, ino, (uint8_t)type, name, length);
	return error ? xfs_read_fail(why, error > 0 ? error : EIO,
	    "directory callback failed") : 0;
}

static int
shortform(const struct xfs_reader *r, const struct xfs_inode_image *dir,
    xfs_directory_emit emit, void *cookie, const char **why)
{
	const struct xfs_sb *sb = &r->sb;
	const uint8_t *data = dir->disk + dir->data_offset, *p, *name;
	size_t pos, size = dir->size;
	uint64_t ino, parent;
	unsigned int count, width, i8count, i, len, type, offset, next;
	unsigned int header = xfs_crc_enabled(sb) ? 64 : 16;
	bool ftype = has_ftype(sb);
	int error;

	if (size > dir->data_size || size < 6)
		return xfs_read_fail(why, EINVAL, "truncated shortform directory");
	count = data[0];
	width = data[1] ? 8 : 4;
	if (size < 2 + width)
		return xfs_read_fail(why, EINVAL, "truncated shortform parent");
	parent = width == 8 ? xfs_get64(data + 2) : xfs_get32(data + 2);
	if (!valid_ino(sb, parent) || (dir->number == sb->sb_rootino && parent != dir->number))
		return xfs_read_fail(why, EINVAL, "invalid directory parent inode");
	i8count = parent > UINT32_MAX;
	next = header + entry_size(sb, 1) + entry_size(sb, 2);
	pos = 2 + width;
	/* Validate the entire shortform before emitting even the implicit dots. */
	for (i = 0; i < count; i++) {
		if (size - pos < 3)
			return xfs_read_fail(why, EINVAL, "truncated shortform entry");
		p = data + pos;
		len = p[0];
		if (size - pos < 3 + len + ftype + width || !valid_name(p + 3, len) ||
		    dot_name(p + 3, len))
			return xfs_read_fail(why, EINVAL, "invalid shortform entry name or length");
		offset = xfs_get16(p + 1);
		if (offset < next || (offset & 7))
			return xfs_read_fail(why, EINVAL, "invalid shortform entry offset");
		next = offset + entry_size(sb, len);
		type = ftype ? p[3 + len] : 0;
		p += 3 + len + ftype;
		ino = width == 8 ? xfs_get64(p) : xfs_get32(p);
		if (!valid_ino(sb, ino) || type >= XFS_DIR3_FT_MAX)
			return xfs_read_fail(why, EINVAL, "invalid shortform inode or file type");
		i8count += ino > UINT32_MAX;
		pos += 3 + len + ftype + width;
	}
	if (pos != size || i8count != data[1] ||
	    next + (count + 2) * 8 + 8 > (sb->sb_blocksize << sb->sb_dirblklog))
		return xfs_read_fail(why, EINVAL, "invalid shortform count or final size");
	error = emit_entry(emit, cookie, dir->number, XFS_DIR3_FT_DIR,
	    (const uint8_t *)".", 1, why);
	if (error == 0)
		error = emit_entry(emit, cookie, parent, XFS_DIR3_FT_DIR,
		    (const uint8_t *)"..", 2, why);
	if (error != 0)
		return error;
	for (pos = 2 + width, i = 0; i < count; i++) {
		p = data + pos;
		len = p[0];
		name = p + 3;
		type = ftype ? name[len] : 0;
		p = name + len + ftype;
		ino = width == 8 ? xfs_get64(p) : xfs_get32(p);
		error = emit_entry(emit, cookie, ino, type, name, len, why);
		if (error != 0)
			return error;
		pos += 3 + len + ftype + width;
	}
	return 0;
}

/* Verify complete data/free records and, for block directories, the embedded
 * hash index. A bitset proves each index pointer names a distinct live entry
 * start. It avoids quadratic scans and rejects pointers into name bytes. */
static int
verify_data(const struct xfs_reader *r, const struct xfs_inode_image *dir,
    const uint8_t *data, unsigned int bytes, uint64_t logical, uint64_t address,
    unsigned int *data_end, const char **why)
{
	const struct xfs_sb *sb = &r->sb;
	uint8_t starts[XFS_MAX_BLOCKSIZE / 8 / 8] = { 0 };
	unsigned int header = xfs_crc_enabled(sb) ? 64 : 16, best = header == 64 ? 48 : 4;
	unsigned int off, end = bytes, len, nlen, type, i, n = 0, stale = 0, live = 0;
	unsigned int freeoff[3], freelen[3], seen = 0, actual_stale = 0, dot;
	uint32_t magic = xfs_get32(data), hash = 0, thishash, ptr;
	uint64_t ino;
	bool block, lastfree = false, found, ftype = has_ftype(sb);

	block = magic == (xfs_crc_enabled(sb) ? XFS_DIR3_BLOCK_MAGIC : XFS_DIR2_BLOCK_MAGIC);
	if (!block && magic != (xfs_crc_enabled(sb) ? XFS_DIR3_DATA_MAGIC : XFS_DIR2_DATA_MAGIC))
		return xfs_read_fail(why, EINVAL, "invalid directory data magic");
	if (xfs_crc_enabled(sb)) {
		if (xfs_verify_metadata_crc(data, bytes, XFS_DIR3_DATA_CRC_OFF) != 0)
			return xfs_read_fail(why, EINVAL, "directory CRC32C mismatch");
		if (xfs_get64(data + 8) != address / 512 ||
		    xfs_get64(data + 40) != dir->number ||
		    memcmp(data + 24, &sb->sb_meta_uuid, 16) != 0)
			return xfs_read_fail(why, EINVAL, "directory owner, address or UUID mismatch");
	}
	if (block) {
		if (logical != 0 || dir->size != bytes)
			return xfs_read_fail(why, EINVAL, "invalid single-block directory size");
		n = xfs_get32(data + bytes - 8);
		stale = xfs_get32(data + bytes - 4);
		if (n >= (bytes - header - 8) / 8 || stale > n)
			return xfs_read_fail(why, EINVAL, "invalid directory leaf count");
		end -= 8 + n * 8;
	}
	for (i = 0; i < 3; i++) {
		freeoff[i] = xfs_get16(data + best + i * 4);
		freelen[i] = xfs_get16(data + best + i * 4 + 2);
		if ((!freelen[i] && freeoff[i]) || (i && freelen[i] > freelen[i - 1]))
			return xfs_read_fail(why, EINVAL, "invalid directory bestfree table");
		if (!freelen[i])
			seen |= 1U << i;
	}
	for (off = header; off < end; off += len) {
		if (end - off < 8)
			return xfs_read_fail(why, EINVAL, "truncated directory record");
		if (xfs_get16(data + off) == XFS_DIR2_DATA_FREE_TAG) {
			len = xfs_get16(data + off + 2);
			if (lastfree || len < 8 || (len & 7) || len > end - off ||
			    xfs_get16(data + off + len - 2) != off)
				return xfs_read_fail(why, EINVAL, "invalid directory free record");
			found = false;
			for (i = 0; i < 3; i++) {
				if (freeoff[i] != off)
					continue;
				if (freelen[i] != len || (seen & (1U << i)) || found)
					return xfs_read_fail(why, EINVAL, "inconsistent directory bestfree record");
				seen |= 1U << i;
				found = true;
			}
			if (!found && len > freelen[2])
				return xfs_read_fail(why, EINVAL, "missing directory bestfree record");
			lastfree = true;
			continue;
		}
		if (end - off < entry_size(sb, 1))
			return xfs_read_fail(why, EINVAL, "truncated directory entry");
		nlen = data[off + 8];
		len = entry_size(sb, nlen);
		if (len > end - off || !valid_name(data + off + 9, nlen) ||
		    xfs_get16(data + off + len - 2) != off)
			return xfs_read_fail(why, EINVAL, "invalid directory entry name, length or tag");
		ino = xfs_get64(data + off);
		type = ftype ? data[off + 9 + nlen] : 0;
		if (!valid_ino(sb, ino) || type >= XFS_DIR3_FT_MAX)
			return xfs_read_fail(why, EINVAL, "invalid directory inode or file type");
		dot = dot_name(data + off + 9, nlen);
		if (dot && (logical != 0 || off != header + (dot == 2 ? entry_size(sb, 1) : 0) ||
		    (dot == 1 && ino != dir->number) || (ftype && type != XFS_DIR3_FT_DIR) ||
		    (dot == 2 && dir->number == sb->sb_rootino && ino != dir->number)))
			return xfs_read_fail(why, EINVAL, "invalid directory dot entry");
		if (logical == 0 && ((off == header && dot != 1) ||
		    (off == header + entry_size(sb, 1) && dot != 2)))
			return xfs_read_fail(why, EINVAL, "missing directory dot entry");
		starts[off / 64] |= 1U << ((off / 8) & 7);
		live++;
		lastfree = false;
	}
	if (seen != 7 || off != end)
		return xfs_read_fail(why, EINVAL, "incomplete directory bestfree coverage");
	/* The first two entries may not be marked free. */
	if (logical == 0 && (!(starts[header / 64] & (1U << ((header / 8) & 7))) ||
	    !(starts[(header + entry_size(sb, 1)) / 64] &
	    (1U << (((header + entry_size(sb, 1)) / 8) & 7)))))
		return xfs_read_fail(why, EINVAL, "missing directory dot entries");
	if (block) {
		for (i = 0; i < n; i++) {
			thishash = xfs_get32(data + end + i * 8);
			ptr = xfs_get32(data + end + i * 8 + 4);
			if (i && thishash < hash)
				return xfs_read_fail(why, EINVAL, "directory hash index out of order");
			hash = thishash;
			if (!ptr) {
				actual_stale++;
				continue;
			}
			if (ptr >= end / 8 || !(starts[ptr / 8] & (1U << (ptr & 7))))
				return xfs_read_fail(why, EINVAL, "invalid or duplicate directory leaf pointer");
			off = ptr * 8;
			if (hash != name_hash(data + off + 9, data[off + 8]))
				return xfs_read_fail(why, EINVAL, "directory name hash mismatch");
			starts[ptr / 8] &= ~(1U << (ptr & 7));
		}
		if (actual_stale != stale || live != n - stale)
			return xfs_read_fail(why, EINVAL, "directory leaf/live count mismatch");
	}
	*data_end = end;
	return 0;
}

size_t
xfs_directory_scratch_size(const struct xfs_reader *r)
{
	return r == NULL ? 0 : (r->sb.sb_blocksize << r->sb.sb_dirblklog) + r->sb.sb_blocksize;
}

int
xfs_read_directory(const struct xfs_reader *r, const struct xfs_inode_image *dir,
    void *scratch, size_t capacity, xfs_directory_emit emit, void *cookie, const char **why)
{
	const struct xfs_sb *sb;
	uint8_t *data = scratch, *map_scratch;
	uint64_t pos = 0, fsb, end, first_address = 0, address, next;
	unsigned int bytes, blocks, i, limit, off, len, type;
	bool zero;
	int error;

	if (why != NULL)
		*why = NULL;
	if (r == NULL || dir == NULL || scratch == NULL || emit == NULL)
		return xfs_read_fail(why, EINVAL, "null directory argument");
	if (!S_ISDIR(dir->mode))
		return xfs_read_fail(why, ENOTDIR, "inode is not a directory");
	sb = &r->sb;
	if (sb->sb_versionnum & XFS_SB_VERSION_BORGBIT)
		return xfs_read_fail(why, EOPNOTSUPP, "ASCII case-insensitive directories are unsupported");
	if (capacity < xfs_directory_scratch_size(r))
		return xfs_read_fail(why, EMSGSIZE, "directory scratch buffer too small");
	if (dir->format == XFS_DINODE_FMT_LOCAL)
		return shortform(r, dir, emit, cookie, why);
	bytes = sb->sb_blocksize << sb->sb_dirblklog;
	blocks = 1U << sb->sb_dirblklog;
	map_scratch = data + bytes;
	if (!dir->size || dir->size > XFS_DIR2_SPACE_SIZE || dir->size % bytes)
		return xfs_read_fail(why, EINVAL, "invalid directory data size");
	while (pos < dir->size) {
		for (i = 0; i < blocks; i++) {
			error = xfs_file_mapping(r, dir, (pos >> sb->sb_blocklog) + i,
			    map_scratch, sb->sb_blocksize, &fsb, &end, &zero, why);
			if (error != 0)
				return error;
			if (zero) {
				if (fsb || i || !pos)
					return xfs_read_fail(why, EINVAL, "unwritten or partial directory block");
				next = end < (dir->size >> sb->sb_blocklog) ? end : dir->size >> sb->sb_blocklog;
				if (next <= (pos >> sb->sb_blocklog) || (next % blocks))
					return xfs_read_fail(why, EINVAL, "unaligned directory hole");
				pos = next << sb->sb_blocklog;
				break;
			}
			if (xfs_data_address(sb, fsb, 1, &address) != 0)
				return xfs_read_fail(why, EINVAL, "invalid directory block address");
			if (!i)
				first_address = address;
			error = xfs_read_blocks(r, fsb, 1, data + i * sb->sb_blocksize,
			    sb->sb_blocksize, why);
			if (error != 0)
				return error;
		}
		if (i != blocks)
			continue;
		error = verify_data(r, dir, data, bytes, pos, first_address, &limit, why);
		if (error != 0)
			return error;
		for (off = xfs_crc_enabled(sb) ? 64 : 16; off < limit; off += len) {
			if (xfs_get16(data + off) == XFS_DIR2_DATA_FREE_TAG) {
				len = xfs_get16(data + off + 2);
				continue;
			}
			len = entry_size(sb, data[off + 8]);
			type = has_ftype(sb) ? data[off + 9 + data[off + 8]] : 0;
			error = emit_entry(emit, cookie, xfs_get64(data + off), type,
			    data + off + 9, data[off + 8], why);
			if (error != 0)
				return error;
		}
		pos += bytes;
	}
	return 0;
}

struct lookup {
	const void *name;
	size_t length;
	uint64_t ino;
	bool found;
};

static int
lookup_entry(void *cookie, uint64_t ino, uint8_t type,
    const uint8_t *name, size_t length)
{
	struct lookup *l = cookie;
	(void)type;
	if (length == l->length && memcmp(name, l->name, length) == 0) {
		if (l->found)
			return EINVAL;
		l->ino = ino;
		l->found = true;
	}
	return 0;
}

int
xfs_directory_lookup(const struct xfs_reader *r, const struct xfs_inode_image *dir,
    const void *name, size_t length, void *scratch, size_t capacity,
    uint64_t *result, const char **why)
{
	struct lookup l = { name, length, 0, false };
	int error;
	if (name == NULL || result == NULL || !valid_name(name, length))
		return xfs_read_fail(why, EINVAL, "invalid lookup name or output");
	error = xfs_read_directory(r, dir, scratch, capacity, lookup_entry, &l, why);
	if (error != 0)
		return error;
	if (!l.found)
		return xfs_read_fail(why, ENOENT, "directory name not found");
	*result = l.ino;
	return 0;
}
