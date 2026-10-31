/* SPDX-License-Identifier: GPL-2.0-only */
/* $NetBSD$ */

/*
 * Scalar and byte-order compatibility for the imported Linux XFS disk
 * definitions.  This is not a Linux kernel API emulation layer.
 */
#ifndef _XFS_NETBSD_COMPAT_H_
#define _XFS_NETBSD_COMPAT_H_

#include <sys/types.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __NetBSD__
#include <sys/endian.h>
#else
#include <endian.h>
#endif

#ifdef _KERNEL
#include <sys/systm.h>
#define ASSERT(x) KASSERT(x)
#else
#include <assert.h>
#include <string.h>
#define ASSERT(x) assert(x)
#endif

typedef uint8_t __u8;
typedef uint16_t __u16;
typedef uint32_t __u32;
typedef uint64_t __u64;
typedef int8_t __s8;
typedef int16_t __s16;
typedef int32_t __s32;
typedef int64_t __s64;
typedef uint16_t __be16;
typedef uint32_t __be32;
typedef uint64_t __be64;
typedef uint32_t __le32;
typedef int64_t time64_t;
typedef uint64_t xfs_ino_t;
typedef int64_t xfs_daddr_t;
typedef uint32_t xfs_nlink_t;
typedef uint32_t xfs_dev_t;

/* Linux UUIDs are opaque bytes, unlike NetBSD's structured UUID type. */
typedef struct { uint8_t bytes[16]; } xfs_uuid_t;

#define be16_to_cpu(x) be16toh(x)
#define be32_to_cpu(x) be32toh(x)
#define be64_to_cpu(x) be64toh(x)
#define cpu_to_be16(x) htobe16(x)
#define cpu_to_be32(x) htobe32(x)
#define cpu_to_be64(x) htobe64(x)
#define uuid_copy(dst, src) memcpy((dst), (src), sizeof(xfs_uuid_t))

#ifndef __packed
#define __packed __attribute__((__packed__))
#endif
#ifndef NBBY
#define NBBY 8
#endif
#define S32_MAX INT32_MAX
#define S32_MIN INT32_MIN
#define U32_MAX UINT32_MAX
#ifndef NSEC_PER_SEC
#define NSEC_PER_SEC 1000000000L
#endif

#include "xfs_types.h"
/* Keep Linux's uuid_t spelling confined to the unchanged imported header. */
#define uuid_t xfs_uuid_t
#include "xfs_format.h"
#undef uuid_t
#include "xfs_sb.h"
#include "xfs_quota_flags.h"

_Static_assert(sizeof(struct xfs_dsb) == 264, "XFS superblock layout");
_Static_assert(offsetof(struct xfs_dsb, sb_versionnum) == 100,
    "XFS version offset");
_Static_assert(XFS_SB_CRC_OFF == 224, "XFS checksum offset");
_Static_assert(offsetof(struct xfs_dsb, sb_meta_uuid) == 248,
    "XFS metadata UUID offset");

#endif /* _XFS_NETBSD_COMPAT_H_ */
