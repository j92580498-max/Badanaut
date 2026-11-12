#pragma once
// Minimal, self-contained ELF32 definitions.
//
// <elf.h> is a glibc header and does not exist on MSVC/Windows (nor on some
// other platforms). The HLE loader only needs the ARM32 ELF DYN subset, so we
// vendor exactly the types and constants used by elf_loader.cpp / cpu.cpp and
// use this header on every platform for deterministic layout.

#include <cstdint>

typedef uint32_t Elf32_Addr;
typedef uint16_t Elf32_Half;
typedef uint32_t Elf32_Off;
typedef int32_t Elf32_Sword;
typedef uint32_t Elf32_Word;

#define EI_NIDENT 16

typedef struct {
    unsigned char e_ident[EI_NIDENT];
    Elf32_Half e_type;
    Elf32_Half e_machine;
    Elf32_Word e_version;
    Elf32_Addr e_entry;
    Elf32_Off e_phoff;
    Elf32_Off e_shoff;
    Elf32_Word e_flags;
    Elf32_Half e_ehsize;
    Elf32_Half e_phentsize;
    Elf32_Half e_phnum;
    Elf32_Half e_shentsize;
    Elf32_Half e_shnum;
    Elf32_Half e_shstrndx;
} Elf32_Ehdr;

typedef struct {
    Elf32_Word p_type;
    Elf32_Off p_offset;
    Elf32_Addr p_vaddr;
    Elf32_Addr p_paddr;
    Elf32_Word p_filesz;
    Elf32_Word p_memsz;
    Elf32_Word p_flags;
    Elf32_Word p_align;
} Elf32_Phdr;

typedef struct {
    Elf32_Word sh_name;
    Elf32_Word sh_type;
    Elf32_Word sh_flags;
    Elf32_Addr sh_addr;
    Elf32_Off sh_offset;
    Elf32_Word sh_size;
    Elf32_Word sh_link;
    Elf32_Word sh_info;
    Elf32_Word sh_addralign;
    Elf32_Word sh_entsize;
} Elf32_Shdr;

typedef struct {
    Elf32_Sword d_tag;
    union {
        Elf32_Word d_val;
        Elf32_Addr d_ptr;
    } d_un;
} Elf32_Dyn;

typedef struct {
    Elf32_Word st_name;
    Elf32_Addr st_value;
    Elf32_Word st_size;
    unsigned char st_info;
    unsigned char st_other;
    Elf32_Half st_shndx;
} Elf32_Sym;

typedef struct {
    Elf32_Addr r_offset;
    Elf32_Word r_info;
} Elf32_Rel;

// e_ident
#define ELFMAG "\177ELF"
#define SELFMAG 4
#define EI_CLASS 4
#define ELFCLASS32 1
#define EI_DATA 5
#define ELFDATA2LSB 1

// e_machine
#define EM_ARM 40

// Segment types (p_type)
#define PT_LOAD 1
#define PT_DYNAMIC 2
#define PT_ARM_EXIDX 0x70000001
#define SHT_ARM_EXIDX 0x70000001
#define SHT_ARM_ATTRIBUTES 0x70000003

// Dynamic tags (d_tag)
#define DT_NULL 0
#define DT_PLTRELSZ 2
#define DT_HASH 4
#define DT_STRTAB 5
#define DT_SYMTAB 6
#define DT_SYMENT 11
#define DT_STRSZ 10
#define DT_REL 17
#define DT_RELSZ 18
#define DT_JMPREL 23
#define DT_INIT_ARRAY 25
#define DT_INIT_ARRAYSZ 27

// Special section index (st_shndx)
#define SHN_UNDEF 0

// Symbol types (st_info)
#define STT_OBJECT 1
#define STT_FUNC 2

#define ELF32_ST_TYPE(info) ((info) & 0xf)
#define ELF32_R_SYM(info) ((info) >> 8)
#define ELF32_R_TYPE(info) ((info) & 0xff)
