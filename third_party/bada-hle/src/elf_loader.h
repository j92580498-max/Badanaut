#pragma once
#include "arm_abi.h"
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

// Minimal ARM32 ELF (DYN/shared-object) parser for the HLE loader.
// Parses PT_LOAD segments, the dynamic symbol/string tables, relocations and
// the init array. The app image loads at bias 0 (its vaddrs are already
// >= 0x100000); a second image (the guest libstdc++.so, whose vaddrs start at
// 0) loads at a bias that keeps it clear of the app.

struct ElfSegment {
    uint32_t vaddr;
    uint32_t filesz;
    uint32_t memsz;
    uint32_t flags;      // PF_R/W/X
    const uint8_t* data; // pointer into the mmapped file
};

struct ElfReloc {
    uint32_t offset; // guest address to patch
    uint32_t type;   // R_ARM_*
    uint32_t symidx; // index into dynsym (0 = none)
};

struct ElfImage {
    std::vector<uint8_t> file; // whole file in memory
    std::vector<ElfSegment> segments;
    std::vector<ElfReloc> relocs;
    std::vector<uint32_t> init_array; // guest addresses of init fns
    uint32_t entry = 0;
    uint32_t bias = 0;      // added to every vaddr this image exposes
    std::string name;       // basename, for logging
    ArmFloatAbi float_abi = ArmFloatAbi::unknown;
    uint32_t exidx = 0, exidx_count = 0; // guest exception index table

    // Dynamic symbols. Imports == STT entries that are UND.
    struct Sym {
        std::string name;
        uint32_t value;
        uint16_t shndx;
        uint8_t type;
    };
    std::vector<Sym> dynsyms;

    // import_index -> symbol name (the order trampoline slots are assigned in)
    std::vector<std::string> imports;
    // dynsym index -> import_index (only for UND symbols)
    std::unordered_map<uint32_t, uint32_t> sym_to_import;
    // import_index -> is this a data symbol (OBJECT) rather than a function
    std::vector<bool> import_is_data;

    bool load(const std::string& path, std::string& err, uint32_t load_bias = 0);
    const std::string& sym_name(uint32_t idx) const;
    uint32_t resolve(const std::string& name) const;            // defined symbol -> guest addr (0 if none)
    uint32_t resolve_contains(const std::string& substr) const; // first defined FUNC whose name contains substr
    bool uses_hard_float() const;
    // Nearest defined FUNC at or below `addr` (for backtraces). Empty if none;
    // sets *off to addr - symbol value.
    std::string nearest_symbol(uint32_t addr, uint32_t* off = nullptr) const;
};
