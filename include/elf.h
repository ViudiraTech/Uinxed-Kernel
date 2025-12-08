/*
 *
 *      elf.h
 *      ELF related header file
 *
 *      2025/5/4 By suhuajun
 *      Based on Apache 2.0 open source license.
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_ELF_H_
#define INCLUDE_ELF_H_

#include <stdint.h>

/* How to extract and insert information held in the st_info field. */
#define ELF32_ST_BIND(val)        (((unsigned char)(val)) >> 4)
#define ELF32_ST_TYPE(val)        ((val) & 0xf)
#define ELF32_ST_INFO(bind, type) (((bind) << 4) + ((type) & 0xf))

/* Both Elf32_Sym and Elf64_Sym use the same one-byte st_info field. */
#define ELF64_ST_BIND(val)        ELF32_ST_BIND(val)
#define ELF64_ST_TYPE(val)        ELF32_ST_TYPE(val)
#define ELF64_ST_INFO(bind, type) ELF32_ST_INFO((bind), (type))

/* Legal values for ST_BIND subfield of st_info (symbol binding). */
#define STB_LOCAL      0  // Local symbol
#define STB_GLOBAL     1  // Global symbol
#define STB_WEAK       2  // Weak symbol
#define STB_NUM        3  // Number of defined types.
#define STB_LOOS       10 // Start of OS-specific
#define STB_GNU_UNIQUE 10 // Unique symbol.
#define STB_HIOS       12 // End of OS-specific
#define STB_LOPROC     13 // Start of processor-specific
#define STB_HIPROC     15 // End of processor-specific

/* Legal values for ST_TYPE subfield of st_info (symbol type). */
#define STT_NOTYPE    0  // Symbol type is unspecified
#define STT_OBJECT    1  // Symbol is a data object
#define STT_FUNC      2  // Symbol is a code object
#define STT_SECTION   3  // Symbol associated with a section
#define STT_FILE      4  // Symbol's name is file name
#define STT_COMMON    5  // Symbol is a common data object
#define STT_TLS       6  // Symbol is thread-local data object
#define STT_NUM       7  // Number of defined types.
#define STT_LOOS      10 // Start of OS-specific
#define STT_GNU_IFUNC 10 // Symbol is indirect code object
#define STT_HIOS      12 // End of OS-specific
#define STT_LOPROC    13 // Start of processor-specific
#define STT_HIPROC    15 // End of processor-specific

#define EI_NIDENT (16)

/* Type for a 16-bit quantity. */
typedef uint16_t Elf32_Half;
typedef uint16_t Elf64_Half;

/* Types for signed and unsigned 32-bit quantities. */
typedef uint32_t Elf32_Word;
typedef int32_t  Elf32_Sword;
typedef uint32_t Elf64_Word;
typedef int32_t  Elf64_Sword;

/* Types for signed and unsigned 64-bit quantities. */
typedef uint64_t Elf32_Xword;
typedef int64_t  Elf32_Sxword;
typedef uint64_t Elf64_Xword;
typedef int64_t  Elf64_Sxword;

/* Type of addresses. */
typedef uint32_t Elf32_Addr;
typedef uint64_t Elf64_Addr;

/* Type of file offsets. */
typedef uint32_t Elf32_Off;
typedef uint64_t Elf64_Off;

/* Type for section indices, which are 16-bit quantities. */
typedef uint16_t Elf32_Section;
typedef uint16_t Elf64_Section;

/* Type for version symbol information. */
typedef Elf32_Half Elf32_Versym;
typedef Elf64_Half Elf64_Versym;

/* The ELF file header. This appears at the start of every ELF file. */
typedef struct {
        unsigned char e_ident[EI_NIDENT]; // Magic number and other info
        Elf64_Half    e_type;             // Object file type
        Elf64_Half    e_machine;          // Architecture
        Elf64_Word    e_version;          // Object file version
        Elf64_Addr    e_entry;            // Entry point virtual address
        Elf64_Off     e_phoff;            // Program header table file offset
        Elf64_Off     e_shoff;            // Section header table file offset
        Elf64_Word    e_flags;            // Processor-specific flags
        Elf64_Half    e_ehsize;           // ELF header size in bytes
        Elf64_Half    e_phentsize;        // Program header table entry size
        Elf64_Half    e_phnum;            // Program header table entry count
        Elf64_Half    e_shentsize;        // Section header table entry size
        Elf64_Half    e_shnum;            // Section header table entry count
        Elf64_Half    e_shstrndx;         // Section header string table index
} Elf64_Ehdr;

/* Section header. */
typedef struct {
        Elf64_Word  sh_name;      // Section name (string tbl index)
        Elf64_Word  sh_type;      // Section type
        Elf64_Xword sh_flags;     // Section flags
        Elf64_Addr  sh_addr;      // Section virtual addr at execution
        Elf64_Off   sh_offset;    // Section file offset
        Elf64_Xword sh_size;      // Section size in bytes
        Elf64_Word  sh_link;      // Link to another section
        Elf64_Word  sh_info;      // Additional section information
        Elf64_Xword sh_addralign; // Section alignment
        Elf64_Xword sh_entsize;   // Entry size if section holds table
} Elf64_Shdr;

/* Symbol table entry. */
typedef struct {
        Elf64_Word    st_name;  // Symbol name (string tbl index)
        unsigned char st_info;  // Symbol type and binding
        unsigned char st_other; // Symbol visibility
        Elf64_Section st_shndx; // Section index
        Elf64_Addr    st_value; // Symbol value
        Elf64_Xword   st_size;  // Symbol size
} Elf64_Sym;

#endif // INCLUDE_ELF_H_
