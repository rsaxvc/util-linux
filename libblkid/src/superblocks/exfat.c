/*
 * Copyright (C) 2010 Andrew Nayenko <resver@gmail.com>
 *
 * This file may be redistributed under the terms of the
 * GNU Lesser General Public License.
 */
#include "superblocks.h"

struct exfat_super_block {
	uint8_t jump[3];
	uint8_t oem_name[8];
	uint8_t	__unused1[53];
	uint64_t block_start;
	uint64_t block_count;
	uint32_t fat_block_start;
	uint32_t fat_block_count;
	uint32_t cluster_block_start;
	uint32_t cluster_count;
	uint32_t rootdir_cluster;
	uint8_t volume_serial[4];
	struct {
		uint8_t vermin;
		uint8_t vermaj;
	} version;
	uint16_t volume_state;
	uint8_t block_bits;
	uint8_t bpc_bits;
	uint8_t fat_count;
	uint8_t drive_no;
	uint8_t allocated_percent;
} __attribute__((__packed__));

struct exfat_entry_label {
	uint8_t type;
	uint8_t length;
	uint8_t name[22];
	uint8_t reserved[8];
} __attribute__((__packed__));

#define BLOCK_SIZE(sb) ((sb)->block_bits < 32 ? (1u << (sb)->block_bits) : 0)
#define CLUSTER_SIZE(sb) ((sb)->bpc_bits < 32 ? (BLOCK_SIZE(sb) << (sb)->bpc_bits) : 0)
#define EXFAT_FIRST_DATA_CLUSTER 2
#define EXFAT_LAST_DATA_CLUSTER 0xffffff6
#define EXFAT_ENTRY_SIZE 32

#define EXFAT_ENTRY_EOD		0x00
#define EXFAT_ENTRY_LABEL	0x83

static uint64_t block_to_offset(const struct exfat_super_block *sb,
		uint64_t block)
{
	return block << sb->block_bits;
}

static uint64_t cluster_to_block(const struct exfat_super_block *sb,
		uint32_t cluster)
{
	return le32_to_cpu(sb->cluster_block_start) +
			((uint64_t) (cluster - EXFAT_FIRST_DATA_CLUSTER)
					<< sb->bpc_bits);
}

static uint64_t cluster_to_offset(const struct exfat_super_block *sb,
		uint32_t cluster)
{
	return block_to_offset(sb, cluster_to_block(sb, cluster));
}

static uint32_t next_cluster(blkid_probe pr,
		const struct exfat_super_block *sb, uint32_t cluster)
{
	uint32_t *next;
	uint64_t fat_offset;

	fat_offset = block_to_offset(sb, le32_to_cpu(sb->fat_block_start))
		+ (uint64_t) cluster * sizeof(cluster);
	next = (uint32_t *) blkid_probe_get_buffer(pr, fat_offset,
			sizeof(uint32_t));
	if (!next)
		return 0;
	return le32_to_cpu(*next);
}

static struct exfat_entry_label *find_label(blkid_probe pr,
		const struct exfat_super_block *sb)
{
	uint32_t cluster = le32_to_cpu(sb->rootdir_cluster);
	uint64_t offset = cluster_to_offset(sb, cluster);
	uint8_t *entry;
	const size_t max_iter = 10000;
	size_t i = 0;

	for (; i < max_iter; i++) {
		entry = (uint8_t *) blkid_probe_get_buffer(pr, offset,
				EXFAT_ENTRY_SIZE);
		if (!entry)
			return NULL;
		if (entry[0] == EXFAT_ENTRY_EOD)
			return NULL;
		if (entry[0] == EXFAT_ENTRY_LABEL)
			return (struct exfat_entry_label *) entry;

		offset += EXFAT_ENTRY_SIZE;
		if (CLUSTER_SIZE(sb) && (offset % CLUSTER_SIZE(sb)) == 0) {
			cluster = next_cluster(pr, sb, cluster);
			if (cluster < EXFAT_FIRST_DATA_CLUSTER)
				return NULL;
			if (cluster > EXFAT_LAST_DATA_CLUSTER)
				return NULL;
			offset = cluster_to_offset(sb, cluster);
		}
	}

	return NULL;
}

/* From https://docs.microsoft.com/en-us/windows/win32/fileio/exfat-specification#34-main-and-backup-boot-checksum-sub-regions */
static uint32_t exfat_boot_checksum(unsigned char *sectors,
				    size_t sector_size)
{
	uint32_t n_bytes = sector_size * 11;
	uint32_t checksum = 0;

	for (size_t i = 0; i < n_bytes; i++) {
		if ((i == 106) || (i == 107) || (i == 112))
			continue;

		checksum = ((checksum & 1) ? 0x80000000 : 0) + (checksum >> 1)
			+ (uint32_t) sectors[i];
	}

	return checksum;
}

static int exfat_validate_checksum(blkid_probe pr,
		const struct exfat_super_block *sb)
{
	size_t sector_size = BLOCK_SIZE(sb);
	/* 11 sectors will be checksummed, the 12th contains the expected */
	unsigned char *data = blkid_probe_get_buffer(pr, 0, sector_size * 12);
	if (!data)
		return 0;

	uint32_t checksum = exfat_boot_checksum(data, sector_size);

	/* The expected checksum is repeated, check all of them */
	for (size_t i = 0; i < sector_size / sizeof(uint32_t); i++) {
		size_t offset = sector_size * 11 + i * 4;
		uint32_t *expected_addr = (uint32_t *) &data[offset];
		uint32_t expected = le32_to_cpu(*expected_addr);
		if (!blkid_probe_verify_csum(pr, checksum, expected))
			return 0;
	};

	return 1;
}

static int probe_exfat(blkid_probe pr, const struct blkid_idmag *mag)
{
	struct exfat_super_block *sb;
	struct exfat_entry_label *label;

	sb = blkid_probe_get_sb(pr, mag, struct exfat_super_block);
	if (!sb || !CLUSTER_SIZE(sb))
		return errno ? -errno : BLKID_PROBE_NONE;

	if (!exfat_validate_checksum(pr, sb))
		return BLKID_PROBE_NONE;

	label = find_label(pr, sb);
	if (label)
		blkid_probe_set_utf8label(pr, label->name,
				min((size_t) label->length * 2, sizeof(label->name)),
				UL_ENCODE_UTF16LE);
	else if (errno)
		return -errno;

	blkid_probe_sprintf_uuid(pr, sb->volume_serial, 4,
			"%02hhX%02hhX-%02hhX%02hhX",
			sb->volume_serial[3], sb->volume_serial[2],
			sb->volume_serial[1], sb->volume_serial[0]);

	blkid_probe_sprintf_version(pr, "%u.%u",
			sb->version.vermaj, sb->version.vermin);

	blkid_probe_set_fsblocksize(pr, BLOCK_SIZE(sb));
	blkid_probe_set_block_size(pr, BLOCK_SIZE(sb));

	return BLKID_PROBE_OK;
}

const struct blkid_idinfo exfat_idinfo =
{
	.name		= "exfat",
	.usage		= BLKID_USAGE_FILESYSTEM,
	.probefunc	= probe_exfat,
	.magics		=
	{
		{ .magic = "EXFAT   ", .len = 8, .sboff = 3 },
		{ NULL }
	}
};
