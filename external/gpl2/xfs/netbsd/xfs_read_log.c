/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2000-2006 Silicon Graphics, Inc.
 * All Rights Reserved.
 */
/* $NetBSD$ */
/*
 * Read-only adaptation of xlog_find_head, xlog_check_unmount_rec,
 * xlog_unpack_data and xlog_cksum from Linux v6.12. A full cycle scan replaces
 * the recovery search/repair heuristics. Only an unambiguous complete latest
 * record can establish cleanliness. Never fall back to an older unmount.
 */
#include "xfs_log.h"
#include "xfs_log_layout.h"
#include "xfs_crc.h"
#include <sys/errno.h>

_Static_assert(offsetof(struct xlog_rec_header, h_size) == 320, "log header");
_Static_assert(sizeof(struct xlog_rec_ext_header) == 260, "extended header");
_Static_assert(sizeof(struct xlog_op_header) == 12, "log operation");

struct log_geometry {
	uint64_t offset;
	uint32_t blocks, unit;
};
struct log_head {
	uint32_t block, cycle, record;
	bool zero_tail;
};

static int
fail(const char **why, int error, const char *message)
{
	if (why != NULL)
		*why = message;
	return error;
}

static uint32_t
get32(const void *data)
{
	__be32 value;
	memcpy(&value, data, sizeof(value));
	return be32_to_cpu(value);
}

static bool
zeroes(const uint8_t *data, size_t length)
{
	size_t i;
	for (i = 0; i < length; i++)
		if (data[i] != 0)
			return false;
	return true;
}

static uint32_t
next_cycle(uint32_t cycle)
{
	if (++cycle == XLOG_HEADER_MAGIC_NUM)
		cycle++;
	return cycle;
}

static uint32_t
prev_cycle(uint32_t cycle)
{
	if (--cycle == XLOG_HEADER_MAGIC_NUM)
		cycle--;
	return cycle;
}

static int
geometry(const struct xfs_reader *reader, struct log_geometry *log,
    const char **why)
{
	const struct xfs_sb *sb = &reader->sb;
	uint64_t agno, agbno, offset, bytes;
	uint32_t blocks, sector = 512;

	if (XFS_SB_VERSION_NUM(sb) != 5 &&
	    !(sb->sb_versionnum & XFS_SB_VERSION_LOGV2BIT))
		return fail(why, EOPNOTSUPP, "v1 journals are not supported");
	if (sb->sb_logstart == 0 || sb->sb_agblklog > 32)
		return fail(why, EINVAL, "invalid internal log address");
	agno = sb->sb_logstart >> sb->sb_agblklog;
	agbno = sb->sb_logstart & ((UINT64_C(1) << sb->sb_agblklog) - 1);
	if (agno >= sb->sb_agcount ||
	    xfs_ag_geometry(sb, (uint32_t)agno, &offset, &blocks) != 0 ||
	    agbno >= blocks || sb->sb_logblocks == 0 ||
	    sb->sb_logblocks > blocks - agbno)
		return fail(why, EINVAL, "internal log outside allocation group");
	bytes = (uint64_t)sb->sb_logblocks * sb->sb_blocksize;
	/* XFS_MAX_LOG_BLOCKS / XFS_MAX_LOG_BYTES, pinned libxfs/xfs_fs.h. */
	if (sb->sb_logblocks > 1024 * 1024 ||
	    bytes > UINT64_C(2147483648) - 10 * 1024 * 1024)
		return fail(why, EINVAL, "internal log exceeds upstream size limits");
	if (sb->sb_versionnum & XFS_SB_VERSION_SECTORBIT) {
		if (sb->sb_logsectlog < 9 || sb->sb_logsectlog > 12 ||
		    sb->sb_logsectsize != (1U << sb->sb_logsectlog))
			return fail(why, EINVAL, "invalid log sector geometry");
		sector = sb->sb_logsectsize;
	} else if (sb->sb_logsectsize != 0 || sb->sb_logsectlog != 0) {
		return fail(why, EINVAL, "unexpected log sector geometry");
	}
	if (sb->sb_logsunit > XLOG_MAX_RECORD_BSIZE ||
	    (sb->sb_logsunit > 1 && sb->sb_logsunit % sb->sb_blocksize != 0))
		return fail(why, EINVAL, "invalid log stripe unit");
	log->offset = offset + agbno * sb->sb_blocksize;
	log->blocks = (uint32_t)(bytes / 512);
	log->unit = sb->sb_logsunit > sector ? sb->sb_logsunit : sector;
	if (log->offset % sector != 0 || bytes % log->unit != 0 ||
	    log->offset > reader->io.size || bytes > reader->io.size - log->offset)
		return fail(why, EINVAL, "invalid log alignment or device bounds");
	return 0;
}

/* Only A...A,previous(A)...previous(A), or cycle 1 followed by zeros. */
static int
find_head(const struct xfs_reader *reader, const struct log_geometry *log,
    uint8_t *scratch, struct log_head *head, const char **why)
{
	uint32_t block, i, count, cycle, first = 0, last = 0, magic;
	uint32_t prefix_record = UINT32_MAX, suffix_record = UINT32_MAX;
	int error;

	head->block = log->blocks;
	head->zero_tail = false;
	for (block = 0; block < log->blocks; block += count) {
		count = log->blocks - block;
		if (count > 128)
			count = 128;
		error = xfs_io_read(&reader->io, log->offset + (uint64_t)block * 512,
		    scratch, count * 512);
		if (error != 0)
			return fail(why, error, "log cycle scan read failed");
		for (i = 0; i < count; i++) {
			const uint8_t *p = scratch + i * 512;
			magic = get32(p);
			cycle = magic == XLOG_HEADER_MAGIC_NUM ? get32(p + 4) : magic;
			if (block + i == 0) {
				first = last = cycle;
				if (first == 0 || first == XLOG_HEADER_MAGIC_NUM)
					return fail(why, EOPNOTSUPP,
					    "zero or ambiguous log cycle; cleanliness unknown");
			}
			if (cycle != last) {
				if (head->block != log->blocks || cycle != prev_cycle(first))
					return fail(why, EOPNOTSUPP,
					    "nonmonotonic log cycles; recovery assessment required");
				head->block = block + i;
				head->zero_tail = cycle == 0;
				last = cycle;
			}
			if (cycle == 0 && !zeroes(p, 512))
				return fail(why, EINVAL, "nonzero data in zero-cycle log suffix");
			if (magic == XLOG_HEADER_MAGIC_NUM) {
				if (block + i < head->block)
					prefix_record = block + i;
				else
					suffix_record = block + i;
			}
		}
	}
	head->cycle = first;
	if (head->block == log->blocks) {
		head->block = 0;
		head->cycle = next_cycle(first);
	}
	if (head->cycle == 0)
		return fail(why, EOPNOTSUPP, "log cycle rollover is unsupported");
	head->record = prefix_record != UINT32_MAX ? prefix_record : suffix_record;
	if (head->record == UINT32_MAX)
		return fail(why, EINVAL, "log has no record header before head");
	return 0;
}

/* Read at most one wrap; all sizes/addresses have been bounded beforehand. */
static int
read_record(const struct xfs_reader *reader, const struct log_geometry *log,
    uint32_t block, uint32_t count, uint8_t *data, const char **why)
{
	uint32_t first = log->blocks - block;
	int error;
	if (first > count)
		first = count;
	error = xfs_io_read(&reader->io, log->offset + (uint64_t)block * 512,
	    data, first * 512);
	if (error == 0 && first < count)
		error = xfs_io_read(&reader->io, log->offset, data + first * 512,
		    (count - first) * 512);
	return error == 0 ? 0 : fail(why, error, "log record read failed");
}

static uint32_t
record_crc(uint8_t *data, uint32_t hblocks, uint32_t length, size_t hdrsize)
{
	uint32_t crc, i;
	crc = xfs_crc32c(UINT32_MAX, data, hdrsize);
	for (i = 1; i < (length + XLOG_HEADER_CYCLE_SIZE - 1) /
	    XLOG_HEADER_CYCLE_SIZE; i++)
		crc = xfs_crc32c(crc, data + i * 512, sizeof(struct xlog_rec_ext_header));
	return ~xfs_crc32c(crc, data + hblocks * 512, length);
}

static int
verify_record(const struct xfs_reader *reader, const struct log_geometry *log,
    const struct log_head *head, uint8_t *data, struct xfs_log_info *info,
    const char **why)
{
	struct xlog_rec_header hdr;
	uint32_t length, size, hblocks, blocks, cycle, fmt, tailcycle, tailblock;
	uint32_t i, group, slot, stamp, stored, offset, numops, prev;
	uint64_t lsn, tail;
	uint8_t *body;
	bool unmount, initial;
	int error;

	error = read_record(reader, log, head->record, 1, data, why);
	if (error != 0)
		return error;
	memcpy(&hdr, data, sizeof(hdr));
	length = be32_to_cpu(hdr.h_len);
	size = be32_to_cpu(hdr.h_size);
	cycle = be32_to_cpu(hdr.h_cycle);
	fmt = be32_to_cpu(hdr.h_fmt);
	lsn = be64_to_cpu(hdr.h_lsn);
	tail = be64_to_cpu(hdr.h_tail_lsn);
	tailcycle = (uint32_t)(tail >> 32);
	tailblock = (uint32_t)tail;
	numops = be32_to_cpu(hdr.h_num_logops);
	prev = be32_to_cpu(hdr.h_prev_block);
	if (be32_to_cpu(hdr.h_magicno) != XLOG_HEADER_MAGIC_NUM || cycle == 0 ||
	    cycle == XLOG_HEADER_MAGIC_NUM ||
	    lsn != ((uint64_t)cycle << 32 | head->record) ||
	    memcmp(&hdr.h_fs_uuid, &reader->sb.sb_uuid, sizeof(xfs_uuid_t)) != 0)
		return fail(why, EINVAL, "log record identity or LSN mismatch");
	if (be32_to_cpu(hdr.h_version) != XLOG_VERSION_2 ||
	    (fmt != XLOG_FMT_LINUX_LE && fmt != XLOG_FMT_LINUX_BE && fmt != XLOG_FMT_IRIX_BE))
		return fail(why, EOPNOTSUPP, "unsupported log record version or format");
	if (size < XLOG_MIN_RECORD_BSIZE || size > XLOG_MAX_RECORD_BSIZE ||
	    size % 512 != 0 || length < 20 || length % 512 != 0 || length > size)
		return fail(why, EINVAL, "invalid log record size");
	hblocks = (size + XLOG_HEADER_CYCLE_SIZE - 1) / XLOG_HEADER_CYCLE_SIZE;
	blocks = hblocks + length / 512;
	if (blocks >= log->blocks || blocks * 512 > size ||
	    head->record * 512U % log->unit != 0 || blocks * 512 % log->unit != 0 ||
	    (head->record + blocks) % log->blocks != head->block ||
	    (head->record + blocks >= log->blocks ? next_cycle(cycle) : cycle) != head->cycle)
		return fail(why, EINVAL, "incomplete or misaligned record at log head");
	if (tailcycle == 0 || tailblock >= log->blocks ||
	    (tailcycle == cycle ? tailblock > head->record :
	    (tailcycle != prev_cycle(cycle) || tailblock <= head->record)) ||
	    (prev != UINT32_MAX && (prev >= log->blocks || prev == head->record)) ||
	    numops == 0 || numops > length / sizeof(struct xlog_op_header))
		return fail(why, EINVAL, "invalid log tail, predecessor or operation count");
	error = read_record(reader, log, head->record, blocks, data, why);
	if (error != 0)
		return error;
	if (memcmp(&hdr, data, sizeof(hdr)) != 0)
		return fail(why, EINVAL, "log changed during inspection");
	for (i = 1; i < blocks; i++) {
		stamp = head->record + i >= log->blocks ? next_cycle(cycle) : cycle;
		if (get32(data + i * 512) != stamp)
			return fail(why, EINVAL, "log record cycle stamp mismatch");
	}
	stored = le32toh(hdr.h_crc);
	memset(data + offsetof(struct xlog_rec_header, h_crc), 0, 4);
	/* Upstream sizeof(header) includes four tail-padding bytes on 64-bit ABIs. */
	if (stored != 0 && stored != record_crc(data, hblocks, length, 328) &&
	    stored != record_crc(data, hblocks, length, 324))
		return fail(why, EINVAL, "log record CRC32C mismatch");
	body = data + hblocks * 512;
	for (i = 0; i < length / 512; i++) {
		group = i / 64;
		slot = i % 64;
		offset = group == 0 ? offsetof(struct xlog_rec_header, h_cycle_data) : 4;
		memcpy(body + i * 512, data + group * 512 + offset + slot * 4, 4);
	}
	unmount = numops == 1 && get32(body + 4) == 8 &&
	    body[8] == XFS_LOG && body[9] == XLOG_UNMOUNT_TRANS &&
	    zeroes(body + 10, 2) && zeroes(body + 14, 6) &&
	    (fmt == XLOG_FMT_LINUX_LE ? (body[12] == 0x6e && body[13] == 0x55) :
	    (body[12] == 0x55 && body[13] == 0x6e));
	if ((body[9] & XLOG_UNMOUNT_TRANS) && !unmount)
		return fail(why, EINVAL, "malformed log unmount operation");
	initial = stored == 0;
	if (initial) {
		/* mkfs's special unchecksummed record: accept only its exact shape. */
		uint32_t init_size = log->unit > 32768 ? log->unit : 32768;
		uint32_t init_bytes = ((hblocks * 512 + 20 + log->unit - 1) /
		    log->unit) * log->unit;
		if (!head->zero_tail || head->record != 0 || cycle != 1 || tail != lsn ||
		    prev != UINT32_MAX || size != init_size || blocks * 512 != init_bytes ||
		    !unmount || get32(body) != 0xb0c0d0d0 || !zeroes(body + 20, length - 20) ||
		    !zeroes(data + 48, 252) || !zeroes(data + 324, 512 - 324))
			return fail(why, EOPNOTSUPP, "unchecksummed log is not a canonical initial unmount");
		for (i = 1; i < hblocks; i++)
			if (!zeroes(data + i * 512 + 4, 508))
				return fail(why, EINVAL, "nonzero initial log extended header data");
	}
	info->state = unmount ? XFS_LOG_CLEAN : XFS_LOG_NEEDS_RECOVERY;
	info->head = head->block;
	info->tail = unmount ? head->block : tailblock;
	info->record = head->record;
	info->cycle = head->cycle;
	info->initialized = initial;
	return 0;
}

int
xfs_log_inspect(const struct xfs_reader *reader, void *scratch, size_t capacity,
    struct xfs_log_info *result, const char **why)
{
	struct log_geometry log;
	struct log_head head;
	struct xfs_log_info info = { 0 };
	int error;

	if (why != NULL)
		*why = NULL;
	if (reader == NULL || scratch == NULL || result == NULL)
		return fail(why, EINVAL, "null log inspection argument");
	if (capacity < XFS_LOG_SCRATCH_SIZE)
		return fail(why, EMSGSIZE, "log scratch buffer too small");
	error = geometry(reader, &log, why);
	if (error == 0)
		error = find_head(reader, &log, scratch, &head, why);
	if (error == 0)
		error = verify_record(reader, &log, &head, scratch, &info, why);
	if (error != 0)
		return error;
	*result = info;
	return 0;
}

int
xfs_require_clean_log(const struct xfs_reader *reader, void *scratch,
    size_t capacity, const char **why)
{
	struct xfs_log_info info;
	int error = xfs_log_inspect(reader, scratch, capacity, &info, why);
	if (error != 0)
		return error;
	if (info.state != XFS_LOG_CLEAN)
		return fail(why, EBUSY, "journal needs recovery; replay is not implemented");
	return 0;
}
