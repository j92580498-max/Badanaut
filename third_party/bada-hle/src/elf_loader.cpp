#include "elf_loader.h"
#include <cstdio>
#include <cstring>
#include "compat/elf.h"
#include <fstream>

#ifndef R_ARM_ABS32
#define R_ARM_ABS32 2
#endif
#ifndef R_ARM_GLOB_DAT
#define R_ARM_GLOB_DAT 21
#endif
#ifndef R_ARM_JUMP_SLOT
#define R_ARM_JUMP_SLOT 22
#endif
#ifndef R_ARM_RELATIVE
#define R_ARM_RELATIVE 23
#endif

static const std::string kEmpty;

// Some builds (PvZ) emit an import whose Itanium mangling lost its leading
// underscore -- "ZN3Osp3App11Application7ExecuteE..." instead of "_ZN...".
// Put the underscore back so the name matches the HLE handler table.
static std::string canonical_import_name(const std::string& name) {
    if (name.size() > 2 && name[0] == 'Z' &&
        (name[1] == 'N' || name[1] == 'S' || name[1] == 'T' || name[1] == 'Z')) {
        return "_" + name;
    }
    return name;
}

const std::string& ElfImage::sym_name(uint32_t idx) const {
    if (idx < dynsyms.size()) {
        return dynsyms[idx].name;
    }
    return kEmpty;
}

uint32_t ElfImage::resolve(const std::string& name) const {
    for (auto& s : dynsyms) {
        if (s.shndx != SHN_UNDEF && s.name == name) {
            return s.value;
        }
    }
    return 0;
}

std::string ElfImage::nearest_symbol(uint32_t addr, uint32_t* off) const {
    const Sym* best = nullptr;
    uint32_t best_v = 0;
    for (auto& s : dynsyms) {
        if (s.shndx == SHN_UNDEF || s.type != STT_FUNC) {
            continue;
        }
        uint32_t v = s.value & ~1u; // drop the Thumb bit
        if (v && v <= addr && (!best || v > best_v)) {
            best = &s;
            best_v = v;
        }
    }
    if (off) {
        *off = best ? addr - best_v : 0;
    }
    return best ? best->name : std::string();
}

uint32_t ElfImage::resolve_contains(const std::string& substr) const {
    // Skip non-virtual thunks (_ZThn..., _ZTv...): those adjust `this` for a
    // secondary base. We want the real method and pass the most-derived pointer.
    for (auto& s : dynsyms) {
        if (s.shndx != SHN_UNDEF && s.type == STT_FUNC &&
            s.name.compare(0, 4, "_ZTh") != 0 && s.name.compare(0, 4, "_ZTv") != 0 &&
            s.name.find(substr) != std::string::npos) {
            return s.value;
        }
    }
    return 0;
}

bool ElfImage::load(const std::string& path, std::string& err, uint32_t load_bias) {
    bias = load_bias;
    size_t slash = path.find_last_of("/\\");
    name = slash == std::string::npos ? path : path.substr(slash + 1);
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) {
        err = "cannot open " + path;
        return false;
    }
    size_t sz = f.tellg();
    f.seekg(0);
    file.resize(sz);
    f.read(reinterpret_cast<char*>(file.data()), sz);

    const uint8_t* base = file.data();
    auto* eh = reinterpret_cast<const Elf32_Ehdr*>(base);
    if (memcmp(eh->e_ident, ELFMAG, SELFMAG) != 0 ||
        eh->e_ident[EI_CLASS] != ELFCLASS32 || eh->e_machine != EM_ARM) {
        err = "not an ARM32 ELF";
        return false;
    }
    entry = eh->e_entry;

    // --- Program headers: collect PT_LOAD and locate PT_DYNAMIC ---
    auto* ph = reinterpret_cast<const Elf32_Phdr*>(base + eh->e_phoff);
    const Elf32_Phdr* dyn_ph = nullptr;
    for (int i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type == PT_LOAD) {
            segments.push_back({ph[i].p_vaddr, ph[i].p_filesz, ph[i].p_memsz,
                                ph[i].p_flags, base + ph[i].p_offset});
        } else if (ph[i].p_type == PT_DYNAMIC) {
            dyn_ph = &ph[i];
        }
    }
    if (!dyn_ph) {
        err = "no PT_DYNAMIC";
        return false;
    }

    // Helper: translate a guest vaddr to a file pointer via the segments.
    auto vaddr_ptr = [&](uint32_t va) -> const uint8_t* {
        for (auto& s : segments) {
            if (va >= s.vaddr && va < s.vaddr + s.filesz) {
                return s.data + (va - s.vaddr);
            }
        }
        return nullptr;
    };

    // --- Walk the dynamic section ---
    auto* dyn = reinterpret_cast<const Elf32_Dyn*>(base + dyn_ph->p_offset);
    uint32_t strtab_va = 0, symtab_va = 0;
    uint32_t jmprel_va = 0, jmprel_sz = 0;
    uint32_t rel_va = 0, rel_sz = 0;
    uint32_t init_arr_va = 0, init_arr_sz = 0;
    uint32_t syment = 16;
    for (int i = 0; dyn[i].d_tag != DT_NULL; i++) {
        switch (dyn[i].d_tag) {
        case DT_STRTAB: strtab_va = dyn[i].d_un.d_ptr; break;
        case DT_SYMTAB: symtab_va = dyn[i].d_un.d_ptr; break;
        case DT_SYMENT: syment = dyn[i].d_un.d_val; break;
        case DT_JMPREL: jmprel_va = dyn[i].d_un.d_ptr; break;
        case DT_PLTRELSZ: jmprel_sz = dyn[i].d_un.d_val; break;
        case DT_REL: rel_va = dyn[i].d_un.d_ptr; break;
        case DT_RELSZ: rel_sz = dyn[i].d_un.d_val; break;
        case DT_INIT_ARRAY: init_arr_va = dyn[i].d_un.d_ptr; break;
        case DT_INIT_ARRAYSZ: init_arr_sz = dyn[i].d_un.d_val; break;
        }
    }
    const char* strtab = reinterpret_cast<const char*>(vaddr_ptr(strtab_va));
    const uint8_t* symtab = vaddr_ptr(symtab_va);
    if (!strtab || !symtab) {
        err = "bad STRTAB/SYMTAB";
        return false;
    }

    // --- Dynamic symbol table ---
    // Number of symbols isn't given directly; derive from STRTAB following SYMTAB
    // in the file is unreliable, so size by the highest symbol referenced by
    // relocations plus a generous scan until the symtab region ends at strtab.
    size_t nsyms = (strtab_va > symtab_va) ? (strtab_va - symtab_va) / syment : 0;
    dynsyms.resize(nsyms);
    for (size_t i = 0; i < nsyms; i++) {
        auto* s = reinterpret_cast<const Elf32_Sym*>(symtab + i * syment);
        ElfImage::Sym out;
        out.name = strtab + s->st_name;
        out.value = s->st_value;
        out.shndx = s->st_shndx;
        out.type = ELF32_ST_TYPE(s->st_info);
        dynsyms[i] = std::move(out);
    }

    // --- Assign an import slot to every UND symbol that has a name ---
    auto note_import = [&](uint32_t symidx) {
        if (symidx == 0 || symidx >= dynsyms.size()) {
            return;
        }
        if (sym_to_import.count(symidx)) {
            return;
        }
        const Sym& s = dynsyms[symidx];
        if (s.shndx != SHN_UNDEF || s.name.empty()) {
            return;
        }
        uint32_t idx = imports.size();
        imports.push_back(canonical_import_name(s.name));
        import_is_data.push_back(s.type == STT_OBJECT);
        sym_to_import[symidx] = idx;
    };

    // --- Relocations (REL format, 8 bytes each) ---
    auto read_rel = [&](uint32_t va, uint32_t sz) {
        auto* r = reinterpret_cast<const Elf32_Rel*>(vaddr_ptr(va));
        if (!r) {
            return;
        }
        for (uint32_t i = 0; i < sz / sizeof(Elf32_Rel); i++) {
            uint32_t type = ELF32_R_TYPE(r[i].r_info);
            uint32_t sym = ELF32_R_SYM(r[i].r_info);
            note_import(sym);
            relocs.push_back({r[i].r_offset, type, sym});
        }
    };
    read_rel(rel_va, rel_sz);
    read_rel(jmprel_va, jmprel_sz);

    // --- Init array ---
    if (init_arr_va) {
        auto* ia = reinterpret_cast<const uint32_t*>(vaddr_ptr(init_arr_va));
        for (uint32_t i = 0; i < init_arr_sz / 4; i++) {
            if (ia[i] && ia[i] != 0xffffffff) {
                init_array.push_back(ia[i]);
            }
        }
    }

    // --- Relocate the parsed addresses to the load bias ---
    // Parsing works on the file's own vaddrs (vaddr_ptr depends on that), so the
    // bias goes on at the end, in one place.
    if (bias) {
        entry += bias;
        for (auto& s : segments) {
            s.vaddr += bias;
        }
        for (auto& r : relocs) {
            r.offset += bias;
        }
        for (auto& a : init_array) {
            a += bias;
        }
        for (auto& s : dynsyms) {
            if (s.shndx != SHN_UNDEF) {
                s.value += bias;
            }
        }
    }

    printf("[elf] %s: entry=0x%x bias=0x%x segments=%zu dynsyms=%zu imports=%zu relocs=%zu init=%zu\n",
           name.c_str(), entry, bias, segments.size(), dynsyms.size(), imports.size(),
           relocs.size(), init_array.size());
    return true;
}
