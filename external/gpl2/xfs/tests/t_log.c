/* SPDX-License-Identifier: GPL-2.0-only */
/* $NetBSD$ */
#include "fixture.h"
#include "xfs_log.h"
#include "xfs_log_layout.h"
#include <errno.h>

#define LOG_BLOCKS 2048
static uint8_t disk[LOG_BLOCKS * 512], record[XFS_LOG_SCRATCH_SIZE];
static uint8_t scratch[XFS_LOG_SCRATCH_SIZE + 1], linear[XFS_LOG_SCRATCH_SIZE];
static struct xfs_reader reader;
static uint64_t log_offset;
static unsigned int calls, fail_call, short_call, over_call, checks;
static uint32_t rec_start, rec_blocks, rec_hblocks, rec_length;
static size_t crc_header_size;

static void
put32(void *where, uint32_t value)
{
	__be32 encoded = cpu_to_be32(value);
	memcpy(where, &encoded, 4);
}

static int
fake_read(void *cookie, uint64_t offset, void *buffer, size_t length, size_t *done)
{
	CHECK(cookie == disk);
	calls++;
	CHECK(offset >= log_offset && offset - log_offset <= sizeof(disk));
	CHECK(length <= sizeof(disk) - (offset - log_offset));
	if (calls == fail_call)
		return EIO;
	memcpy(buffer, disk + offset - log_offset, length);
	*done = calls == short_call ? length - 1 : length;
	if (calls == over_call)
		*done = length + 1;
	return 0;
}

static void
setup(bool v5)
{
	union sector sb;
	const char *why;
	fixture(&sb, v5);
	sb.sb.sb_logblocks = cpu_to_be32(sizeof(disk) / 4096);
	if (v5)
		checksum(&sb);
	memset(&reader, 0, sizeof(reader));
	CHECK(xfs_probe_superblock(sb.bytes, 512, IMAGE_BYTES, &reader.sb, &why) == 0);
	reader.io = (struct xfs_io){ disk, IMAGE_BYTES, fake_read };
	log_offset = (UINT64_C(20000) + 100) * 4096;
	memset(disk, 0, sizeof(disk));
	calls = fail_call = short_call = over_call = 0;
}

/* Independent checksum: concatenate the on-disk regions, use test CRC table. */
static void
seal(void)
{
	size_t size = crc_header_size;
	unsigned int i;
	memcpy(linear, record, size);
	for (i = 1; i < (rec_length + 32767) / 32768; i++) {
		memcpy(linear + size, record + i * 512, 260);
		size += 260;
	}
	memcpy(linear + size, record + rec_hblocks * 512, rec_length);
	size += rec_length;
	metadata_checksum(linear, (unsigned int)size, 32);
	memcpy(record + 32, linear + 32, 4);
}

static void
publish(void)
{
	unsigned int i;
	for (i = 0; i < rec_blocks; i++)
		memcpy(disk + (rec_start + i) % LOG_BLOCKS * 512, record + i * 512, 512);
}

static uint32_t
next(uint32_t cycle)
{
	cycle++;
	return cycle == XLOG_HEADER_MAGIC_NUM ? cycle + 1 : cycle;
}

static void
make_record(uint32_t start, uint32_t cycle, uint32_t size, uint32_t length,
    uint32_t fmt, bool clean, bool crc)
{
	struct xlog_rec_header hdr = { 0 };
	uint8_t *body;
	uint32_t i, group, slot, offset;
	memset(record, 0, sizeof(record));
	rec_start = start;
	rec_hblocks = (size + 32767) / 32768;
	rec_length = length;
	rec_blocks = rec_hblocks + (length + 511) / 512;
	crc_header_size = 328;
	hdr.h_magicno = cpu_to_be32(XLOG_HEADER_MAGIC_NUM);
	hdr.h_cycle = cpu_to_be32(cycle);
	hdr.h_version = cpu_to_be32(2);
	hdr.h_len = cpu_to_be32(length);
	hdr.h_lsn = cpu_to_be64((uint64_t)cycle << 32 | start);
	hdr.h_tail_lsn = hdr.h_lsn;
	hdr.h_prev_block = cpu_to_be32(UINT32_MAX);
	hdr.h_num_logops = cpu_to_be32(1);
	hdr.h_fmt = cpu_to_be32(fmt);
	hdr.h_fs_uuid = reader.sb.sb_uuid;
	hdr.h_size = cpu_to_be32(size);
	memcpy(record, &hdr, sizeof(hdr));
	body = record + rec_hblocks * 512;
	put32(body, 0xb0c0d0d0);
	put32(body + 4, 8);
	body[8] = clean ? XFS_LOG : XFS_TRANSACTION;
	body[9] = clean ? XLOG_UNMOUNT_TRANS : 2; /* COMMIT */
	body[12] = fmt == XLOG_FMT_LINUX_LE ? 0x6e : 0x55;
	body[13] = fmt == XLOG_FMT_LINUX_LE ? 0x55 : 0x6e;
	for (i = 0; i < (length + 511) / 512; i++) {
		group = i / 64;
		slot = i % 64;
		offset = group == 0 ? 44 : 4;
		memcpy(record + group * 512 + offset + slot * 4, body + i * 512, 4);
		put32(body + i * 512, cycle);
	}
	for (i = 1; i < rec_hblocks; i++)
		put32(record + i * 512, cycle);
	for (i = 1; i < rec_blocks; i++)
		if (start + i >= LOG_BLOCKS)
			put32(record + i * 512, next(cycle));
	if (crc)
		seal();
	publish();
}

static struct xfs_log_info
inspect(int expected)
{
	struct xfs_log_info info, before;
	const char *why = NULL;
	int error;
	memset(&info, 0xa5, sizeof(info));
	memcpy(&before, &info, sizeof(info));
	error = xfs_log_inspect(&reader, scratch + 1, sizeof(scratch) - 1, &info, &why);
	if (error != expected) {
		fprintf(stderr, "log test %u: got %d, expected %d: %s\n", checks,
		    error, expected, why == NULL ? "success" : why);
		exit(1);
	}
	CHECK((why == NULL) == (error == 0));
	if (error != 0)
		CHECK(memcmp(&info, &before, sizeof(info)) == 0);
	checks++;
	return info;
}

static void
make_v1(uint32_t start, uint32_t cycle, uint32_t length, uint32_t fmt,
    bool clean, bool checksum, bool old_string)
{
	reader.sb.sb_versionnum &= ~XFS_SB_VERSION_LOGV2BIT;
	make_record(start, cycle, 32768, length, fmt ? fmt : XLOG_FMT_IRIX_BE, clean, false);
	put32(record + 8, 1);
	if (fmt == 0)
		memset(record + 300, 0, 212); /* actual IRIX header shape */
	if (!clean)
		put32(record + 516, length - 12);
	if (old_string) {
		put32(record + 516, 20);
		memcpy(record + 524, "Unmount filesystem--", 20);
	}
	if (checksum) seal();
	publish();
}

static void
stamps(uint32_t head, uint32_t cycle)
{
	uint32_t i, previous = cycle - 1;
	if (previous == XLOG_HEADER_MAGIC_NUM)
		previous--;
	for (i = 0; i < LOG_BLOCKS; i++)
		put32(disk + i * 512, i < head ? cycle : previous);
}

static void
v1_tests(void)
{
	static const uint32_t lengths[] = {12, 20, 508, 512, 516, 32256};
	struct xfs_log_info info;
	const char *why;
	uint32_t fmt, crc, wrap, start, cycle, n, i, count;
	for (fmt = 0; fmt <= 3; fmt++) {
		for (crc = 0; crc <= 1; crc++) {
			if (fmt == 0 && crc) continue;
			for (wrap = 0; wrap < 3; wrap++) {
				start = wrap == 0 ? 0 : wrap == 1 ? LOG_BLOCKS - 1 : LOG_BLOCKS - 2;
				cycle = wrap ? 7 : 1;
				setup(false);
				if (wrap) stamps(wrap == 1 ? 1 : LOG_BLOCKS, wrap == 1 ? 8 : 7);
				make_v1(start, cycle, 20, fmt, true, crc, false);
				info = inspect(0);
				CHECK(info.state == XFS_LOG_CLEAN && info.version == 1 && !info.initialized);
				CHECK(info.legacy == (fmt == 0) && info.checksummed == (crc != 0));
				CHECK(info.head == (start + 2) % LOG_BLOCKS && info.record == start);
				CHECK(xfs_require_clean_log(&reader, scratch, sizeof(scratch), &why) == 0);
				/* Padding beyond v1 h_len is not part of the operation. */
				record[512 + 100] = 0xa5; publish();
				CHECK(inspect(0).state == XFS_LOG_CLEAN);
				if (fmt != 1) {
					make_v1(start, cycle, 32, fmt, true, crc, true);
					CHECK(inspect(0).state == XFS_LOG_CLEAN);
				}
			}
			for (n = 0; n < sizeof(lengths) / sizeof(lengths[0]); n++) {
				setup(false);
				make_v1(0, 1, lengths[n], fmt, false, crc, false);
				info = inspect(0);
				CHECK(info.state == XFS_LOG_NEEDS_RECOVERY);
				CHECK(info.head == 1 + (lengths[n] + 511) / 512);
				CHECK(xfs_require_clean_log(&reader, scratch, sizeof(scratch), &why) == EBUSY);
			}
		}
	}
	/* Every read of a wrapped legacy record: I/O errors and bad byte counts. */
	setup(false); stamps(1, 8);
	make_v1(LOG_BLOCKS - 1, 7, 32, 0, true, false, true);
	calls = 0; inspect(0); count = calls;
	for (i = 1; i <= count; i++) {
		calls = 0; fail_call = i; inspect(EIO); fail_call = 0;
		calls = 0; short_call = i; inspect(EIO); short_call = 0;
		calls = 0; over_call = i; inspect(EIO); over_call = 0;
	}
	/* Mixed/unknown identities and legacy checksum encodings stay rejected. */
	for (i = 0; i < 5; i++) {
		setup(false); make_v1(0, 1, 20, 0, true, false, false);
		switch (i) {
		case 0: record[32] = 1; break;
		case 1: record[304] = 1; break;
		case 2: put32(record + 320, 32768); break;
		case 3: record[324] = 1; break;
		case 4: record[511] = 1; break;
		}
		publish(); inspect(EOPNOTSUPP);
	}
	for (i = 0; i < 15; i++) {
		setup(false); make_v1(0, 1, 20, 0, true, false, false);
		switch (i) {
		case 0: put32(record + 12, 12); break;
		case 1: put32(record + 12, 16); break;
		case 2: put32(record + 12, 21); break;
		case 3: put32(record + 12, 24); break;
		case 4: put32(record + 12, 32768); break;
		case 5: put32(record + 20, 1); break;
		case 6: put32(record + 28, 1); break;
		case 7: put32(record + 36, 0); break;
		case 8: put32(record + 40, 2); break;
		case 9: put32(record + 516, 4); break;
		case 10: record[520] = XFS_TRANSACTION; break;
		case 11: record[521] |= 1; break;
		case 12: record[522] = 1; break;
		case 13: record[524] = 0x6e; break; /* wrong payload byte order */
		case 14: record[526] = 1; break;
		}
		publish(); inspect(EINVAL);
	}
	for (i = 0; i < 20; i++) {
		setup(false); make_v1(0, 1, 32, 0, true, false, true);
		record[524 + i] ^= 1; publish(); inspect(EINVAL);
	}
	for (i = 0; i < 6; i++) {
		setup(false); make_v1(0, 1, 20, 0, false, false, false);
		switch (i) {
		case 0: put32(record + 516, UINT32_MAX); break;
		case 1: put32(record + 516, 4); break;
		case 2: put32(record + 516, 7); break;
		case 3: record[520] = 0; break;
		case 4: record[521] = 0x80; break;
		case 5: record[522] = 1; break;
		}
		publish(); inspect(EINVAL);
	}
	/* Latest dirty record takes precedence over an older clean unmount. */
	setup(false); make_v1(0, 1, 20, 0, true, false, false);
	make_v1(2, 1, 516, 0, false, false, false);
	CHECK(inspect(0).state == XFS_LOG_NEEDS_RECOVERY);
	put32(disk + 4 * 512, 0); /* torn last sector, including partial payload */
	inspect(EINVAL);
	/* Whole-sector mkfs records, and unchecksummed old Linux unmounts. */
	setup(false); make_v1(0, 1, 512, 1, true, false, false);
	CHECK(inspect(0).initialized);
	record[44] ^= 1; publish();
	CHECK(!inspect(0).initialized);
	record[512 + 20] = 1; publish(); inspect(EINVAL);
	/* No v1 interpretation or missing-UUID exception on a v2 filesystem. */
	setup(false); make_v1(0, 1, 20, 0, true, false, false);
	reader.sb.sb_versionnum |= XFS_SB_VERSION_LOGV2BIT;
	inspect(EOPNOTSUPP);
	put32(record + 8, 2); publish(); inspect(EINVAL);
	setup(false); make_v1(0, 1, 20, 1, true, true, false);
	record[512 + 12] ^= 1; publish(); inspect(EINVAL); /* CRC protected */
	setup(false); make_v1(0, 1, 20, 0, true, false, false);
	reader.sb.sb_logsunit = 4096; inspect(EINVAL);
	reader.sb.sb_logsunit = 0;
	reader.sb.sb_versionnum |= XFS_SB_VERSION_SECTORBIT;
	reader.sb.sb_logsectsize = 4096; reader.sb.sb_logsectlog = 12; inspect(EINVAL);
}

int
main(void)
{
	struct xfs_log_info info;
	const char *why;
	uint32_t i, j, call_count, start, size, length, cycle;
	uint8_t saved;
	bool v5;
	int error;

	v1_tests();
	for (i = 0; i < 2; i++) {
		v5 = i != 0;
		for (j = 1; j <= 3; j++) {
			setup(v5);
			make_record(0, 1, 32768, 512, j, true, false);
			info = inspect(0);
			CHECK(info.state == XFS_LOG_CLEAN && info.initialized);
			CHECK(info.head == 2 && info.tail == 2 && info.record == 0 && info.cycle == 1);
			CHECK(xfs_require_clean_log(&reader, scratch, sizeof(scratch), &why) == 0);
			make_record(2, 1, 32768, 512, j, false, true);
			info = inspect(0);
			CHECK(info.state == XFS_LOG_NEEDS_RECOVERY && !info.initialized);
			CHECK(info.head == 4 && info.record == 2 && info.tail == 2);
			CHECK(xfs_require_clean_log(&reader, scratch, sizeof(scratch), &why) == EBUSY);
			make_record(4, 1, 32768, 512, j, true, true);
			CHECK(inspect(0).state == XFS_LOG_CLEAN);
			/* Linux unmounts use zero oh_len, unlike mkfs's eight. */
			put32(record + rec_hblocks * 512 + 4, 0);
			seal(); publish();
			CHECK(inspect(0).state == XFS_LOG_CLEAN);
		}
	}
	/* Linux's xlog_clear_stale_blocks writes empty cycle-zero headers ahead
	 * of the head on a second mount. They must never conceal a dirty record.
	 */
	setup(true);
	make_record(0, 1, 32768, 512, 1, true, true);
	make_record(2, 1, 32768, 512, 1, false, true);
	{
		struct xlog_rec_header cleared = { 0 };
		uint8_t *p = disk + 4 * 512;
		cleared.h_magicno = cpu_to_be32(XLOG_HEADER_MAGIC_NUM);
		cleared.h_version = cpu_to_be32(2);
		cleared.h_lsn = cpu_to_be64(4);
		cleared.h_tail_lsn = cpu_to_be64(UINT64_C(1) << 32);
		cleared.h_fmt = cpu_to_be32(1);
		cleared.h_fs_uuid = reader.sb.sb_uuid;
		memcpy(p, &cleared, sizeof(cleared));
		CHECK(inspect(0).state == XFS_LOG_NEEDS_RECOVERY);
		CHECK(xfs_require_clean_log(&reader, scratch, sizeof(scratch), &why) == EBUSY);
		/* Reject mutations throughout the empty-header block, including UUID,
		 * LSN, version, payload length, checksum and unchecked padding.
		 */
		for (i = 8; i < 512; i++) {
			saved = p[i]; p[i] ^= 0x80;
			inspect(EINVAL);
			p[i] = saved;
		}
		put32(p + 28, 4); /* stale tail must precede the current head */
		inspect(EINVAL);
	}
	/* Extended headers, wraps in headers and bodies, head exactly at zero. */
	for (size = 16384; size <= 262144; size *= 2) {
		for (j = 0; j < 6; j++) {
			setup(true);
			length = j % 2 ? size - ((size + 32767) / 32768) * 512 : 512;
			start = j < 2 ? 100 : j < 4 ? LOG_BLOCKS - 1 :
			    LOG_BLOCKS - ((size + 32767) / 32768 + length / 512);
			cycle = j == 3 ? XLOG_HEADER_MAGIC_NUM - 1 : 7;
			stamps((start + (size + 32767) / 32768 + length / 512) % LOG_BLOCKS,
			    start > 100 ? next(cycle) : cycle);
			if (j >= 4) /* physical end: every block has the old cycle */
				for (i = 0; i < LOG_BLOCKS; i++)
					put32(disk + i * 512, cycle);
			make_record(start, cycle, size, length, 1, true, true);
			info = inspect(0);
			CHECK(info.state == XFS_LOG_CLEAN && info.record == start);
			CHECK(info.head == (start + rec_blocks) % LOG_BLOCKS);
			/* Both upstream ABI header checksum lengths. */
			crc_header_size = 324;
			seal();
			publish();
			CHECK(inspect(0).state == XFS_LOG_CLEAN);
		}
	}
	setup(true);
	make_record(0, 1, 32768, 512, 1, true, true);
	/* A checksum protects every header and payload byte used to declare clean. */
	for (i = 0; i < 1024; i++) {
		if (i >= 328 && i < 512) /* padding outside the checksum */
			continue;
		saved = disk[i];
		disk[i] ^= 0x80;
		/* Corruption may be detected by geometry before the CRC. */
		error = xfs_require_clean_log(&reader, scratch, sizeof(scratch), &why);
		CHECK(error != 0);
		checks++;
		disk[i] = saved;
	}
	/* Header padding in the 64-bit checksum can be nonzero; 32-bit checksums differ. */
	CHECK(inspect(0).state == XFS_LOG_CLEAN);
	call_count = calls = 0;
	inspect(0);
	call_count = calls;
	for (i = 1; i <= call_count; i++) {
		calls = 0; fail_call = i;
		inspect(EIO);
		fail_call = 0; calls = 0; short_call = i;
		inspect(EIO);
		short_call = 0; calls = 0; over_call = i;
		inspect(EIO);
		over_call = 0;
	}
	/* Semantic corruption with recalculated checksums. */
	for (i = 0; i < 11; i++) {
		setup(true);
		make_record(0, 1, 32768, 512, 1, true, true);
		switch (i) {
		case 0: put32(record + 20, 1); break; /* LSN block */
		case 1: put32(record + 28, LOG_BLOCKS); break;
		case 2: record[304] ^= 1; break;
		case 3: put32(record + 40, 0); break;
		case 4: put32(record + 36, LOG_BLOCKS); break;
		case 5: put32(record + 12, 1024); break; /* truncated at head */
		case 6: record[512 + 8] = XFS_TRANSACTION; break;
		case 7: record[512 + 9] |= 1; break;
		case 8: record[512 + 12] = 0; break;
		case 9: record[512 + 10] = 1; break;
		case 10: put32(record + 512 + 4, 4); break;
		}
		seal(); publish();
		inspect(EINVAL);
	}
	/* Canonical zero-CRC record must match all otherwise unchecked bytes. */
	for (i = 0; i < 7; i++) {
		setup(true);
		make_record(0, 1, 32768, 512, 1, true, false);
		switch (i) {
		case 0: disk[48] = 1; break;
		case 1: disk[324] = 1; break;
		case 2: disk[511] = 1; break;
		case 3: disk[512 + 20] = 1; break;
		case 4: disk[44] ^= 1; break;
		case 5: make_record(2, 1, 32768, 512, 1, true, false); break;
		case 6: put32(disk + 320, 65536); break;
		}
		CHECK(xfs_require_clean_log(&reader, scratch, sizeof(scratch), &why) != 0);
		checks++;
	}
	setup(true);
	inspect(EOPNOTSUPP); /* Entirely zero log is never clean. */
	make_record(0, 1, 32768, 512, 1, true, true);
	put32(disk + 4 * 512, 1); /* write beyond a cycle hole */
	inspect(EOPNOTSUPP);
	memset(disk + 4 * 512, 0, 512);
	disk[4 * 512 + 100] = 1; /* zero stamp hiding nonzero contents */
	inspect(EINVAL);
	setup(true);
	make_record(0, 1, 32768, 512, 1, true, true);
	make_record(2, 1, 32768, 1024, 1, true, true);
	memset(disk + 4 * 512, 0, 512); /* latest unmount torn */
	inspect(EINVAL);
	setup(true);
	make_record(0, 1, 32768, 512, 1, true, true);
	make_record(2, 1, 32768, 512, 1, false, true);
	disk[3 * 512 + 100] ^= 1; /* cannot fall back to old clean record */
	inspect(EINVAL);
	setup(true);
	stamps(100, 8);
	make_record(98, 8, 32768, 512, 1, true, true);
	put32(disk + 100 * 512, 6); /* nonadjacent cycle */
	inspect(EOPNOTSUPP);
	setup(true);
	for (i = 0; i < LOG_BLOCKS; i++)
		put32(disk + i * 512, UINT32_MAX);
	make_record(LOG_BLOCKS - 2, UINT32_MAX, 32768, 512, 1, true, true);
	inspect(EOPNOTSUPP);
	/* Sector/stripe initial records, and disjoint log/metadata UUIDs. */
	for (size = 4096; size <= 262144; size *= 2) {
		setup(true);
		reader.sb.sb_logsunit = size;
		reader.sb.sb_versionnum |= XFS_SB_VERSION_SECTORBIT;
		reader.sb.sb_logsectlog = 12;
		reader.sb.sb_logsectsize = 4096;
		memset(&reader.sb.sb_meta_uuid, 0xcd, 16);
		length = size - (size > 32768 ? size / 32768 : 1) * 512;
		make_record(0, 1, size > 32768 ? size : 32768, length, 1, true, false);
		CHECK(inspect(0).initialized);
	}
	setup(true);
	make_record(0, 1, 32768, 512, 1, true, true);
	reader.sb.sb_logsectlog = 255;
	reader.sb.sb_versionnum |= XFS_SB_VERSION_SECTORBIT;
	inspect(EINVAL);
	setup(false);
	reader.sb.sb_versionnum &= ~XFS_SB_VERSION_LOGV2BIT;
	inspect(EOPNOTSUPP);
	setup(true);
	reader.sb.sb_logsunit = 513;
	inspect(EINVAL);
	CHECK(xfs_log_inspect(&reader, scratch, XFS_LOG_SCRATCH_SIZE - 1, &info, &why) == EMSGSIZE);
	CHECK(xfs_log_inspect(NULL, scratch, sizeof(scratch), &info, &why) == EINVAL);
	CHECK(xfs_log_inspect(&reader, NULL, sizeof(scratch), &info, &why) == EINVAL);
	CHECK(xfs_log_inspect(&reader, scratch, sizeof(scratch), NULL, &why) == EINVAL);
	printf("Passed %u journal checks plus clean-guard and argument checks\n", checks);
	return 0;
}
