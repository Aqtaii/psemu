#pragma once
#include <cstdint>

// ELF Tanımlamaları ve Yapıları
// Standart ELF64 spesifikasyonlarına göre tanımlanmıştır.

// ---------------------------------------------------------
// SELF Konteyner Yapilari (Sony Signed ELF)
// ---------------------------------------------------------
#define SELFMAG 0x1D3D154F // 4F 15 3D 1D (Little-Endian)

struct SelfHeader {
    uint32_t magic;         // 0x1D3D154F
    uint32_t version;
    uint8_t  mode;
    uint8_t  endian;
    uint8_t  attr;
    uint8_t  key_type;
    uint16_t header_size;   // SELF basliginin toplam boyutu
    uint16_t meta_size;
    uint64_t file_size;     // Dosyanin toplam boyutu
    uint16_t num_entries;   // Segment sayisi
    uint16_t flags;
    uint32_t pad;
};

// ---------------------------------------------------------
// Standart ELF64 Tanimlamalari ve Yapilari
// ---------------------------------------------------------

#define EI_NIDENT 16

// 64-bit ELF Başlığı (Header)
struct Elf64_Ehdr {
    uint8_t   e_ident[EI_NIDENT]; // Magic bytes ve yapısal belirteçler
    uint16_t  e_type;             // Obje dosya tipi (Executable, Shared vs.)
    uint16_t  e_machine;          // Hedef Mimari (x86_64, ARM vs.)
    uint32_t  e_version;          // Obje dosya versiyonu
    uint64_t  e_entry;            // Giriş noktası sanal adresi (Entry Point)
    uint64_t  e_phoff;            // Program Header table dosya ofseti (byte cinsinden)
    uint64_t  e_shoff;            // Section Header table dosya ofseti
    uint32_t  e_flags;            // İşlemciye özel bayraklar
    uint16_t  e_ehsize;           // Bu başlığın boyutu
    uint16_t  e_phentsize;        // Program header tablosundaki her bir kaydın boyutu
    uint16_t  e_phnum;            // Program header tablosundaki kayıt sayısı
    uint16_t  e_shentsize;        // Section header tablosundaki her bir kaydın boyutu
    uint16_t  e_shnum;            // Section header tablosundaki kayıt sayısı
    uint16_t  e_shstrndx;         // Section string table indeksi
};

// 64-bit Program Başlığı (Segment)
struct Elf64_Phdr {
    uint32_t  p_type;             // Segment tipi (Load, Dynamic vs.)
    uint32_t  p_flags;            // Segment izinleri (Read, Write, Execute)
    uint64_t  p_offset;           // Segmentin dosyadan başladığı ofset
    uint64_t  p_vaddr;            // Belleğe yerleştirileceği Sanal Adres (Virtual Address)
    uint64_t  p_paddr;            // Fiziksel adres (Genellikle önemsizdir)
    uint64_t  p_filesz;           // Segmentin dosyadaki boyutu
    uint64_t  p_memsz;            // Segmentin bellekteki boyutu (BSS içeriyorsa filesz'den büyük olabilir)
    uint64_t  p_align;            // Bellek hizalaması
};

// e_ident indeksleri
#define EI_MAG0     0
#define EI_MAG1     1
#define EI_MAG2     2
#define EI_MAG3     3
#define EI_CLASS    4
#define EI_DATA     5
#define EI_VERSION  6
#define EI_OSABI    7
#define EI_ABIVERSION 8

// Beklenen Magic Bytes (\x7fELF)
#define ELFMAG0     0x7f
#define ELFMAG1     'E'
#define ELFMAG2     'L'
#define ELFMAG3     'F'

#define ELFCLASS64  2 // 64-bit mimari
#define ELFDATA2LSB 1 // Little-Endian

#define EM_X86_64   62 // AMD x86-64 Mimarisi

// ---------------------------------------------------------
// Dinamik Segment ve Relocation (Yer Degistirme) Yapilari
// ---------------------------------------------------------

struct Elf64_Dyn {
    int64_t d_tag;
    union {
        uint64_t d_val;
        uint64_t d_ptr;
    } d_un;
};

struct Elf64_Rela {
    uint64_t r_offset;
    uint64_t r_info;
    int64_t  r_addend;
};

struct Elf64_Sym {
    uint32_t st_name;       // String table offset
    uint8_t  st_info;       // Type and Binding attributes
    uint8_t  st_other;      // Reserved
    uint16_t st_shndx;      // Section table index
    uint64_t st_value;      // Symbol value
    uint64_t st_size;       // Size of object
};

// PT_DYNAMIC tag'leri
#define DT_NULL     0
#define DT_PLTRELSZ 2
#define DT_STRTAB   5
#define DT_SYMTAB   6
#define DT_RELA     7
#define DT_RELASZ   8
#define DT_RELAENT  9
#define DT_STRSZ    10
#define DT_SYMENT   11
#define DT_PLTREL   20
#define DT_JMPREL   23

// Relocation tipleri ve sembol cıkarımı
#define ELF64_R_SYM(i)  ((i) >> 32)
#define ELF64_R_TYPE(i) ((i) & 0xffffffff)
#define R_X86_64_GLOB_DAT  6
#define R_X86_64_JUMP_SLOT 7
#define R_X86_64_RELATIVE  8

// Program Header Segment Tipleri
#define PT_LOAD     1  // Belleğe yüklenmesi gereken segment
#define PT_DYNAMIC  2  // Dinamik linkleme ve Relocation tabloları
#define PT_TLS      7  // Thread-Local Storage sablonu (.tdata/.tbss)

// Program Header İzin Bayrakları
#define PF_X        1  // Execute (Çalıştırılabilir)
#define PF_W        2  // Write (Yazılabilir)
#define PF_R        4  // Read (Okunabilir)
