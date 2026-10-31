/* SPDX-License-Identifier: GPL-2.0 */
/* $NetBSD$ */
/*
 * Adapted from Linux v6.12 libxfs/xfs_cksum.h's safe verification path.
 * Bound the checksum field and use memcpy for unaligned buffers. The
 * software CRC32C implementation replaces Linux's crc32c service.
 */
#include "xfs_io.h"
#include <sys/errno.h>

static uint32_t
crc32c(uint32_t crc, const void *data, size_t length)
{
	const uint8_t *bytes = data;
	size_t i;
	unsigned int bit;

	for (i = 0; i < length; i++) {
		crc ^= bytes[i];
		for (bit = 0; bit < 8; bit++)
			crc = (crc >> 1) ^ ((crc & 1) ? 0x82f63b78U : 0);
	}
	return crc;
}

int
xfs_verify_metadata_crc(const void *data, size_t length, size_t offset)
{
	const uint8_t *bytes = data;
	uint32_t zero = 0, stored, crc;

	if (data == NULL || offset > length || length - offset < sizeof(stored))
		return EINVAL;
	crc = crc32c(UINT32_MAX, data, offset);
	crc = crc32c(crc, &zero, sizeof(zero));
	crc = crc32c(crc, bytes + offset + sizeof(stored),
	    length - offset - sizeof(stored));
	memcpy(&stored, bytes + offset, sizeof(stored));
	return le32toh(stored) == ~crc ? 0 : EINVAL;
}
