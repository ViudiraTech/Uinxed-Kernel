/*
 *
 *		fat16.h
 *		fat16文件系统头文件
 *
 *		2024/10/12 By MicroFish
 *		基于 GPL-3.0 开源协议
 *		Copyright © 2020 ViudiraTech，保留最终解释权。
 *
 */

#ifndef INCLUDE_FAT16_H_
#define INCLUDE_FAT16_H_

#include "types.h"

#define SECTOR_SIZE 512
#define FAT1_SECTORS 32
#define ROOT_DIR_SECTORS 32
#define FAT1_START_LBA 1
#define ROOT_DIR_START_LBA 65
#define DATA_START_LBA 97
#define SECTOR_CLUSTER_BALANCE (DATA_START_LBA - 2)
#define MAX_FILE_NUM 512

typedef struct FILEINFO {
	uint8_t name[8], ext[3];
	uint8_t type, reserved[10];
	uint16_t time, date, clustno;
	uint32_t size;
} __attribute__((packed)) fileinfo_t;

typedef struct FAT_BPB_HEADER {
	unsigned char BS_jmpBoot[3];
	unsigned char BS_OEMName[8];
	unsigned short BPB_BytsPerSec;
	unsigned char BPB_SecPerClust;
	unsigned short BPB_RsvdSecCnt;
	unsigned char BPB_NumFATs;
	unsigned short BPB_RootEntCnt;
	unsigned short BPB_TotSec16;
	unsigned char BPB_Media;
	unsigned short BPB_FATSz16;
	unsigned short BPB_SecPerTrk;
	unsigned short BPB_NumHeads;
	unsigned int BPB_HiddSec;
	unsigned int BPB_TotSec32;
	unsigned char BS_DrvNum;
	unsigned char BS_Reserved1;
	unsigned char BS_BootSig;
	unsigned int BS_VolID;
	unsigned char BS_VolLab[11];
	unsigned char BS_FileSysType[8];
	unsigned char BS_BootCode[448];
	unsigned short BS_BootEndSig;
} __attribute__((packed)) bpb_hdr_t;

/* 硬盘格式化为fat16 */
int fat16_format(void);

/* 将文件名转为83式 */
int fat16_lfn2sfn(const char *lfn, char *sfn);

/* 读取根目录目录项 */
fileinfo_t *fat16_read_dir(int *dir_ents);

/* 创建文件 */
int fat16_create_file(fileinfo_t *finfo, char *filename);

/* 打开文件 */
int fat16_open_file(fileinfo_t *finfo, char *filename);

/* 获取第n个FAT项 */
static uint16_t get_nth_fat(uint16_t n);

/* 设置第n个FAT项 */
static void set_nth_fat(uint16_t n, uint16_t val);

/* 读取第n个clust */
static void read_nth_clust(uint16_t n, void *clust);

/* 写入第n个clust */
static void write_nth_clust(uint16_t n, const void *clust);

/* 读取文件 */
int fat16_read_file(fileinfo_t *finfo, void *buf);

/* 删除文件 */
int fat16_delete_file(char *filename);

/* 写入文件 */
int fat16_write_file(fileinfo_t *finfo, const void *buf, uint32_t size);

#endif // INCLUDE_FAT16_H_
