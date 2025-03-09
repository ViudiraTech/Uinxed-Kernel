/*
 *
 *		smbios.h
 *		系统管理BIOS头文件
 *
 *		2025/3/8 By MicroFish
 *		基于 GPL-3.0 开源协议
 *		Copyright © 2020 ViudiraTech，基于GPLv3协议。
 *
 */

#ifndef INCLUDE_SMBIOS_H_
#define INCLUDE_SMBIOS_H_

#include "stdint.h"

/* 32位入口点结构 */
struct EntryPoint32 {
	uint8_t AnchorString[4];				// 锚字符串，值为"_SM_"
	uint8_t Checksum;						// 校验和
	uint8_t EntryLength;					// 入口点结构长度
	uint8_t MajorVersion;					// SMBIOS主版本号
	uint8_t MinorVersion;					// SMBIOS次版本号
	uint16_t MaxStructureSize;				// 最大结构体大小
	uint8_t EntryPointRevision;				// 入口点修订号
	uint8_t FormattedArea[5];				// 格式化区域
	uint8_t IntermediateAnchorString[5];	// 中间锚字符串，值为"_DMI_"
	uint8_t IntermediateChecksum;			// 中间校验和
	uint16_t StructureTableLength;			// 结构表长度
	uint32_t StructureTableAddress;			// 结构表地址
	uint16_t NumberOfStructures;			// 结构数量
	uint8_t BCDRevision;					// BCD 修订号
};

/* 64位入口点结构 */
struct EntryPoint64 {
	uint8_t AnchorString[5];				// 锚字符串，值为"_SM3_"
	uint8_t Checksum;						// 校验和
	uint8_t EntryLength;					// 入口点结构长度
	uint8_t MajorVersion;					// SMBIOS主版本号
	uint8_t MinorVersion;					// SMBIOS次版本号
	uint8_t Docrev;							// 文档修订号
	uint8_t EntryPointRevision;				// 入口点修订号
	uint8_t Reserved;						// 保留字节
	uint32_t MaxStructureSize;				// 最大结构体大小
	uint64_t StructureTableAddress;			// 结构表地址
};

/* 结构表头部 */
struct Header {
	uint8_t type;							// 结构类型
	uint8_t length;							// 结构长度（不包括字符串表）
	uint16_t handle;						// 结构句柄
};

/* 获取SMBIOS入口点 */
void *smbios_entry(void);

/* 获取SMBIOS主版本 */
int smbios_major_version(void);

/* 获取SMBIOS次版本 */
int smbios_minor_version(void);

#endif // INCLUDE_SMBIOS_H_
