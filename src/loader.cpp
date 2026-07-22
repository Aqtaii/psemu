#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <map>
#include <windows.h>
#include "elf64.h"
#include "logger.h"
#include "syscalls.h"
#include "core.h"

// utf16 detour tanisi (core.cpp'de tanimli); trampoline'den cagirilir.
extern "C" void Utf16DiagValue(void*);
#include "scanner.h"
#include "linker.h"

// ELF bellek izinlerini Windows API bellek koruma bayraklarına dönüştüren yardımcı fonksiyon
DWORD GetWindowsProtection(uint32_t p_flags) {
    bool r = (p_flags & PF_R) != 0;
    bool w = (p_flags & PF_W) != 0;
    bool x = (p_flags & PF_X) != 0;

    if (r && w && x) return PAGE_EXECUTE_READWRITE;
    if (r && x) return PAGE_EXECUTE_READ;
    if (r && w) return PAGE_READWRITE;
    if (r) return PAGE_READONLY;
    if (x) return PAGE_EXECUTE;
    return PAGE_NOACCESS;
}

bool LoadEboot(const std::string& filePath) {
    // ==========================================
    // 0. VFS kokunu belirle (eboot.bin'in bulundugu klasor)
    // ==========================================
    // Guest'teki "/app0/xxx" yollari bu dizine eslenecek.
    {
        size_t slash = filePath.find_last_of("/\\");
        g_game_root = (slash == std::string::npos) ? "." : filePath.substr(0, slash);
        std::cout << "[VFS] Oyun kok dizini (/app0 -> ): " << g_game_root << std::endl;
    }

    // ==========================================
    // 1. Dosya Okuma (Binary Mode)
    // ==========================================
    // Dosyayı sonuna kadar açıp boyutunu öğreniyoruz (ate = at end). Binary flag ile işliyoruz.
    std::ifstream file(filePath, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        std::cerr << "Dosya acilamadi: " << filePath << std::endl;
        return false;
    }

    std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);

    // Tüm dosyayı RAM'e alıyoruz. High-performance loader'lar dosya I/O işlemlerini
    // azaltmak için tüm buffer'ı bir kerede çekmelidir veya memory-mapped file kullanmalıdır.
    std::vector<uint8_t> buffer(size);
    if (!file.read(reinterpret_cast<char*>(buffer.data()), size)) {
        std::cerr << "Dosya okunamadi!" << std::endl;
        return false;
    }

    // ==========================================
    // 2. Başlık Doğrulama (SELF / ELF Parsing)
    // ==========================================
    if (size < sizeof(SelfHeader)) {
        std::cerr << "Dosya cok kucuk, gecerli bir SELF/ELF olamaz." << std::endl;
        return false;
    }

    uint32_t* file_magic = reinterpret_cast<uint32_t*>(buffer.data());
    size_t elf_offset = 0;

    // Eger dosya SELF (Signed ELF) ise
    if (*file_magic == SELFMAG) {
        std::cout << "[+] SELF (Signed ELF) konteyneri saptandi. Kutudan cikartiliyor..." << std::endl;
        
        // Gercek ELF dosyasinin baslangicini bulmak icin ilk 64KB icinde \x7fELF arayalim
        bool elf_found = false;
        size_t limit = (size > 65536) ? 65536 : (size - 4);
        for (size_t i = 0; i < limit; i += 4) {
            if (buffer[i] == ELFMAG0 && buffer[i+1] == ELFMAG1 && buffer[i+2] == ELFMAG2 && buffer[i+3] == ELFMAG3) {
                elf_offset = i;
                elf_found = true;
                break;
            }
        }

        if (!elf_found) {
            std::cerr << "[-] HATA: SELF icerisinde gecerli bir ELF bulunamadi (Dosya tamamen sifreli olabilir)." << std::endl;
            return false;
        }
        std::cout << "[+] Gomulu ELF " << std::hex << "0x" << elf_offset << std::dec << " ofsetinde bulundu!" << std::endl;
    } else {
        // Dosya zaten saf ELF ise ofset dogrudan 0'dir
        elf_offset = 0;
    }

    // Pointer Aritmetiği: Bulunan ofset kadar ileri giderek gercek ELF Header'a ulasalim
    Elf64_Ehdr* header = reinterpret_cast<Elf64_Ehdr*>(buffer.data() + elf_offset);

    // Magic Bytes Kontrolü (\x7fELF)
    if (header->e_ident[EI_MAG0] != ELFMAG0 ||
        header->e_ident[EI_MAG1] != ELFMAG1 ||
        header->e_ident[EI_MAG2] != ELFMAG2 ||
        header->e_ident[EI_MAG3] != ELFMAG3) {
        std::cerr << "HATA: Gecersiz ELF magic bytes." << std::endl;
        return false;
    }

    // 64-bit Kontrolü
    if (header->e_ident[EI_CLASS] != ELFCLASS64) {
        std::cerr << "HATA: Dosya 64-bit ELF degil." << std::endl;
        return false;
    }

    // Little-Endian Kontrolü
    if (header->e_ident[EI_DATA] != ELFDATA2LSB) {
        std::cerr << "HATA: Dosya Little-Endian degil." << std::endl;
        return false;
    }

    // Mimari Kontrolü (PS5/Orbis x86_64 mimarisidir)
    if (header->e_machine != EM_X86_64) {
        std::cerr << "HATA: Desteklenmeyen mimari. x86_64 bekleniyor." << std::endl;
        return false;
    }

    std::cout << "[+] Gecerli bir 64-bit x86_64 ELF dosyasi saptandi." << std::endl;

    // ==========================================
    // 3. Bellek Tahsisi (Unified Memory Mapping)
    // ==========================================
    // Pointer Aritmetiği: elf_offset (gercek ELF'in baslangici) uzerine e_phoff eklenir
    Elf64_Phdr* phdrs = reinterpret_cast<Elf64_Phdr*>(buffer.data() + elf_offset + header->e_phoff);

    // Adim 3.1: Toplam Sanal Bellek Ihtiyacini Hesapla ve Ozel Segmentleri Bul
    uint64_t max_vaddr_end = 0;
    uint64_t procparam_vaddr = 0;
    uint64_t tls_vaddr = 0, tls_filesz = 0, tls_memsz = 0, tls_align = 0;
    uint64_t init_vaddr = 0; // DT_INIT: module_start'tan ONCE cagirilmasi gereken CRT baslatici (.init_array yurutucusu)
    for (int i = 0; i < header->e_phnum; ++i) {
        if (phdrs[i].p_type == PT_LOAD) {
            uint64_t segment_end = phdrs[i].p_vaddr + phdrs[i].p_memsz;
            if (segment_end > max_vaddr_end) {
                max_vaddr_end = segment_end;
            }
        }
        else if (phdrs[i].p_type == 0x61000001) { // PT_SCE_PROCPARAM
            procparam_vaddr = phdrs[i].p_vaddr;
            std::cout << "[INFO] PT_SCE_PROCPARAM (ProcessParam) Segmenti Bulundu! vaddr=0x"
                      << std::hex << phdrs[i].p_vaddr << ", filesz=0x" << phdrs[i].p_filesz << std::dec << std::endl;
        }
        else if (phdrs[i].p_type == PT_TLS) {
            tls_vaddr = phdrs[i].p_vaddr;
            tls_filesz = phdrs[i].p_filesz;
            tls_memsz = phdrs[i].p_memsz;
            tls_align = phdrs[i].p_align;
            std::cout << "[INFO] PT_TLS Segmenti bulundu! vaddr=0x" << std::hex << tls_vaddr
                      << ", filesz=0x" << tls_filesz << ", memsz=0x" << tls_memsz
                      << ", align=0x" << tls_align << std::dec << std::endl;
        }
    }

    if (max_vaddr_end == 0) {
        std::cerr << "[-] HATA: PT_LOAD segmenti bulunamadi." << std::endl;
        return false;
    }

    // Toplam bellek icin tek bir devasa (Unified) tahsisat yapiyoruz.
    // vaddr = 0x0 oldugu icin Windows rastgele bir guvenli Base Address verecek.
    void* base_address = VirtualAlloc(nullptr, max_vaddr_end, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!base_address) {
        std::cerr << "[-] HATA: Bellek ayirma basarisiz. Istenen boyut: 0x" << std::hex << max_vaddr_end << std::endl;
        return false;
    }

    uint8_t* base_ptr = reinterpret_cast<uint8_t*>(base_address);
    std::cout << "[+] Bellek blogu basariyla tahsis edildi. Base Address: 0x" << std::hex << reinterpret_cast<uint64_t>(base_ptr) << std::dec << std::endl;

    // Adim 3.2: Segmentleri Bellege Kopyala
    for (int i = 0; i < header->e_phnum; ++i) {
        Elf64_Phdr* phdr = &phdrs[i];

        if (phdr->p_type == PT_LOAD) {
            std::cout << "[Segment] PT_LOAD kopyalaniyor: vaddr=0x" << std::hex << phdr->p_vaddr 
                      << ", memsz=0x" << phdr->p_memsz << std::dec << std::endl;

            // p_offset ham ELF'in baslangicina gore hesaplanmistir. SELF dosyalarinda 
            // gercek veriye ulasmak icin daima elf_offset eklenmelidir!
            size_t absolute_file_offset = phdr->p_offset + elf_offset;

            if (absolute_file_offset + phdr->p_filesz > (size_t)size) {
                std::cerr << "[-] UYARI: Segment dosya boyutunu asiyor! ofset: " << absolute_file_offset << " filesz: " << phdr->p_filesz << std::endl;
                size_t copy_size = 0;
                if (absolute_file_offset < (size_t)size) {
                    copy_size = (size_t)size - absolute_file_offset;
                    std::cerr << "    Dosyanin sonuna ulasildi. Sadece " << copy_size << " byte kopyalanacak." << std::endl;
                }
                
                if (copy_size > 0) {
                    memcpy(base_ptr + phdr->p_vaddr, buffer.data() + absolute_file_offset, copy_size);
                }
            } else {
                memcpy(base_ptr + phdr->p_vaddr, buffer.data() + absolute_file_offset, phdr->p_filesz);
            }
            
            // BSS sifirlama
            if (phdr->p_memsz > phdr->p_filesz) {
                memset(base_ptr + phdr->p_vaddr + phdr->p_filesz, 0, phdr->p_memsz - phdr->p_filesz);
            }
        }
    }

    // ==========================================
    // 4. Base Relocation (Adres Yamalama)
    // ==========================================
    for (int i = 0; i < header->e_phnum; ++i) {
        if (phdrs[i].p_type == PT_DYNAMIC) {
            std::cout << "[+] PT_DYNAMIC bulundu! Relocation (Adres Duzeltme) basliyor..." << std::endl;
            
            Elf64_Dyn* dyn_table = reinterpret_cast<Elf64_Dyn*>(base_ptr + phdrs[i].p_vaddr);
            
            Elf64_Rela* rela_table = nullptr;
            size_t rela_size = 0;
            size_t rela_ent = 0;
            // DYNAMIC tablosunu tara
            std::cout << "[DEBUG] DYNAMIC tablosu TAM d_tag listesi:" << std::endl;
            std::cout << "[DEBUG] PT_DYNAMIC p_vaddr=0x" << std::hex << phdrs[i].p_vaddr
                      << " p_offset=0x" << phdrs[i].p_offset
                      << " p_filesz=0x" << phdrs[i].p_filesz
                      << " dyn_table_addr=0x" << reinterpret_cast<uint64_t>(dyn_table)
                      << std::dec << std::endl;

            // Ilk PF_X (executable) segmentin boyutunu bul
            uint64_t exec_seg_size = 0;
            for (int es = 0; es < header->e_phnum; ++es) {
                if (phdrs[es].p_type == PT_LOAD && (phdrs[es].p_flags & 0x1)) {
                    exec_seg_size = phdrs[es].p_memsz;
                    break;
                }
            }
            if (exec_seg_size == 0) exec_seg_size = max_vaddr_end;
            uint64_t base_as_u64 = reinterpret_cast<uint64_t>(base_ptr);

            // ============================================================
            // KOSULSUZ HAM DUMP: Ilk 20 adet Elf64_Dyn yapisini yazdir
            // ============================================================
            std::cout << "[DEBUG] HAM DYNAMIC DUMP (ilk 20 entry, 320 byte):" << std::endl;
            for (int j = 0; j < 20; ++j) {
                uint64_t tag = dyn_table[j].d_tag;
                uint64_t val = dyn_table[j].d_un.d_val;
                
                const char* tag_name = "";
                if (tag == 0x00) tag_name = " (DT_NULL)";
                else if (tag == 0x01) tag_name = " (DT_NEEDED)";
                else if (tag == 0x04) tag_name = " (DT_HASH)";
                else if (tag == 0x05) tag_name = " (DT_STRTAB)";
                else if (tag == 0x06) tag_name = " (DT_SYMTAB)";
                else if (tag == 0x07) tag_name = " (DT_RELA)";
                else if (tag == 0x08) tag_name = " (DT_RELASZ)";
                else if (tag == 0x09) tag_name = " (DT_RELAENT)";
                else if (tag == 0x0C) tag_name = " (DT_INIT)";
                else if (tag == 0x0D) tag_name = " (DT_FINI)";
                else if (tag == 0x17) tag_name = " (DT_JMPREL)";
                else if (tag == 0x19) tag_name = " (DT_INIT_ARRAY)";
                else if (tag == 0x1A) tag_name = " (DT_FINI_ARRAY)";
                else if (tag == 0x1B) tag_name = " (DT_INIT_ARRAYSZ)";
                else if (tag == 0x1C) tag_name = " (DT_FINI_ARRAYSZ)";
                else if (tag >= 0x60000000 && tag <= 0x6FFFFFFF) tag_name = " [SONY]";
                
                std::cout << "  [" << j << "] d_tag=0x" << std::hex << tag << tag_name
                          << " d_val=0x" << val << std::dec << std::endl;
            }
            std::cout << "[DEBUG] HAM DUMP BITTI." << std::endl;
            
            // Ayri dongu: RELA ve Symbol tablolarini cikart (DT_NULL'a kadar)
            Elf64_Sym* sym_table = nullptr;
            char* str_table = nullptr;
            uint64_t sym_ent = 0;
            
            Elf64_Rela* jmprel_table = nullptr;
            uint64_t jmprel_size = 0;
            
            for (int j = 0; dyn_table[j].d_tag != DT_NULL; ++j) {
                uint64_t tag = dyn_table[j].d_tag;
                uint64_t val = dyn_table[j].d_un.d_val;
                
                if (tag == 0x0C) { // DT_INIT
                    init_vaddr = val;
                } else if (tag == DT_RELA || tag == 0x6100002F) {
                    rela_table = reinterpret_cast<Elf64_Rela*>(base_ptr + val);
                } else if (tag == DT_RELASZ || tag == 0x61000031) {
                    rela_size = val;
                } else if (tag == DT_RELAENT || tag == 0x61000033) {
                    rela_ent = val;
                } else if (tag == DT_SYMTAB || tag == 0x61000039) {
                    sym_table = reinterpret_cast<Elf64_Sym*>(base_ptr + val);
                } else if (tag == DT_SYMENT || tag == 0x6100003B) {
                    sym_ent = val;
                } else if (tag == DT_STRTAB || tag == 0x61000035) {
                    str_table = reinterpret_cast<char*>(base_ptr + val);
                } else if (tag == DT_JMPREL) {
                    jmprel_table = reinterpret_cast<Elf64_Rela*>(base_ptr + val);
                } else if (tag == DT_PLTRELSZ) {
                    jmprel_size = val;
                }
                
                // Sonsuz donguyu onle (max 500 entry)
                if (j > 500) break;
            }

            if (rela_table && rela_ent > 0) {
                int patch_count = 0;
                size_t num_relocs = rela_size / rela_ent;
                
                // TESHIS: Her relocation tipinden kac adet geldigini say
                std::map<uint32_t, int> rela_type_counts;
                for (size_t k = 0; k < num_relocs; ++k) {
                    uint32_t rtype = ELF64_R_TYPE(rela_table[k].r_info);
                    rela_type_counts[rtype]++;
                }
                std::cout << "[DEBUG] RELA Tip Dagilimi (toplam " << num_relocs << " entry):" << std::endl;
                for (auto& pair : rela_type_counts) {
                    std::string tname;
                    switch (pair.first) {
                        case 6:  tname = "R_X86_64_GLOB_DAT"; break;
                        case 7:  tname = "R_X86_64_JUMP_SLOT"; break;
                        case 8:  tname = "R_X86_64_RELATIVE"; break;
                        case 1:  tname = "R_X86_64_64"; break;
                        case 5:  tname = "R_X86_64_COPY"; break;
                        case 10: tname = "R_X86_64_32"; break;
                        case 11: tname = "R_X86_64_32S"; break;
                        default: tname = "UNKNOWN"; break;
                    }
                    std::cout << "  Tip " << pair.first << " (" << tname << "): " << pair.second << " adet" << std::endl;
                }
                
                for (size_t k = 0; k < num_relocs; ++k) {
                    Elf64_Rela* rela = &rela_table[k];
                    uint32_t r_type = ELF64_R_TYPE(rela->r_info);
                    uint32_t r_sym  = ELF64_R_SYM(rela->r_info);
                    
                    if (r_type == R_X86_64_RELATIVE) {
                        uint64_t* patch_target = reinterpret_cast<uint64_t*>(base_ptr + rela->r_offset);
                        *patch_target = reinterpret_cast<uint64_t>(base_ptr) + rela->r_addend;
                        patch_count++;
                    } else if (r_type == R_X86_64_GLOB_DAT) {
                        // GLOB_DAT: bu, harici bir kutuphaneden (libkernel/libc)
                        // import edilen bir VERI sembolune (fonksiyon degil!)
                        // isaret eder. Kod bu adresi bir GOT slotu gibi
                        // kullanir: once [hedef]'i okuyup GERCEK degiskenin
                        // adresini alir, sonra ONU dereference eder. Bu
                        // dallari daha once HIC yamalanmiyordu; hedef 0
                        // kaliyordu ve ilk dereference NULL-pointer cokmesine
                        // yol aciyordu (ornegin __stack_chk_guard, RVA
                        // 0x2c61b2 cokmesinin gercek kaynagi).
                        std::string sym_name = "<unknown>";
                        if (sym_table && str_table && r_sym > 0) {
                            Elf64_Sym* sym = &sym_table[r_sym];
                            sym_name = &str_table[sym->st_name];
                        }

                        // Sembolun kendisi icin gercek bellekte bir hucre ayir
                        // (bu hucrenin ADRESINI relocation hedefine yaziyoruz).
                        uint64_t* cell = reinterpret_cast<uint64_t*>(
                            VirtualAlloc(nullptr, 0x10000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));

                        if (cell) {
                            if (sym_name.rfind("f7uOxY9mM1U", 0) == 0) {
                                // __stack_chk_guard: stack protector canary'si.
                                // Gercek deger onemsiz, sadece sifir olmayan
                                // ve tutarli olmasi yeterli.
                                *cell = 0x0FEDCBA987654321ULL;
                            } else if (sym_name.rfind("Qoo175Ig+-k", 0) == 0) {
                                // libc'den import edilen bir C++ NESNE pointer'i.
                                // Oyun `obj = *cell; obj->vtable[2]()` yapiyor
                                // (RVA 0x134a80). Gercek nesneyi saglayamadigimiz
                                // icin: bos metotlardan (xor eax,eax; ret) olusan
                                // bir vtable'a sahip, alanlari sifir bir SAHTE
                                // nesne kuruyoruz. count(+0x18)=0 oldugundan
                                // oyun "bos kayit" yolunu izliyor.
                                static void* s_fake_obj = nullptr;
                                if (s_fake_obj == nullptr) {
                                    // ret-stub: 31 C0 C3 = xor eax,eax; ret
                                    uint8_t* stub = reinterpret_cast<uint8_t*>(VirtualAlloc(
                                        nullptr, 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
                                    stub[0] = 0x31; stub[1] = 0xC0; stub[2] = 0xC3;
                                    // vtable: 32 giris, hepsi ret-stub
                                    uint64_t* vtbl = reinterpret_cast<uint64_t*>(VirtualAlloc(
                                        nullptr, 32 * sizeof(uint64_t),
                                        MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
                                    for (int vi = 0; vi < 32; vi++)
                                        vtbl[vi] = reinterpret_cast<uint64_t>(stub);
                                    // nesne: 0x80 byte sifir, [0] = vtable
                                    uint64_t* obj = reinterpret_cast<uint64_t*>(VirtualAlloc(
                                        nullptr, 0x80, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
                                    memset(obj, 0, 0x80);
                                    obj[0] = reinterpret_cast<uint64_t>(vtbl);
                                    s_fake_obj = obj;
                                }
                                *cell = reinterpret_cast<uint64_t>(s_fake_obj);
                                std::cout << "[GLOB_DAT] Qoo175Ig+-k -> SAHTE C++ nesnesi @ 0x"
                                          << std::hex << *cell << std::dec << std::endl;
                            } else {
                                *cell = 0; // guvenli varsayilan
                            }

                            uint64_t* patch_target = reinterpret_cast<uint64_t*>(base_ptr + rela->r_offset);
                            *patch_target = reinterpret_cast<uint64_t>(cell);
                        }

                        std::cout << "[GLOB_DAT] " << sym_name << " -> hucre ayrildi @ 0x"
                                  << std::hex << reinterpret_cast<uint64_t>(cell) << std::dec << std::endl;
                    }
                }
                std::cout << "[+] RELA islemi tamamlandi! Toplam " << patch_count << " adet pointer yamanip guncellendi." << std::endl;
                
                // JMPREL (PLT) Tablosunu işle ve g_plt_names'i doldur
                if (jmprel_table != nullptr && jmprel_size > 0 && rela_ent > 0) {
                    size_t num_plt = jmprel_size / rela_ent;
                    std::cout << "[INFO] DT_JMPREL bulundu, " << num_plt << " adet JUMP_SLOT isleniyor..." << std::endl;
                    
                    for (size_t k = 0; k < num_plt; ++k) {
                        Elf64_Rela* rela = &jmprel_table[k];
                        uint32_t r_type = ELF64_R_TYPE(rela->r_info);
                        uint32_t r_sym  = ELF64_R_SYM(rela->r_info);
                        
                        if (r_type == R_X86_64_JUMP_SLOT && sym_table && str_table && r_sym > 0) {
                            Elf64_Sym* sym = &sym_table[r_sym];
                            std::string sym_name = &str_table[sym->st_name];
                            
                            // core.cpp'nin erisebilmesi icin map'e kaydet
                            g_plt_names[static_cast<int>(k)] = sym_name;
                        }
                    }
                    std::cout << "[+] JMPREL sembol isimleri alindi." << std::endl;
                }
            } else {
                std::cout << "[-] UYARI: DYNAMIC segment var ama icinde RELA tablosu bulunamadi." << std::endl;
                
                // ==========================================
                // 4b. Brute-Force Relocation (Fallback)
                // ==========================================
                // RELA tablosu truncated (kirpilmis) dosyalarda kayiptir.
                // Bu durumda veri segmentlerini tarayip, gecerli sanal adrese
                // benzeyen her 8-byte'lik degere base_address ekliyoruz.
                // Bu, GOT/PLT tablolarindaki pointer'lari duzeltir.
                std::cout << "[+] Brute-force relocation baslatiliyor (RELA eksik, fallback mod)..." << std::endl;
                int bf_patch_count = 0;
                for (int si = 0; si < header->e_phnum; ++si) {
                    Elf64_Phdr* seg = &phdrs[si];
                    
                    if (seg->p_type != PT_LOAD) continue;
                    
                    // Executable (PF_X) segmentleri KESINLIKLE atla - makine kodlarini bozmayalim
                    if (seg->p_flags & PF_X) {
                        std::cout << "[BF-RELOC] SKIP (Executable): vaddr=0x" << std::hex 
                                  << seg->p_vaddr << " memsz=0x" << seg->p_memsz 
                                  << " flags=0x" << seg->p_flags << std::dec << std::endl;
                        continue;
                    }
                    
                    // Sadece yazilabilir (PF_W) veri segmentlerini tara
                    if (!(seg->p_flags & PF_W)) {
                        std::cout << "[BF-RELOC] SKIP (Read-Only): vaddr=0x" << std::hex 
                                  << seg->p_vaddr << " memsz=0x" << seg->p_memsz 
                                  << " flags=0x" << seg->p_flags << std::dec << std::endl;
                        continue;
                    }
                    
                    std::cout << "[BF-RELOC] SCAN: vaddr=0x" << std::hex 
                              << seg->p_vaddr << " memsz=0x" << seg->p_memsz 
                              << " flags=0x" << seg->p_flags << std::dec << std::endl;
                    
                    uint8_t* seg_start = base_ptr + seg->p_vaddr;
                    size_t seg_size = seg->p_memsz;
                    
                    // Bellek korumasini gecici olarak READWRITE yap
                    DWORD oldProt;
                    VirtualProtect(seg_start, seg_size, PAGE_READWRITE, &oldProt);
                    
                    int seg_patch_count = 0;
                    // 8-byte hizali her degeri kontrol et
                    for (size_t off = 0; off + 8 <= seg_size; off += 8) {
                        uint64_t* slot = reinterpret_cast<uint64_t*>(seg_start + off);
                        uint64_t val = *slot;
                        
                        // Alt sinir filtresi: 0x1000'den kucuk degerler pointer degil,
                        // duz veridir (enum, boyut, bayrak vb.) - ATLA
                        if (val < 0x1000 || val >= max_vaddr_end) continue;
                        
                        // Deger gecerli bir PT_LOAD segmentinin RVA araligina dusuyor mu?
                        bool is_valid_rva = false;
                        for (int p_idx = 0; p_idx < header->e_phnum; ++p_idx) {
                            if (phdrs[p_idx].p_type == PT_LOAD) {
                                uint64_t vaddr_start = phdrs[p_idx].p_vaddr;
                                uint64_t vaddr_end = vaddr_start + phdrs[p_idx].p_memsz;
                                if (val >= vaddr_start && val < vaddr_end) {
                                    is_valid_rva = true;
                                    break;
                                }
                            }
                        }
                        
                        if (is_valid_rva) {
                            *slot = val + reinterpret_cast<uint64_t>(base_ptr);
                            bf_patch_count++;
                            seg_patch_count++;
                        }
                    }
                    
                    std::cout << "  -> " << seg_patch_count << " pointer yamalandi." << std::endl;
                    
                    // Bellek korumasini eski haline getir
                    VirtualProtect(seg_start, seg_size, oldProt, &oldProt);
                }
                std::cout << "[+] Brute-force relocation tamamlandi! " << bf_patch_count << " adet pointer yamalandi." << std::endl;
            }
            break; // Sadece ilk PT_DYNAMIC okunur
        }
    }

    if (init_vaddr != 0) {
        std::cout << "[INFO] DT_INIT bulundu! vaddr=0x" << std::hex << init_vaddr << std::dec
                   << " - module_start'tan ONCE cagirilacak (CRT/.init_array yurutucusu)." << std::endl;
    } else {
        std::cout << "[-] UYARI: DT_INIT bulunamadi, CRT baslatici atlanacak." << std::endl;
    }

    // ==========================================
    // 5. Entry Point (Giriş Noktası)
    // ==========================================
    // Oyun artik Base Address ile yamanmistir. Giris noktasi da bu kaymayi alir.
    uint64_t original_entry = reinterpret_cast<uint64_t>(base_ptr) + header->e_entry;
    uint64_t entry_point = original_entry;
    
    // 8. Heuristic Scanner ve Dynamic Linker
    // NOT: phdrs pointer'i satir 121'de elf_offset dahil edilerek dogru hesaplanmistir.
    // Daha once burada elf_offset eklenmeden buffer.data() + header->e_phoff kullaniliyordu
    // ve bu SELF dosyalarinda yanlis adresten okumaya, text_segment_size=0 olmasina neden oluyordu!
    uint64_t text_segment_size = 0; 
    for (int i = 0; i < header->e_phnum; ++i) {
        if (phdrs[i].p_type == PT_LOAD && phdrs[i].p_vaddr == 0x0) {
            text_segment_size = phdrs[i].p_memsz;
            break;
        }
    }
    
    // PLT/GOT tablolari text segmentin sonunda yer alir ama GOT farklı bir segmentte olabilir.
    // Bu yuzden tarayiciya tum yuklenmis bellek alanini (max_vaddr_end) geciriyoruz.
    size_t scan_size = (text_segment_size > 0) ? text_segment_size : max_vaddr_end;
    
    // Scanner: SADECE e_entry gecersizse (0 veya base disinda) fallback olarak kullan
    if (entry_point == reinterpret_cast<uint64_t>(base_address) || entry_point == 0) {
        uint64_t heuristic_entry = Scanner::FindEntryPoint(reinterpret_cast<uint8_t*>(base_address), scan_size);
        if (heuristic_entry != 0) {
            entry_point = heuristic_entry;
            std::cout << "[INFO] e_entry gecersiz, heuristic entry point kullaniliyor: 0x" 
                      << std::hex << entry_point << std::dec << std::endl;
        }
    } else {
        std::cout << "[INFO] ELF e_entry kullaniliyor (heuristic override DEVRE DISI): 0x" 
                  << std::hex << entry_point << std::dec << std::endl;
    }
    
    // Linker ile PRX stub'lari kuruyoruz
    Linker::ResolveImports(reinterpret_cast<uint8_t*>(base_address), scan_size);
    
    std::cout << "\n=============================================" << std::endl;
    std::cout << "[!] Gercek Entry Point (Giris Noktasi): 0x" << std::hex << entry_point << std::dec << std::endl;
    
    // Asil makine kodlarinin (Instruction) bellege dogru oturdugunu kanitlamak icin ilk 16 byte'i dump edelim
    std::cout << "[DEBUG] Entry Point Makine Kodlari (Hex): ";
    uint8_t* ep_ptr = reinterpret_cast<uint8_t*>(entry_point);
    for (int k = 0; k < 16; ++k) {
        printf("%02X ", ep_ptr[k]);
    }
    std::cout << "\n=============================================\n" << std::endl;
    
    std::cout << "Oyun kodlari windows uzerinde dogru yerlerine oturtuldu ve calismaya hazir." << std::endl;

    // ==========================================
    // 6. Syscall Hooking (VEH Patching)
    // ==========================================
    std::cout << "[+] Calistirilabilir (Executable) segmentler taranip Syscall kancalari (INT 3) atiliyor..." << std::endl;
    for (int i = 0; i < header->e_phnum; ++i) {
        Elf64_Phdr* phdr = &phdrs[i];
        if (phdr->p_type == PT_LOAD && (phdr->p_flags & PF_X)) {
            uint8_t* seg_start = base_ptr + phdr->p_vaddr;
            size_t seg_size = phdr->p_filesz;

            // Bellek korumasini gecici olarak READWRITE yap
            DWORD oldProtect;
            VirtualProtect(seg_start, seg_size, PAGE_EXECUTE_READWRITE, &oldProtect);

            int patch_count = 0;
            // 0F 05 (syscall) arayalim
            for (size_t k = 0; k < seg_size - 1; ++k) {
                if (seg_start[k] == 0x0F && seg_start[k+1] == 0x05) {
                    seg_start[k] = 0xCC;   // INT 3
                    seg_start[k+1] = 0x90; // NOP
                    patch_count++;
                }
            }

            // Bellek korumasini eski haline getir
            VirtualProtect(seg_start, seg_size, oldProtect, &oldProtect);
            
            if (patch_count > 0) {
                std::cout << "  -> Segment (vaddr: 0x" << std::hex << phdr->p_vaddr << std::dec 
                          << ") uzerinde " << patch_count << " adet syscall yamalandi." << std::endl;
            }
        }
    }

    // ==========================================
    // 6c. YARIS DUZELTMESI: tip-kayit fonksiyonunu (0x2dfff0) serilestir
    // ==========================================
    // Bu fonksiyon paylasilan bir sayaci (0x4c6be8) kilitsiz "oku;+1;yaz"
    // ile guncelliyor ve tek-thread'li init varsayiyor. Bizim thread
    // zamanlamamiz onu eszamanli calistirinca sayac 4'e tasip (tablo 4
    // slot) NULL fallback uzerinden cokuyordu (RVA 0x2e02f7). Cozum:
    // fonksiyonu spinlock'lu bir trampoline'e sarip iki cagri yerini
    // (RVA 0xe380e, 0xe75be) oraya yonlendiriyoruz. Boylece tum cagrilar
    // serilesip sayac tutarli kaliyor.
    {
        uint8_t* base_ptr2 = reinterpret_cast<uint8_t*>(base_address);
        uint64_t target = reinterpret_cast<uint64_t>(base_ptr2) + 0x2dfff0;
        uint8_t* tr = reinterpret_cast<uint8_t*>(
            VirtualAlloc(nullptr, 0x80, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
        if (tr) {
            const uint32_t LOCK  = 0x40;              // kilit bayti offseti
            const uint32_t CNTR  = 0x48;              // cagri sayaci offseti (4 byte)
            int p = 0;
            tr[p++] = 0x50;                            // push rax
            // .spin (@0x01):
            tr[p++] = 0xB0; tr[p++] = 0x01;            // mov al, 1
            tr[p++] = 0x86; tr[p++] = 0x05;            // xchg [rip+d1], al
            int32_t d1 = (int32_t)LOCK - (p + 4);
            memcpy(tr + p, &d1, 4); p += 4;
            tr[p++] = 0x84; tr[p++] = 0xC0;            // test al, al
            tr[p++] = 0x75; tr[p++] = (uint8_t)(0x01 - (p + 1)); // jne .spin
            tr[p++] = 0x58;                            // pop rax
            // lock inc dword [rip+dc]  (cagri sayacini artir)
            tr[p++] = 0xF0; tr[p++] = 0xFF; tr[p++] = 0x05;
            int32_t dc = (int32_t)CNTR - (p + 4);
            memcpy(tr + p, &dc, 4); p += 4;
            tr[p++] = 0x49; tr[p++] = 0xBB;            // movabs r11, target
            memcpy(tr + p, &target, 8); p += 8;
            tr[p++] = 0x41; tr[p++] = 0xFF; tr[p++] = 0xD3; // call r11
            tr[p++] = 0xC6; tr[p++] = 0x05;            // mov byte [rip+d2], 0
            int32_t d2 = (int32_t)LOCK - (p + 5);
            memcpy(tr + p, &d2, 4); p += 4;
            tr[p++] = 0x00;
            tr[p++] = 0xC3;                            // ret
            tr[LOCK] = 0;                              // kilit
            *reinterpret_cast<uint32_t*>(tr + CNTR) = 0; // sayac
            extern volatile uint32_t* g_reg_call_count_ptr;
            g_reg_call_count_ptr = reinterpret_cast<uint32_t*>(tr + CNTR);

            uint64_t tramp = reinterpret_cast<uint64_t>(tr);
            // Iki cagri yerini (E8 rel32) trampoline'e yonlendir.
            // NOT: bu RVA'lar E8 KOMUTUNUN kendi konumu (donus adresi degil).
            for (uint64_t call_rva : { (uint64_t)0xe3813, (uint64_t)0xe75c3 }) {
                uint8_t* call_site = base_ptr2 + call_rva;
                DWORD oldp;
                if (VirtualProtect(call_site, 5, PAGE_EXECUTE_READWRITE, &oldp)) {
                    // yeni rel32 = hedef - (E8 + 5)
                    int32_t rel = (int32_t)(tramp -
                        (reinterpret_cast<uint64_t>(base_ptr2) + call_rva + 5));
                    call_site[0] = 0xE8;
                    memcpy(call_site + 1, &rel, 4);
                    VirtualProtect(call_site, 5, oldp, &oldp);
                }
            }
            std::cout << "[YARIS-FIX] 0x2dfff0 serilestirildi (trampoline @0x"
                      << std::hex << tramp << std::dec << ")" << std::endl;
        }
    }

    // ==========================================
    // 6d. UTF16 DETOUR: string-format cagri yerini (0x17b818 -> 0x17b120)
    //     bir trampoline'e yonlendir; trampoline once Utf16DiagValue(rsi)'i,
    //     sonra orijinal converter'i cagirir. Thread-guvenli (single-step
    //     yok). Bozuk surrogate iceren gercek degerin tag'ini gormek icin.
    // ==========================================
    {
        uint8_t* base_ptr3 = reinterpret_cast<uint8_t*>(base_address);
        uint64_t conv = reinterpret_cast<uint64_t>(base_ptr3) + 0x17b120;
        uint64_t diag = reinterpret_cast<uint64_t>(&Utf16DiagValue);
        uint8_t* tr = reinterpret_cast<uint8_t*>(
            VirtualAlloc(nullptr, 0x80, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
        if (tr) {
            int p = 0;
            tr[p++] = 0x55;                                  // push rbp
            tr[p++] = 0x48; tr[p++] = 0x89; tr[p++] = 0xE5;  // mov rbp, rsp
            tr[p++] = 0x57;                                  // push rdi
            tr[p++] = 0x56;                                  // push rsi
            tr[p++] = 0x48; tr[p++] = 0x89; tr[p++] = 0xF1;  // mov rcx, rsi
            tr[p++] = 0x48; tr[p++] = 0x83; tr[p++] = 0xEC; tr[p++] = 0x20; // sub rsp,0x20
            tr[p++] = 0x48; tr[p++] = 0xB8;                  // movabs rax, diag
            memcpy(tr + p, &diag, 8); p += 8;
            tr[p++] = 0xFF; tr[p++] = 0xD0;                  // call rax
            tr[p++] = 0x48; tr[p++] = 0x83; tr[p++] = 0xC4; tr[p++] = 0x20; // add rsp,0x20
            tr[p++] = 0x5E;                                  // pop rsi
            tr[p++] = 0x5F;                                  // pop rdi
            tr[p++] = 0x49; tr[p++] = 0xBB;                  // movabs r11, conv
            memcpy(tr + p, &conv, 8); p += 8;
            tr[p++] = 0x41; tr[p++] = 0xFF; tr[p++] = 0xD3;  // call r11
            tr[p++] = 0x5D;                                  // pop rbp
            tr[p++] = 0xC3;                                  // ret

            uint64_t tramp = reinterpret_cast<uint64_t>(tr);
            uint8_t* cs = base_ptr3 + 0x17b818;              // E8 call 0x17b120
            DWORD oldp;
            if (VirtualProtect(cs, 5, PAGE_EXECUTE_READWRITE, &oldp)) {
                int32_t rel = (int32_t)(tramp -
                    (reinterpret_cast<uint64_t>(base_ptr3) + 0x17b818 + 5));
                cs[0] = 0xE8;
                memcpy(cs + 1, &rel, 4);
                VirtualProtect(cs, 5, oldp, &oldp);
            }
            std::cout << "[UTF16-DETOUR] converter cagrisi yonlendirildi (tramp @0x"
                      << std::hex << tramp << std::dec << ")" << std::endl;
        }
    }

    // ==========================================
    // 6e. UTF16 NON-FATAL: invalid_utf16 throw dallarini NOP'la
    // ==========================================
    // Katic utf16->utf8 serilestirici (0x216050) eslesmemis surrogate'te
    // invalid_utf16 firlatip oyunu olduruyor (catch yok). Item aciklamasi
    // "i7||..." glyph kodlamasi bunu tetikliyor. Throw'a giden iki kosullu
    // dali (0x216462 je, 0x2164cc jge) NOP'layarak throw'u engelliyoruz:
    // fonksiyon normal yolundan (islem/ret) devam eder, cikti eksik olsa da
    // oyun cokmeden ILERLER. (Ayni mantik yaris-skip gibi: throw'a girmeden
    // engelle.) Gerekirse geri alinabilir.
    {
        uint8_t* bp = reinterpret_cast<uint8_t*>(base_address);
        for (uint64_t rva : { (uint64_t)0x216462, (uint64_t)0x2164cc }) {
            uint8_t* site = bp + rva;
            DWORD oldp;
            if (VirtualProtect(site, 6, PAGE_EXECUTE_READWRITE, &oldp)) {
                // 6-baytlik kosullu jump -> 6 NOP (asla alinmaz = fall-through)
                for (int i = 0; i < 6; i++) site[i] = 0x90;
                VirtualProtect(site, 6, oldp, &oldp);
            }
        }
        std::cout << "[UTF16-NONFATAL] invalid_utf16 throw dallari NOP'landi "
                     "(0x216462, 0x2164cc)" << std::endl;
    }

    // ==========================================
    // 7. Oyunu Calistir!
    // ==========================================
    Core::StartExecution(entry_point, reinterpret_cast<uint64_t>(base_address), scan_size, original_entry, procparam_vaddr,
                          tls_vaddr, tls_filesz, tls_memsz, tls_align, max_vaddr_end, init_vaddr);

    return true;
}

int main(int argc, char** argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    Logger::Init();

    if (argc < 2) {
        std::cout << "Kullanim: " << argv[0] << " <eboot.bin_yolu>" << std::endl;
        std::cout << "Not: Bu iskelet yalnizca decrpyted (sifresi cozulmus) bin dosyalarinda calisir." << std::endl;
        return 1;
    }

    LoadEboot(argv[1]);

    return 0;
}
