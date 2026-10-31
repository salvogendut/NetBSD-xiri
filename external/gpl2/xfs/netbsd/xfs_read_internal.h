/* SPDX-License-Identifier: GPL-2.0-only */
/* $NetBSD$ */
#ifndef _XFS_READ_INTERNAL_H_
#define _XFS_READ_INTERNAL_H_
#include "xfs_inode.h"
#include "xfs_attr.h"
#include <sys/errno.h>
#include <sys/stat.h>

static inline uint16_t
xfs_get16(const void *p)
{
	__be16 n;
	memcpy(&n, p, sizeof(n));
	return be16_to_cpu(n);
}

static inline uint32_t
xfs_get32(const void *p)
{
	__be32 n;
	memcpy(&n, p, sizeof(n));
	return be32_to_cpu(n);
}

static inline uint64_t
xfs_get64(const void *p)
{
	__be64 n;
	memcpy(&n, p, sizeof(n));
	return be64_to_cpu(n);
}

static inline bool
xfs_crc_enabled(const struct xfs_sb *sb)
{
	return XFS_SB_VERSION_NUM(sb) == XFS_SB_VERSION_5;
}

static inline int
xfs_read_fail(const char **why, int error, const char *message)
{
	if (why != NULL)
		*why = message;
	return error;
}

/* Data/metadata ranges exclude AG headers and the internal log. */
int xfs_data_address(const struct xfs_sb *, uint64_t, uint32_t, uint64_t *);
int xfs_read_tree(const struct xfs_reader *, uint64_t, uint64_t, bool,
    unsigned int, void *, size_t, unsigned int *, unsigned int *, const char **);
int xfs_tree_child(const struct xfs_sb *, const uint8_t *, unsigned int,
    unsigned int, bool, uint32_t, uint64_t, bool, uint64_t *, uint64_t *,
    uint64_t *, const char **);
int xfs_extent_lookup(const struct xfs_sb *, const uint8_t *, uint64_t,
    uint64_t, bool, uint64_t, uint64_t, uint64_t *, uint64_t *, bool *,
    const char **);
int xfs_bmap_root(const struct xfs_sb *, const uint8_t *, unsigned int,
    uint64_t, unsigned int *, uint64_t *, uint64_t *, uint64_t *, const char **);
int xfs_file_mapping(const struct xfs_reader *, const struct xfs_inode_image *,
    uint64_t, void *, size_t, uint64_t *, uint64_t *, bool *, const char **);
int xfs_attr_mapping(const struct xfs_reader *, const struct xfs_inode_image *,
    unsigned int, unsigned int, unsigned int, uint64_t, uint64_t, void *, size_t,
    uint64_t *, uint64_t *, bool *, const char **);
int xfs_read_attrs_external(const struct xfs_reader *, const struct xfs_inode_image *,
    unsigned int, unsigned int, unsigned int, uint64_t, void *, size_t,
    xfs_attr_emit_fn, void *, const char **);
int xfs_read_symlink_range(const struct xfs_reader *, const struct xfs_inode_image *,
    uint64_t, void *, size_t, void *, size_t, const char **);

#endif
