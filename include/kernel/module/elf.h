/*
 *
 *      elf.h
 *      ELF related header file
 *
 *      2025/5/4 By suhuajun
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_ELF_H_
#define INCLUDE_ELF_H_

#include <libs/std/stdint.h>

/* ELF magic */
#define ELF_MAGIC 0x464c457f

/* ELF file type */
#define ET_NONE 0
#define ET_REL  1
#define ET_EXEC 2
#define ET_DYN  3
#define ET_CORE 4

/* How to extract and insert information held in the st_info field. */
#define ELF32_ST_BIND(val)        (((unsigned char)(val)) >> 4)
#define ELF32_ST_TYPE(val)        ((val) & 0xf)
#define ELF32_ST_INFO(bind, type) (((bind) << 4) + ((type) & 0xf))

/* Both Elf32_Sym and Elf64_Sym use the same one-byte st_info field. */
#define ELF64_ST_BIND(val)        ELF32_ST_BIND(val)
#define ELF64_ST_TYPE(val)        ELF32_ST_TYPE(val)
#define ELF64_ST_INFO(bind, type) ELF32_ST_INFO((bind), (type))

/* Legal values for ST_BIND subfield of st_info (symbol binding). */
#define STB_LOCAL      0
#define STB_GLOBAL     1
#define STB_WEAK       2
#define STB_NUM        3
#define STB_LOOS       10
#define STB_GNU_UNIQUE 10
#define STB_HIOS       12
#define STB_LOPROC     13
#define STB_HIPROC     15

/* Legal values for ST_TYPE subfield of st_info (symbol type). */
#define STT_NOTYPE    0
#define STT_OBJECT    1
#define STT_FUNC      2
#define STT_SECTION   3
#define STT_FILE      4
#define STT_COMMON    5
#define STT_TLS       6
#define STT_NUM       7
#define STT_LOOS      10
#define STT_GNU_IFUNC 10
#define STT_HIOS      12
#define STT_LOPROC    13
#define STT_HIPROC    15

#define EI_NIDENT (16)

/* Program header types */
#define PT_NULL         0
#define PT_LOAD         1
#define PT_DYNAMIC      2
#define PT_INTERP       3
#define PT_NOTE         4
#define PT_SHLIB        5
#define PT_PHDR         6
#define PT_TLS          7
#define PT_GNU_EH_FRAME 0x6474e550
#define PT_GNU_STACK    0x6474e551
#define PT_GNU_RELRO    0x6474e552
#define PT_GNU_PROPERTY 0x6474e553

/* Program header flags */
#define PF_X (1 << 0)
#define PF_W (1 << 1)
#define PF_R (1 << 2)

/* Dynamic section tags */
#define DT_NULL            0
#define DT_NEEDED          1
#define DT_PLTRELSZ        2
#define DT_PLTGOT          3
#define DT_HASH            4
#define DT_STRTAB          5
#define DT_SYMTAB          6
#define DT_RELA            7
#define DT_RELASZ          8
#define DT_RELAENT         9
#define DT_STRSZ           10
#define DT_SYMENT          11
#define DT_INIT            12
#define DT_FINI            13
#define DT_SONAME          14
#define DT_RPATH           15
#define DT_SYMBOLIC        16
#define DT_REL             17
#define DT_RELSZ           18
#define DT_RELENT          19
#define DT_PLTREL          20
#define DT_DEBUG           21
#define DT_TEXTREL         22
#define DT_JMPREL          23
#define DT_BIND_NOW        24
#define DT_INIT_ARRAY      25
#define DT_FINI_ARRAY      26
#define DT_INIT_ARRAYSZ    27
#define DT_FINI_ARRAYSZ    28
#define DT_RUNPATH         29
#define DT_FLAGS           30
#define DT_ENCODING        32
#define DT_PREINIT_ARRAY   32
#define DT_PREINIT_ARRAYSZ 33
#define DT_SYMTAB_SHNDX    34
#define DT_NUM             35
#define DT_LOOS            0x6000000d
#define DT_HIOS            0x6ffff000
#define DT_LOPROC          0x70000000
#define DT_HIPROC          0x7fffffff
#define DT_GNU_HASH        0x6ffffef5
#define DT_VERSYM          0x6ffffff0
#define DT_VERNEED         0x6ffffffe
#define DT_VERNEEDNUM      0x6fffffff
#define DT_FLAGS_1         0x6ffffffb

/* DT_FLAGS values */
#define DF_ORIGIN     0x00000001
#define DF_SYMBOLIC   0x00000002
#define DF_TEXTREL    0x00000004
#define DF_BIND_NOW   0x00000008
#define DF_STATIC_TLS 0x00000010

/* DT_FLAGS_1 values */
#define DF_1_NOW        0x00000001
#define DF_1_GLOBAL     0x00000002
#define DF_1_GROUP      0x00000004
#define DF_1_NODELETE   0x00000008
#define DF_1_LOADFLTR   0x00000010
#define DF_1_INITFIRST  0x00000020
#define DF_1_NOOPEN     0x00000040
#define DF_1_ORIGIN     0x00000080
#define DF_1_DIRECT     0x00000100
#define DF_1_TRANS      0x00000200
#define DF_1_INTERPOSE  0x00000400
#define DF_1_NODEFLIB   0x00000800
#define DF_1_NODUMP     0x00001000
#define DF_1_CONFALT    0x00002000
#define DF_1_ENDFILTEE  0x00004000
#define DF_1_DISPRELDNE 0x00008000
#define DF_1_DISPRELPND 0x00010000
#define DF_1_NODIRECT   0x00020000
#define DF_1_IGNMULD    0x00040000
#define DF_1_OKSOEXT    0x00080000
#define DF_1_DUMP       0x00100000

/* x86_64 specific relocation types */
#define R_X86_64_NONE            0
#define R_X86_64_64              1
#define R_X86_64_PC32            2
#define R_X86_64_GOT32           3
#define R_X86_64_PLT32           4
#define R_X86_64_COPY            5
#define R_X86_64_GLOB_DAT        6
#define R_X86_64_JUMP_SLOT       7
#define R_X86_64_RELATIVE        8
#define R_X86_64_GOTPCREL        9
#define R_X86_64_32              10
#define R_X86_64_32S             11
#define R_X86_64_16              12
#define R_X86_64_PC16            13
#define R_X86_64_8               14
#define R_X86_64_PC8             15
#define R_X86_64_DTPMOD64        16
#define R_X86_64_DTPOFF64        17
#define R_X86_64_TPOFF64         18
#define R_X86_64_TLSGD           19
#define R_X86_64_TLSLD           20
#define R_X86_64_DTPOFF32        21
#define R_X86_64_GOTTPOFF        22
#define R_X86_64_TPOFF32         23
#define R_X86_64_PC64            24
#define R_X86_64_GOTOFF64        25
#define R_X86_64_GOTPC32         26
#define R_X86_64_GOT64           27
#define R_X86_64_GOTPCREL64      28
#define R_X86_64_GOTPC64         29
#define R_X86_64_GOTPLT64        30
#define R_X86_64_PLTOFF64        31
#define R_X86_64_SIZE32          32
#define R_X86_64_SIZE64          33
#define R_X86_64_GOTPC32_TLSDESC 34
#define R_X86_64_TLSDESC_CALL    35
#define R_X86_64_TLSDESC         36
#define R_X86_64_IRELATIVE       37

/* Section index constants */
#define SHN_UNDEF     0
#define SHN_LORESERVE 0xff00
#define SHN_LOPROC    0xff00
#define SHN_BEFORE    0xff00
#define SHN_AFTER     0xff01
#define SHN_HIPROC    0xff1f
#define SHN_LOOS      0xff20
#define SHN_HIOS      0xff3f
#define SHN_ABS       0xfff1
#define SHN_COMMON    0xfff2
#define SHN_XINDEX    0xffff
#define SHN_HIRESERVE 0xffff

/* Section types */
#define SHT_NULL          0
#define SHT_PROGBITS      1
#define SHT_SYMTAB        2
#define SHT_STRTAB        3
#define SHT_RELA          4
#define SHT_HASH          5
#define SHT_DYNAMIC       6
#define SHT_NOTE          7
#define SHT_NOBITS        8
#define SHT_REL           9
#define SHT_SHLIB         10
#define SHT_DYNSYM        11
#define SHT_INIT_ARRAY    14
#define SHT_FINI_ARRAY    15
#define SHT_PREINIT_ARRAY 16
#define SHT_GNU_HASH      0x6ffffff6
#define SHT_GNU_VERDEF    0x6ffffffd
#define SHT_GNU_VERNEED   0x6ffffffe
#define SHT_GNU_VERSYM    0x6fffffff

/* Section flags */
#define SHF_WRITE            (1 << 0)
#define SHF_ALLOC            (1 << 1)
#define SHF_EXECINSTR        (1 << 2)
#define SHF_MERGE            (1 << 4)
#define SHF_STRINGS          (1 << 5)
#define SHF_INFO_LINK        (1 << 6)
#define SHF_LINK_ORDER       (1 << 7)
#define SHF_OS_NONCONFORMING (1 << 8)
#define SHF_GROUP            (1 << 9)
#define SHF_TLS              (1 << 10)
#define SHF_COMPRESSED       (1 << 11)
#define SHF_MASKOS           0x0ff00000
#define SHF_MASKPROC         0xf0000000
#define SHF_ORDERED          0x40000000
#define SHF_EXCLUDE          0x80000000

/* Auxiliary vector types */
#define AT_NULL          0
#define AT_IGNORE        1
#define AT_EXECFD        2
#define AT_PHDR          3
#define AT_PHENT         4
#define AT_PHNUM         5
#define AT_PAGESZ        6
#define AT_BASE          7
#define AT_FLAGS         8
#define AT_ENTRY         9
#define AT_NOTELF        10
#define AT_UID           11
#define AT_EUID          12
#define AT_GID           13
#define AT_EGID          14
#define AT_PLATFORM      15
#define AT_HWCAP         16
#define AT_CLKTCK        17
#define AT_SECURE        23
#define AT_BASE_PLATFORM 24
#define AT_RANDOM        25
#define AT_HWCAP2        26
#define AT_EXECFN        31
#define AT_SYSINFO_EHDR  33

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
        unsigned char e_ident[EI_NIDENT];
        Elf64_Half    e_type;
        Elf64_Half    e_machine;
        Elf64_Word    e_version;
        Elf64_Addr    e_entry;
        Elf64_Off     e_phoff;
        Elf64_Off     e_shoff;
        Elf64_Word    e_flags;
        Elf64_Half    e_ehsize;
        Elf64_Half    e_phentsize;
        Elf64_Half    e_phnum;
        Elf64_Half    e_shentsize;
        Elf64_Half    e_shnum;
        Elf64_Half    e_shstrndx;
} Elf64_Ehdr;

/* Program header. */
typedef struct {
        uint32_t type;
        uint32_t flags;
        uint64_t offset;
        uint64_t vaddr;
        uint64_t paddr;
        uint64_t filesz;
        uint64_t memsz;
        uint64_t align;
} Elf64_Phdr;

/* Section header. */
typedef struct {
        Elf64_Word  sh_name;
        Elf64_Word  sh_type;
        Elf64_Xword sh_flags;
        Elf64_Addr  sh_addr;
        Elf64_Off   sh_offset;
        Elf64_Xword sh_size;
        Elf64_Word  sh_link;
        Elf64_Word  sh_info;
        Elf64_Xword sh_addralign;
        Elf64_Xword sh_entsize;
} Elf64_Shdr;

/* Symbol table entry. */
typedef struct {
        Elf64_Word    st_name;
        unsigned char st_info;
        unsigned char st_other;
        Elf64_Section st_shndx;
        Elf64_Addr    st_value;
        Elf64_Xword   st_size;
} Elf64_Sym;

/* Dynamic section entry. */
typedef struct {
        Elf64_Xword d_tag;
        union {
                Elf64_Xword d_val;
                Elf64_Addr  d_ptr;
        } d_un;
} Elf64_Dyn;

/* Relocation entry (explicit addend). */
typedef struct {
        Elf64_Addr   r_offset;
        Elf64_Xword  r_info;
        Elf64_Sxword r_addend;
} Elf64_Rela;

/* Relocation entry (implicit addend). */
typedef struct {
        Elf64_Addr  r_offset;
        Elf64_Xword r_info;
} Elf64_Rel;

/* Extract symbol index and relocation type from r_info. */
#define ELF64_R_SYM(i)     ((i) >> 32)
#define ELF64_R_TYPE(i)    ((i) & 0xffffffffL)
#define ELF64_R_INFO(s, t) (((s) << 32) + (t))

#endif // INCLUDE_ELF_H_
