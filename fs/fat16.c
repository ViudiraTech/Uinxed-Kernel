/*
 *
 *		fat16.c
 *		fat16文件系统
 *
 *		2024/10/12 By MicroFish
 *		基于 GPL-3.0 开源协议
 *		Copyright © 2020 ViudiraTech，保留最终解释权。
 *
 */

#include "fat16.h"
#include "ide.h"
#include "memory.h"
#include "string.h"
#include "cmos.h"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wtype-limits"

/* 硬盘格式化为fat16 */
int fat16_format(void)
{
	char *fat1 = (char *)kmalloc(512);
	int sectors = ide_get_size();

	static unsigned char default_boot_code[] = {
		0x8c, 0xc8, 0x8e, 0xd8, 0x8e, 0xc0, 0xb8, 0x00, 0x06, 0xbb, 0x00, 0x07, 0xb9, 0x00, 0x00, 0xba,
		0x4f, 0x18, 0xcd, 0x10, 0xb6, 0x00, 0xe8, 0x02, 0x00, 0xeb, 0xfe, 0xb8, 0x6c, 0x7c, 0x89, 0xc5,
		0xb9, 0x2a, 0x00, 0xb8, 0x01, 0x13, 0xbb, 0x07, 0x00, 0xb2, 0x00, 0xcd, 0x10, 0xc3, 0x46, 0x41,
		0x54, 0x41, 0x4c, 0x3a, 0x20, 0x6e, 0x6f, 0x74, 0x20, 0x61, 0x20, 0x62, 0x6f, 0x6f, 0x74, 0x61,
		0x62, 0x6c, 0x65, 0x20, 0x64, 0x69, 0x73, 0x6b, 0x2e, 0x20, 0x53, 0x79, 0x73, 0x74, 0x65, 0x6d,
		0x20, 0x68, 0x61, 0x6c, 0x74, 0x65, 0x64, 0x2e, 0x00, 0x00
	};

	ide_read_secs(FAT1_START_LBA, fat1, 1);
	if (fat1[0] == 0xff) {
		kfree(fat1);
		return 1; // 存在fat16文件系统，无需格式化
	}
	kfree(fat1);

	bpb_hdr_t hdr;
	hdr.BS_jmpBoot[0] = 0xeb;
	hdr.BS_jmpBoot[1] = 0x3c;
	hdr.BS_jmpBoot[2] = 0x90;

	strcpy(hdr.BS_OEMName, "TUTORIAL");

	hdr.BPB_BytsPerSec = 512;
	hdr.BPB_SecPerClust = 1;
	hdr.BPB_RsvdSecCnt = 1;
	hdr.BPB_NumFATs = 2;
	hdr.BPB_RootEntCnt = 512;

	if (sectors < (1 << 16) - 1) {
		hdr.BPB_TotSec16 = sectors;
		hdr.BPB_TotSec32 = 0;
	} else {
		hdr.BPB_TotSec16 = 0;
		hdr.BPB_TotSec32 = sectors;
	}
	hdr.BPB_Media = 0xf8;
	hdr.BPB_FATSz16 = 32;
	hdr.BPB_SecPerTrk = 63;
	hdr.BPB_NumHeads = 16;
	hdr.BPB_HiddSec = 0;
	hdr.BS_DrvNum = 0x80;
	hdr.BS_Reserved1 = 0;
	hdr.BS_BootSig = 0x29;
	hdr.BS_VolID = 0;

	strcpy(hdr.BS_VolLab, "UxDISK");
	strcpy(hdr.BS_FileSysType, "FAT16   ");
	memset(hdr.BS_BootCode, 0, 448);
	memcpy(hdr.BS_BootCode, default_boot_code, sizeof(default_boot_code));

	hdr.BS_BootEndSig = 0xaa55;
	ide_write_secs(0, &hdr, 1);
	char initial_fat[512] = {0xff, 0xf8, 0xff, 0xff, 0};
	ide_write_secs(FAT1_START_LBA, &initial_fat, 1);
	ide_write_secs(FAT1_START_LBA + FAT1_SECTORS, &initial_fat, 1);
	return 0;
}

/* 将文件名转为83式 */
int fat16_lfn2sfn(const char *lfn, char *sfn)
{
	int len = strlen(lfn), last_dot = -1;
	for (int i = len - 1; i >= 0; i--) {
		if (lfn[i] == '.') {
			last_dot = i;
			break;
		}
	}
	if (last_dot == -1) last_dot = len;
	if (lfn[0] == '.') return -1;
	int len_name = last_dot, len_ext = len - 1 - last_dot;
	if (len_name > 8) return -1;
	if (len_ext > 3) return -1;
	char *name = (char *) kmalloc(10);
	char *ext = NULL;
	if (len_ext > 0) ext = (char *) kmalloc(5);
	memcpy(name, lfn, len_name);
	if (ext) memcpy(ext, lfn + last_dot + 1, len_ext);
	if (name[0] == 0xe5) name[0] = 0x05;
	for (int i = 0; i < len_name; i++) {
		if (name[i] == '.') return -1;
		if ((name[i] >= 'a' && name[i] <= 'z') || (name[i] >= 'A' && name[i] <= 'Z') || (name[i] >= '0' && name[i] <= '9')) sfn[i] = name[i];
		else sfn[i] = '_';
		if (sfn[i] >= 'a' && sfn[i] <= 'z') sfn[i] -= 0x20;
	}
	for (int i = len_name; i < 8; i++) sfn[i] = ' ';
	for (int i = 0; i < len_ext; i++) {
		if ((ext[i] >= 'a' && ext[i] <= 'z') || (ext[i] >= 'A' && name[i] <= 'Z') || (ext[i] >= '0' && ext[i] <= '9')) sfn[i + 8] = ext[i];
		else sfn[i + 8] = '_';
		if (sfn[i + 8] >= 'a' && sfn[i + 8] <= 'z') sfn[i + 8] -= 0x20;
	}
	if (len_ext > 0) {
		for (int i = len_ext; i < 3; i++) sfn[i + 8] = ' ';
	} else {
		for (int i = 0; i < 3; i++) sfn[i + 8] = ' ';
	}
	sfn[11] = 0;
	return 0;
}

/* 读取根目录目录项 */
fileinfo_t *fat16_read_dir(int *dir_ents)
{
	int i;

	fileinfo_t *root_dir = (fileinfo_t *) kmalloc(ROOT_DIR_SECTORS * SECTOR_SIZE);
	ide_read_secs(ROOT_DIR_START_LBA, root_dir, ROOT_DIR_SECTORS);
	for (i = 0; i < MAX_FILE_NUM; i++) {
		if (root_dir[i].name[0] == 0) break;
	}
	*dir_ents = i;
	return root_dir;
}

/* 创建文件 */
int fat16_create_file(fileinfo_t *finfo, char *filename)
{
	if (filename[0] == 0xe5) filename[0] = 0x05;
	char sfn[20] = {0};
	int ret = fat16_lfn2sfn(filename, sfn);
	if (ret) return -1;
	int entries;
	fileinfo_t *root_dir = fat16_read_dir(&entries);
	int free_slot = entries;

	for (int i = 0; i < entries; i++) {
		if (!memcmp(root_dir[i].name, sfn, 8) && !memcmp(root_dir[i].ext, sfn + 8, 3)) {
			kfree(root_dir);
			return -1;
		}
		if (root_dir[i].name[0] == 0xe5) {
			free_slot = i;
			break;
		}
	}
	if (free_slot == MAX_FILE_NUM) {
		kfree(root_dir);
		return -1;
	}
	memcpy(root_dir[free_slot].name, sfn, 8);
	memcpy(root_dir[free_slot].ext, sfn + 8, 3);
	root_dir[free_slot].type = 0x20;
	root_dir[free_slot].clustno = 0;
	root_dir[free_slot].size = 0;
	memset(root_dir[free_slot].reserved, 0, 10);
	root_dir[free_slot].date = ((get_year() - 1980) << 9) | (get_mon_hex() << 5) | get_day_of_month();
	root_dir[free_slot].time = (get_hour_hex() << 11) | (get_min_hex() << 5) | get_sec_hex();
	if (finfo) *finfo = root_dir[free_slot];
	ide_write_secs(ROOT_DIR_START_LBA, root_dir, ROOT_DIR_SECTORS);
	kfree(root_dir);
	return 0;
}

/* 打开文件 */
int fat16_open_file(fileinfo_t *finfo, char *filename)
{
	char sfn[20] = {0};
	int ret = fat16_lfn2sfn(filename, sfn);
	if (ret) return -1;
	int entries;
	fileinfo_t *root_dir = fat16_read_dir(&entries);
	int file_index = entries;

	for (int i = 0; i < entries; i++) {
		if (!memcmp(root_dir[i].name, sfn, 8) && !memcmp(root_dir[i].ext, sfn + 8, 3)) {
			file_index = i;
			break;
		}
	}
	if (file_index < entries) {
		*finfo = root_dir[file_index];
		kfree(root_dir);
		return 0;
	}
	else {
		finfo = NULL;
		kfree(root_dir);
		return -1;
	}
}

/* 获取第n个FAT项 */
static uint16_t get_nth_fat(uint16_t n)
{
	uint8_t *fat = (uint8_t *) kmalloc(512);
	uint32_t fat_start = FAT1_START_LBA;
	uint32_t fat_offset = n * 2;
	uint32_t fat_sect = fat_start + (fat_offset / 512);
	uint32_t sect_offset = fat_offset % 512;
	ide_read_secs(fat_sect, fat, 1);
	uint16_t table_val = *(uint16_t *) &fat[sect_offset];
	kfree(fat);
	return table_val;
}

/* 设置第n个FAT项 */
static void set_nth_fat(uint16_t n, uint16_t val)
{
	int fat_start = FAT1_START_LBA;
	int second_fat_start = FAT1_START_LBA + FAT1_SECTORS;
	uint8_t *fat = (uint8_t *) kmalloc(512);
	uint32_t fat_offset = n * 2;
	uint32_t fat_sect = fat_start + (fat_offset / 512);
	uint32_t second_fat_sect = second_fat_start + (fat_offset / 512);
	uint32_t sect_offset = fat_offset % 512;
	ide_read_secs(fat_sect, fat, 1);
	*(uint16_t *) &fat[sect_offset] = val;
	ide_write_secs(fat_sect, fat, 1);
	ide_write_secs(second_fat_sect, fat, 1);
	kfree(fat);
}

/* 读取第n个clust */
static void read_nth_clust(uint16_t n, void *clust)
{
	ide_read_secs(n + SECTOR_CLUSTER_BALANCE, clust, 1);
}

/* 写入第n个clust */
static void write_nth_clust(uint16_t n, const void *clust)
{
	ide_write_secs(n + SECTOR_CLUSTER_BALANCE, (void *)clust, 1);
}

/* 读取文件 */
int fat16_read_file(fileinfo_t *finfo, void *buf)
{
	uint16_t clustno = finfo->clustno;
	char *clust = (char *) kmalloc(512);
	do {
		read_nth_clust(clustno, clust);
		memcpy(buf, clust, 512);
		buf += 512;
		clustno = get_nth_fat(clustno);
		if (clustno >= 0xFFF8) break;
	} while (1);
	kfree(clust);
	return 0;
}

/* 删除文件 */
int fat16_delete_file(char *filename)
{
	char sfn[20] = {0};
	int ret = fat16_lfn2sfn(filename, sfn);
	if (ret) return -1;
	int entries;
	fileinfo_t *root_dir = fat16_read_dir(&entries);
	int file_ind = -1;

	for (int i = 0; i < entries; i++) {
		if (!memcmp(root_dir[i].name, sfn, 8) && !memcmp(root_dir[i].ext, sfn + 8, 3)) {
			file_ind = i;
			break;
		}
	}
	if (file_ind == -1) {
		kfree(root_dir);
		return -1;
	}
	root_dir[file_ind].name[0] = 0xe5;
	ide_write_secs(ROOT_DIR_START_LBA, root_dir, ROOT_DIR_SECTORS);
	kfree(root_dir);
	if (root_dir[file_ind].clustno == 0) {
		return 0;
	}
	unsigned short clustno = root_dir[file_ind].clustno, next_clustno;
	while (1) {
		next_clustno = get_nth_fat(clustno);
		set_nth_fat(clustno, 0);
		if (next_clustno >= 0xfff8) break;
		clustno = next_clustno;
	}
	return 0;
}

/* 写入文件 */
int fat16_write_file(fileinfo_t *finfo, const void *buf, uint32_t size)
{
	uint16_t clustno = finfo->clustno, next_clustno;
	if (finfo->size == 0 && finfo->clustno == 0) {
		clustno = 2;
		while (1) {
			if (get_nth_fat(clustno) == 0) {
				finfo->clustno = clustno;
				break;
			}
			clustno++;
		}
	}
	finfo->size = size;
	int write_sects = (size + 511) / 512;
	while (write_sects) {
		write_nth_clust(clustno, buf);
		write_sects--;
		buf += 512;
		next_clustno = get_nth_fat(clustno);
		if (next_clustno == 0 || next_clustno >= 0xfff8) {
			next_clustno = clustno + 1;
			while (1) {
				if (get_nth_fat(next_clustno) == 0) {
					set_nth_fat(clustno, next_clustno);
					break;
				} else next_clustno++;
			}
		}
		clustno = next_clustno;
	}
	finfo->date = ((get_year() - 1980) << 9) | (get_mon_hex() << 5) | get_day_of_month();
	finfo->time = (get_hour_hex() << 11) | (get_min_hex() << 5) | get_sec_hex();
	int entries;
	fileinfo_t *root_dir = fat16_read_dir(&entries);
	for (int i = 0; i < entries; i++) {
		if (!memcmp(root_dir[i].name, finfo->name, 8) && !memcmp(root_dir[i].ext, finfo->ext, 3)) {
			root_dir[i] = *finfo;
			break;
		}
	}
	ide_write_secs(ROOT_DIR_START_LBA, root_dir, ROOT_DIR_SECTORS);
	kfree(root_dir);
	return 0;
}

#pragma GCC diagnostic pop
