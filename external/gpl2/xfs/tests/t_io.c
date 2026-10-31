/* SPDX-License-Identifier: GPL-2.0-only */
/* $NetBSD$ */
#include "fixture.h"
#include <errno.h>

static struct {
	union sector primary, agf, agi, agfl;
	uint64_t agbase, offset;
	size_t length;
	unsigned int calls, fail_call, short_call, over_call;
	int error;
} device;
static struct xfs_reader reader;
static unsigned int checks;

static int
fake_read(void *cookie, uint64_t offset, void *data, size_t length, size_t *done)
{
	const void *source = NULL;
	unsigned int sector = be16_to_cpu(device.primary.sb.sb_sectsize);

	CHECK(cookie == &device);
	device.calls++;
	device.offset = offset;
	device.length = length;
	if (device.calls == device.fail_call)
		return device.error;
	if (offset == 0)
		source = device.primary.bytes;
	else if (offset == device.agbase + sector)
		source = device.agf.bytes;
	else if (offset == device.agbase + 2 * sector)
		source = device.agi.bytes;
	else if (offset == device.agbase + 3 * sector)
		source = device.agfl.bytes;
	if (source != NULL) {
		CHECK(length <= sizeof(union sector));
		memcpy(data, source, length);
	} else {
		memset(data, 0x5a, length);
	}
	*done = length;
	if (device.calls == device.short_call)
		*done = length - 1;
	if (device.calls == device.over_call)
		*done = length + 1;
	return 0;
}

static void
ag_checksums(void)
{
	if (XFS_SB_VERSION_NUM(&reader.sb) != 5)
		return;
	metadata_checksum(device.agf.bytes, reader.sb.sb_sectsize, XFS_AGF_CRC_OFF);
	metadata_checksum(device.agi.bytes, reader.sb.sb_sectsize, XFS_AGI_CRC_OFF);
	metadata_checksum(device.agfl.bytes, reader.sb.sb_sectsize, XFS_AGFL_CRC_OFF);
}

static void
setup(bool v5, uint32_t agno, unsigned int sector, bool short_last)
{
	struct xfs_io io = { &device, IMAGE_BYTES, fake_read };
	uint8_t scratch[XFS_MAX_SECTORSIZE];
	uint32_t blocks, first, size, entry;
	const char *why;
	unsigned int i;

	memset(&device, 0, sizeof(device));
	fixture(&device.primary, v5);
	if (short_last)
		device.primary.sb.sb_dblocks = cpu_to_be64(75000);
	if (sector == 4096) {
		device.primary.sb.sb_sectsize = cpu_to_be16(4096);
		device.primary.sb.sb_sectlog = 12;
	}
	if (v5) {
		device.primary.sb.sb_features_incompat |=
		    cpu_to_be32(XFS_SB_FEAT_INCOMPAT_META_UUID);
		device.primary.sb.sb_features_ro_compat =
		    cpu_to_be32(XFS_SB_FEAT_RO_COMPAT_ALL);
		checksum(&device.primary);
	}
	CHECK(xfs_reader_open(&io, scratch, sizeof(scratch), &reader, &why) == 0);
	CHECK(why == NULL);
	CHECK(xfs_ag_geometry(&reader.sb, agno, &device.agbase, &blocks) == 0);
	first = (sector * 4 + 4095) / 4096;
	device.agf.agf.agf_magicnum = cpu_to_be32(XFS_AGF_MAGIC);
	device.agf.agf.agf_versionnum = cpu_to_be32(1);
	device.agf.agf.agf_seqno = cpu_to_be32(agno);
	device.agf.agf.agf_length = cpu_to_be32(blocks);
	device.agf.agf.agf_bno_root = cpu_to_be32(first);
	device.agf.agf.agf_cnt_root = cpu_to_be32(first + 1);
	device.agf.agf.agf_bno_level = device.agf.agf.agf_cnt_level = cpu_to_be32(1);
	device.agf.agf.agf_rmap_root = cpu_to_be32(first + 4);
	device.agf.agf.agf_rmap_level = cpu_to_be32(1);
	device.agf.agf.agf_refcount_root = cpu_to_be32(first + 5);
	device.agf.agf.agf_refcount_level = cpu_to_be32(1);
	device.agf.agf.agf_freeblks = cpu_to_be32(1000);
	device.agf.agf.agf_longest = cpu_to_be32(900);
	device.agf.agf.agf_fllast = cpu_to_be32(1);
	device.agf.agf.agf_flcount = cpu_to_be32(2);
	memcpy(&device.agf.agf.agf_uuid, &reader.sb.sb_meta_uuid, 16);
	device.agi.agi.agi_magicnum = cpu_to_be32(XFS_AGI_MAGIC);
	device.agi.agi.agi_versionnum = cpu_to_be32(1);
	device.agi.agi.agi_seqno = cpu_to_be32(agno);
	device.agi.agi.agi_length = cpu_to_be32(blocks);
	device.agi.agi.agi_root = cpu_to_be32(first + 2);
	device.agi.agi.agi_level = cpu_to_be32(1);
	device.agi.agi.agi_free_root = cpu_to_be32(first + 3);
	device.agi.agi.agi_free_level = cpu_to_be32(1);
	device.agi.agi.agi_count = cpu_to_be32(64);
	device.agi.agi.agi_freecount = cpu_to_be32(61);
	for (i = 0; i < XFS_AGI_UNLINKED_BUCKETS; i++)
		device.agi.agi.agi_unlinked[i] = cpu_to_be32(NULLAGINO);
	memcpy(&device.agi.agi.agi_uuid, &reader.sb.sb_meta_uuid, 16);
	memset(device.agfl.bytes, v5 ? 0xff : 0xa5, sizeof(device.agfl));
	if (v5) {
		device.agfl.agfl.agfl_magicnum = cpu_to_be32(XFS_AGFL_MAGIC);
		device.agfl.agfl.agfl_seqno = cpu_to_be32(agno);
		device.agfl.agfl.agfl_lsn = 0;
		memcpy(&device.agfl.agfl.agfl_uuid, &reader.sb.sb_meta_uuid, 16);
	}
	size = v5 ? sizeof(struct xfs_agfl) : 0;
	for (i = 0; i < 2; i++) {
		entry = cpu_to_be32(first + 6 + i);
		memcpy(device.agfl.bytes + size + i * 4, &entry, 4);
	}
	ag_checksums();
	device.calls = 0;
}

static void
expect_ag(uint32_t agno, int error, const char *message)
{
	uint8_t scratch[XFS_MAX_SECTORSIZE + 1];
	struct xfs_ag_headers result, before;
	const char *why;
	int actual;

	memset(&result, 0xa5, sizeof(result));
	memcpy(&before, &result, sizeof(before));
	actual = xfs_read_ag(&reader, agno, scratch + 1, sizeof(scratch) - 1,
	    &result, &why);
	if (actual != error)
		fprintf(stderr, "AG: expected %d, got %d (%s)\n", error, actual,
		    why != NULL ? why : "success");
	CHECK(actual == error);
	CHECK((why == NULL) == (error == 0));
	if (error != 0) {
		CHECK(memcmp(&result, &before, sizeof(before)) == 0);
		if (message != NULL)
			CHECK(strstr(why, message) != NULL);
	} else {
		CHECK(be32_to_cpu(result.agf.agf_seqno) == agno);
		CHECK(be32_to_cpu(result.agi.agi_seqno) == agno);
	}
	checks++;
}

int
main(void)
{
	uint8_t scratch[XFS_MAX_SECTORSIZE];
	struct xfs_reader result, before;
	struct xfs_ag_headers ag;
	struct xfs_io io;
	const char *why;
	uint64_t offset;
	uint32_t blocks, entry;
	unsigned int i, j, n, size;
	union sector *sector;

	setup(true, 0, 512, false);
	expect_ag(0, 0, NULL);
	CHECK(device.calls == 3);
	setup(true, 2, 4096, false);
	expect_ag(2, 0, NULL);
	device.agi.bytes[4095] ^= 1;
	expect_ag(2, EINVAL, "CRC32C");
	setup(true, 3, 512, true);
	expect_ag(3, 0, NULL);
	CHECK(xfs_ag_geometry(&reader.sb, 3, &offset, &blocks) == 0);
	CHECK(blocks == 15000 && offset == UINT64_C(60000) * 4096);
	device.calls = 0;
	expect_ag(4, EINVAL, "geometry");
	CHECK(device.calls == 0);
	reader.sb.sb_dblocks = UINT64_C(60000) + 63;
	CHECK(xfs_ag_geometry(&reader.sb, 3, &offset, &blocks) == EINVAL);
	reader.sb.sb_dblocks = 75000;
	CHECK(xfs_read_ag(&reader, 3, scratch, 511, &ag, &why) == EMSGSIZE);
	CHECK(device.calls == 0);
	CHECK(xfs_read_ag(NULL, 0, scratch, sizeof(scratch), &ag, NULL) == EINVAL);
	CHECK(xfs_read_ag(&reader, 0, NULL, sizeof(scratch), &ag, NULL) == EINVAL);

	/* Fail each individual metadata read; no partial result may escape. */
	for (i = 1; i <= 3; i++) {
		for (j = 0; j < 4; j++) {
			setup(true, 0, 512, false);
			if (j == 0 || j == 3) {
				device.fail_call = i;
				device.error = j == 0 ? EACCES : -1;
			} else if (j == 1)
				device.short_call = i;
			else
				device.over_call = i;
			expect_ag(0, j == 0 ? EACCES : EIO, "read failed");
			CHECK(device.calls == i);
		}
	}
	setup(true, 0, 512, false);
	for (i = 0; i < 3; i++) {
		sector = i == 0 ? &device.agf : (i == 1 ? &device.agi : &device.agfl);
		for (j = 0; j < 512; j++) {
			sector->bytes[j] ^= 1;
			expect_ag(0, EINVAL, "CRC32C");
			sector->bytes[j] ^= 1;
		}
	}

#define BAD(kind, field, value, message) do { \
	setup(true, 0, 512, false); \
	device.kind.kind.field = cpu_to_be32(value); \
	ag_checksums(); \
	expect_ag(0, EINVAL, message); \
} while (0)
	BAD(agf, agf_magicnum, 0, "magic");
	BAD(agf, agf_versionnum, 0, "version");
	BAD(agf, agf_seqno, 1, "mismatch");
	BAD(agf, agf_length, 19999, "mismatch");
	BAD(agf, agf_flfirst, 119, "index");
	BAD(agf, agf_fllast, 119, "index");
	BAD(agf, agf_flcount, 120, "index");
	BAD(agf, agf_flcount, 1, "reset required");
	BAD(agf, agf_longest, 1001, "counters");
	BAD(agf, agf_freeblks, 20001, "counters");
	BAD(agf, agf_bno_root, 0, "btree");
	BAD(agf, agf_cnt_root, 20000, "btree");
	BAD(agf, agf_bno_level, 0, "btree");
	BAD(agf, agf_cnt_level, UINT32_MAX, "btree");
	BAD(agf, agf_btreeblks, 20001, "count");
	BAD(agf, agf_rmap_root, 20000, "reverse-map");
	BAD(agf, agf_rmap_level, UINT32_MAX, "reverse-map");
	BAD(agf, agf_rmap_blocks, 20001, "reverse-map");
	BAD(agf, agf_refcount_root, 20000, "reference-count");
	BAD(agf, agf_refcount_level, 0, "reference-count");
	BAD(agf, agf_refcount_blocks, 20001, "reference-count");
	BAD(agi, agi_magicnum, 0, "magic");
	BAD(agi, agi_versionnum, 0, "version");
	BAD(agi, agi_seqno, 1, "mismatch");
	BAD(agi, agi_length, 19999, "mismatch");
	BAD(agi, agi_count, UINT32_MAX, "counters");
	BAD(agi, agi_freecount, 65, "counters");
	BAD(agi, agi_root, 0, "inode btree");
	BAD(agi, agi_level, 0, "inode btree");
	BAD(agi, agi_free_root, 20000, "free-inode");
	BAD(agi, agi_free_level, UINT32_MAX, "free-inode");
	BAD(agi, agi_iblocks, 20001, "counters");
	BAD(agi, agi_fblocks, 20001, "counters");
	BAD(agi, agi_unlinked[0], 0, "unlinked");
	BAD(agi, agi_unlinked[63], 20000 * 8, "unlinked");
	BAD(agfl, agfl_magicnum, 0, "magic");
	BAD(agfl, agfl_seqno, 1, "mismatch");

	for (i = 0; i < 3; i++) {
		setup(true, 0, 512, false);
		if (i == 0)
			memset(&device.agf.agf.agf_uuid, 0, 16);
		else if (i == 1)
			memset(&device.agi.agi.agi_uuid, 0, 16);
		else
			memset(&device.agfl.agfl.agfl_uuid, 0, 16);
		ag_checksums();
		expect_ag(0, EINVAL, "UUID");
	}

	/* Wrapped, empty, and full free-list rings. */
	setup(true, 0, 512, false);
	size = (512 - sizeof(struct xfs_agfl)) / 4;
	device.agf.agf.agf_flfirst = cpu_to_be32(size - 1);
	device.agf.agf.agf_fllast = 0;
	entry = cpu_to_be32(20);
	memcpy(device.agfl.bytes + 512 - 4, &entry, 4);
	ag_checksums();
	expect_ag(0, 0, NULL);
	device.agf.agf.agf_flcount = 0;
	ag_checksums();
	expect_ag(0, 0, NULL);
	device.agf.agf.agf_flfirst = 0;
	device.agf.agf.agf_fllast = cpu_to_be32(size - 1);
	device.agf.agf.agf_flcount = cpu_to_be32(size);
	for (i = 0; i < size; i++) {
		entry = cpu_to_be32(100 + i);
		memcpy(device.agfl.bytes + sizeof(struct xfs_agfl) + i * 4, &entry, 4);
	}
	ag_checksums();
	expect_ag(0, 0, NULL);
	entry = cpu_to_be32(NULLAGBLOCK);
	memcpy(device.agfl.bytes + sizeof(struct xfs_agfl), &entry, 4);
	ag_checksums();
	expect_ag(0, EINVAL, "active AGFL");

	setup(false, 1, 512, false);
	expect_ag(1, 0, NULL); /* Inactive v4 slots contain deliberate garbage. */
	device.agf.agf.agf_flfirst = cpu_to_be32(10);
	device.agf.agf.agf_fllast = cpu_to_be32(10);
	device.agf.agf.agf_flcount = cpu_to_be32(1);
	expect_ag(1, EINVAL, "active AGFL");
	setup(true, 3, 512, true);
	entry = cpu_to_be32(16000);
	memcpy(device.agfl.bytes + sizeof(struct xfs_agfl) + 10 * 4, &entry, 4);
	ag_checksums();
	expect_ag(3, EINVAL, "out of bounds");

	/* Decode the AG/block address, not a linear filesystem block number. */
	setup(true, 0, 512, true);
	CHECK(xfs_read_blocks(&reader, (UINT64_C(2) << 15) | 100, 1,
	    scratch, sizeof(scratch), &why) == 0);
	CHECK(device.offset == UINT64_C(40100) * 4096 && device.length == 4096);
	CHECK(scratch[0] == 0x5a && scratch[4095] == 0x5a);
	n = device.calls;
	CHECK(xfs_read_blocks(&reader, (UINT64_C(3) << 15) | 14999, 2,
	    scratch, sizeof(scratch), &why) == EINVAL);
	CHECK(xfs_read_blocks(&reader, 25000, 1, scratch, sizeof(scratch), &why) == EINVAL);
	CHECK(xfs_read_blocks(&reader, UINT64_MAX, 1,
	    scratch, sizeof(scratch), &why) == EINVAL);
	CHECK(xfs_read_blocks(&reader, 100, 1, scratch, 4095, &why) == EMSGSIZE);
	CHECK(xfs_read_blocks(&reader, 100, 0, scratch, sizeof(scratch), NULL) == EINVAL);
	CHECK(device.calls == n);
	device.short_call = n + 1;
	CHECK(xfs_read_blocks(&reader, 100, 1, scratch, sizeof(scratch), NULL) == EIO);

	/* Read-open failures preserve the entire output context. */
	for (i = 1; i <= 2; i++) {
		setup(true, 0, 4096, false);
		device.fail_call = i;
		device.error = EIO;
		memset(&result, 0xa5, sizeof(result));
		memcpy(&before, &result, sizeof(result));
		CHECK(xfs_reader_open(&reader.io, scratch, sizeof(scratch), &result, &why) == EIO);
		CHECK(memcmp(&before, &result, sizeof(result)) == 0);
		CHECK(device.calls == i);
	}
	setup(true, 0, 4096, false);
	CHECK(xfs_reader_open(&reader.io, scratch, 512, &result, NULL) == EMSGSIZE);
	CHECK(device.calls == 1);
	setup(true, 0, 512, false);
	io = reader.io;
	CHECK(xfs_io_read(&io, io.size, NULL, 0) == 0);
	CHECK(xfs_io_read(&io, io.size, scratch, 1) == EMSGSIZE);
	CHECK(xfs_io_read(&io, io.size + 1, scratch, 0) == EMSGSIZE);
	CHECK(xfs_io_read(&io, 0, NULL, 1) == EINVAL);
	CHECK(xfs_io_read(NULL, 0, scratch, 1) == EINVAL);
	io.size = UINT64_MAX;
	CHECK(xfs_io_read(&io, INT64_MAX, scratch, 1) == EOVERFLOW);
	CHECK(xfs_io_read(&io, UINT64_MAX, scratch, 2) == EOVERFLOW);
	CHECK(device.calls == 0);
	CHECK(xfs_verify_metadata_crc(scratch, 3, 0) == EINVAL);
	CHECK(xfs_verify_metadata_crc(scratch, sizeof(scratch), SIZE_MAX) == EINVAL);
	CHECK(xfs_verify_metadata_crc(NULL, 512, 224) == EINVAL);
	printf("Passed %u AG validation cases plus I/O bounds and failure checks\n", checks);
	return 0;
}
