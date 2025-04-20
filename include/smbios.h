/*
 *
 *		smbios.h
 *		System Management BIOS Header File
 *
 *		2025/3/8 By MicroFish
 *		Based on GPL-3.0 open source agreement
 *		Copyright © 2020 ViudiraTech, based on the GPLv3 agreement.
 *
 */

#ifndef INCLUDE_SMBIOS_H_
#define INCLUDE_SMBIOS_H_

#include "stdint.h"

/* 32-bit entry point structure */
struct EntryPoint32 {
		uint8_t AnchorString[4];			 // Anchor string, value is "_SM_"
		uint8_t Checksum;					 // Checksum
		uint8_t EntryLength;				 // Entry point structure length
		uint8_t MajorVersion;				 // SMBIOS major version number
		uint8_t MinorVersion;				 // SMBIOS minor version number
		uint16_t MaxStructureSize;			 // Maximum structure size
		uint8_t EntryPointRevision;			 // Entry point revision number
		uint8_t FormattedArea[5];			 // Formatting Area
		uint8_t IntermediateAnchorString[5]; // Middle anchor string, value is "_DMI_"
		uint8_t IntermediateChecksum;		 // Intermediate Checksum
		uint16_t StructureTableLength;		 // Structure table length
		uint32_t StructureTableAddress;		 // Structure table address
		uint16_t NumberOfStructures;		 // Number of structures
		uint8_t BCDRevision;				 // BCD revision number
};

/* 64-bit entry point structure */
struct EntryPoint64 {
		uint8_t AnchorString[5];		// Anchor string, value is "_SM3_"
		uint8_t Checksum;				// Checksum
		uint8_t EntryLength;			// Entry point structure length
		uint8_t MajorVersion;			// SMBIOS major version number
		uint8_t MinorVersion;			// SMBIOS minor version number
		uint8_t Docrev;					// Document revision number
		uint8_t EntryPointRevision;		// Entry point revision number
		uint8_t Reserved;				// Reserved Bytes
		uint32_t MaxStructureSize;		// Maximum structure size
		uint64_t StructureTableAddress; // Structure table address
};

/* Structure table header */
struct Header {
		uint8_t type;	 // Structure Type
		uint8_t length;	 // Structure length (excluding string table)
		uint16_t handle; // Structure Handle
};

/* Get SMBIOS entry point */
void *smbios_entry(void);

/* Get the SMBIOS major version */
int smbios_major_version(void);

/* Get SMBIOS minor version */
int smbios_minor_version(void);

#endif // INCLUDE_SMBIOS_H_
