/*
 *   CRC calculation code (C) Copyright IBM Corp. 2002, 2004
 *   BBR table creation code (C) Copyright Yang Yuanzhi 2013
 *
 *   This program is free software;  you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY;  without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See
 *   the GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program;  if not, write to the Free Software
 *   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 *
 * linux/drivers/md/dm-bbr-table.c
 *
 * Bad-block-relocation (BBR) table creator.
 *
 * The BBR target is designed to remap I/O write failures to another safe
 * location on disk. Note that most disk drives have BBR built into them,
 * this means that our software BBR will be only activated when all hardware
 * BBR replacement sectors have been used.
 */

#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <asm/byteorder.h>
#include <asm-generic/errno-base.h>

#include "dm-bbr.h"

#define SECTOR_SIZE	512
#define	MAX_SEEK	(LONG_MAX >> 9)

static u32 crc_table[256];

static void build_crc_table(void)
{
	u32 i, j, crc;

	for (i = 0; i <= 255; i++) {
		crc = i;
		for (j = 8; j > 0; j--) {
			if (crc & 1)
				crc = (crc >> 1) ^ CRC_POLYNOMIAL;
			else
				crc >>= 1;
		}
		crc_table[i] = crc;
	}
}

static u32 calculate_crc(u32 crc, void *buffer, u32 buffersize)
{
	unsigned char *current_byte;
	u32 temp1, temp2, i;

	current_byte = (unsigned char *) buffer;
	/* Process each byte in the buffer. */
	for (i = 0; i < buffersize; i++) {
		temp1 = (crc >> 8) & 0x00FFFFFF;
		temp2 = crc_table[(crc ^ (u32) * current_byte) &
				  (u32) 0xff];
		current_byte++;
		crc = temp1 ^ temp2;
	}
	return crc;
}

static int write_bbr_table(FILE *f, const char *device, struct bbr_table *table,
	u64 table_lsn, u64 nr_sects)
{
	int i;

	if (fseek(f, 0, SEEK_SET)) {
		printf("Failed to seek %s to beginning\n", device);
		return ferror(f);
	}

	while (table_lsn > MAX_SEEK) {
		if (fseek(f, MAX_SEEK << 9, SEEK_CUR)) {
			printf("Failed to seek %s for table\n", device);
			return ferror(f);
		}

		table_lsn -= MAX_SEEK;
	}

	if (fseek(f, table_lsn << 9, SEEK_CUR)) {
		printf("Failed to seek %s\n", device);
		return ferror(f);
	}

	for (i = 0; i < nr_sects; i++) {
		if (fwrite(table, SECTOR_SIZE, 1, f) != 1) {
			printf("Failed to write bbr table for %s\n", device);
			return ferror(f);
		}
	}

	return 0;
}

int main(int argc, char **argv)
{
	struct bbr_table *table = malloc(SECTOR_SIZE);
	int rc = 0;
	FILE *f;
	u64 table1_lsn, table2_lsn, nr_sects_bbr_table;
	int i;

	if (!table) {
		printf("Failed to allocate bbr_table buffer\n");
		return -ENOMEM;
	}

	if (argc != 5) {
		printf("Usage: %s device table1_lsn table2_lsn nr_sects_bbr_table\n", argv[0]);
		rc = -EINVAL;
		goto out;
	}

	sscanf(argv[2], PFU64, &table1_lsn);
	sscanf(argv[3], PFU64, &table2_lsn);
	sscanf(argv[4], PFU64, &nr_sects_bbr_table);
	f = fopen(argv[1], "wb");
	if (!f) {
		printf("Failed to open %s\n", argv[1]);
		rc = -EINVAL;
		goto out;
	}

	build_crc_table();
	table->signature = __constant_cpu_to_le32(BBR_TABLE_SIGNATURE);
	table->crc = 0;
	table->sequence_number = 0;
	table->in_use_cnt = 0;
	for (i = 0; i < BBR_ENTRIES_PER_SECT; i++) {
		table->entries[i].bad_sect = 0;
		table->entries[i].replacement_sect = 0;
	}
	table->crc = calculate_crc(INITIAL_CRC, table, SECTOR_SIZE);
	__cpu_to_le32s(&table->crc);

	rc = write_bbr_table(f, argv[1], table, table1_lsn, nr_sects_bbr_table);
	if (!rc)
		rc = write_bbr_table(f, argv[1], table, table2_lsn,
			nr_sects_bbr_table);

	fclose(f);
out:
	free(table);
	return rc;
}
