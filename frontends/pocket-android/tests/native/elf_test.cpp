#include "arm_abi.h"
#include "compat/elf.h"
#include "elf_loader.h"
#include "runtime_libraries.h"
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
void word(std::vector<uint8_t>& bytes, size_t offset, uint32_t value) {
    std::memcpy(bytes.data() + offset, &value, 4);
}
std::vector<uint8_t> attributes(int abi) {
    // CPU name (string), CPU arch (ULEB), then optional VFP argument attribute.
    std::vector<uint8_t> fields{5, 'b', 'a', 'd', 'a', 0, 6, 5};
    if (abi >= 0) { fields.push_back(28); fields.push_back(uint8_t(abi)); }
    std::vector<uint8_t> bytes{'A', 0, 0, 0, 0, 'a', 'e', 'a', 'b', 'i', 0, 1, 0, 0, 0, 0};
    bytes.insert(bytes.end(), fields.begin(), fields.end());
    word(bytes, 1, uint32_t(bytes.size() - 1));
    word(bytes, 12, uint32_t(bytes.size() - 11));
    return bytes;
}
std::vector<uint8_t> fixture() {
    std::vector<uint8_t> bytes(1024, 0);
    auto* header = reinterpret_cast<Elf32_Ehdr*>(bytes.data());
    std::memcpy(header->e_ident, "\177ELF\1\1\1", 7);
    header->e_type = 3; header->e_machine = EM_ARM; header->e_version = 1;
    header->e_entry = 0x10081;
    header->e_ehsize = sizeof(Elf32_Ehdr);
    header->e_phoff = sizeof(Elf32_Ehdr);
    header->e_phnum = 2; header->e_phentsize = sizeof(Elf32_Phdr);
    header->e_shoff = 0x300; header->e_shnum = 3; header->e_shentsize = sizeof(Elf32_Shdr);
    header->e_flags = 0x5000002; // No float flags, as in the supplied bada games.
    auto* ph = reinterpret_cast<Elf32_Phdr*>(bytes.data() + header->e_phoff);
    ph[0] = {PT_LOAD, 0, 0x10000, 0, 1024, 1024, 5, 4096};
    ph[1] = {PT_DYNAMIC, 0x180, 0x10180, 0, 56, 56, 6, 4};
    word(bytes, 0x80, 0xe12fff1e); // ARM bx lr; no proprietary guest code.
    word(bytes, 0x100, 1); word(bytes, 0x104, 2); // DT_HASH: nbucket, nchain
    auto* symbol = reinterpret_cast<Elf32_Sym*>(bytes.data() + 0x120);
    symbol[1] = {1, 0x10081, 4, 0x12, 0, 1};
    std::memcpy(bytes.data() + 0x200, "\0demo\0", 6);
    auto* dynamic = reinterpret_cast<Elf32_Dyn*>(bytes.data() + 0x180);
    const std::pair<int32_t, uint32_t> tags[] = {
        {DT_STRTAB, 0x10200}, {DT_STRSZ, 6}, {DT_SYMTAB, 0x10120},
        {DT_SYMENT, sizeof(Elf32_Sym)}, {DT_HASH, 0x10100}, {DT_NULL, 0}
    };
    for (size_t i = 0; i < std::size(tags); ++i) {
        dynamic[i].d_tag = tags[i].first; dynamic[i].d_un.d_val = tags[i].second;
    }
    const auto attrs = attributes(1);
    std::memcpy(bytes.data() + 0x220, attrs.data(), attrs.size());
    auto* sections = reinterpret_cast<Elf32_Shdr*>(bytes.data() + 0x300);
    sections[1] = {0, SHT_ARM_ATTRIBUTES, 0, 0, 0x220, uint32_t(attrs.size()), 0, 0, 1, 0};
    sections[2] = {0, SHT_ARM_EXIDX, 2, 0x10260, 0x260, 16, 0, 0, 4, 8};
    return bytes;
}

void runtime_discovery_test(const std::filesystem::path& sandbox) {
    namespace fs = std::filesystem;
    require(!fs::exists(sandbox), "runtime test directory already exists");
    const auto engine = sandbox / ".build" / "desktop" / "native";
    fs::create_directories(engine);
    auto save_library = [&](const fs::path& path, bool hard) {
        fs::create_directories(path.parent_path());
        auto bytes = fixture();
        const auto attrs = attributes(hard ? 1 : 0);
        std::memcpy(bytes.data() + 0x220, attrs.data(), attrs.size());
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        require(bool(file), "could not write a runtime fixture");
    };
    try {
        const auto repository_hard = sandbox / "bada_libs" / "hard-float" / "libstdc++.so.6";
        const auto packaged_hard = engine / "bada_libs" / "hard-float" / "libstdc++.so.6";
        const auto packaged_soft = engine / "bada_libs" / "soft-float" / "libstdc++.so.6";
        save_library(repository_hard, true);
        save_library(packaged_soft, false);
        ElfImage hard, soft;
        hard.float_abi = ArmFloatAbi::hard;
        soft.float_abi = ArmFloatAbi::soft;
        require(fs::path(runtime::bundled_library(hard, engine, "libstdc++.so.6")) == repository_hard,
                "development runtime discovery depended on the working directory");
        require(fs::path(runtime::bundled_library(soft, engine, "libstdc++.so.6")) == packaged_soft,
                "soft-float game selected the hard-float bundle");
        save_library(packaged_hard, true);
        require(fs::path(runtime::bundled_library(hard, engine, "libstdc++.so.6")) == packaged_hard,
                "repository bundle overrode the packaged runtime");
        for (const char* name : {"libgcc_s.so.1", "libsupc++.so"}) {
            const auto path = packaged_hard.parent_path() / name;
            save_library(path, true);
            require(fs::path(runtime::bundled_library(hard, engine, name)) == path,
                    "bundled exception companion was not discovered");
        }
        save_library(packaged_hard, false);
        require(fs::path(runtime::bundled_library(hard, engine, "libstdc++.so.6")) == repository_hard,
                "runtime directory name concealed an incompatible ARM ABI");
        fs::remove(repository_hard);
        require(runtime::bundled_library(hard, engine, "libstdc++.so.6").empty(),
                "incompatible runtimes were used when no matching library existed");
        fs::remove_all(sandbox);
    } catch (...) {
        std::error_code ignored;
        fs::remove_all(sandbox, ignored);
        throw;
    }
}
}

int main() {
    const auto path = std::filesystem::temp_directory_path() /
        ("badanaut-elf-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".so");
    try {
        for (const auto& example : {std::pair<int, ArmFloatAbi>{-1, ArmFloatAbi::soft},
                                   {0, ArmFloatAbi::soft}, {1, ArmFloatAbi::hard},
                                   {2, ArmFloatAbi::unknown}, {3, ArmFloatAbi::compatible}}) {
            const auto bytes = attributes(example.first);
            require(arm_attributes_float_abi(bytes.data(), bytes.size()) == example.second,
                    "ARM VFP argument attributes were decoded incorrectly");
        }
        auto truncated = attributes(1); truncated.pop_back();
        bool rejected = false;
        try { arm_attributes_float_abi(truncated.data(), truncated.size()); }
        catch (const std::exception&) { rejected = true; }
        require(rejected, "truncated ARM attributes were accepted");
        auto bytes = fixture();
        auto save = [&] {
            std::ofstream file(path, std::ios::binary | std::ios::trunc);
            file.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
            require(bool(file), "could not write the synthetic ELF fixture");
        };
        save();
        ElfImage image;
        std::string error;
        require(image.load(path.string(), error, 0x11000000), error.c_str());
        require(image.dynsyms.size() == 2 && image.resolve("demo") == 0x11010081,
                "DT_HASH symbol count or symbol load bias was lost");
        require(image.float_abi == ArmFloatAbi::hard && image.uses_hard_float(),
                "bada ELF flags concealed the VFP calling convention");
        require(image.exidx == 0x11010260 && image.exidx_count == 2,
                "exception index was not relocated alongside its image");
        require(image.load(path.string(), error) && image.segments.size() == 1,
                "reloading an ELF retained stale image metadata");
        // EF_ARM_ABI_FLOAT_HARD / EF_ARM_ABI_FLOAT_SOFT, including Windows
        // builds whose minimal compatibility header omits those constants.
        for (const auto& example : {std::pair<uint32_t, ArmFloatAbi>{0x400, ArmFloatAbi::hard},
                                   {0x200, ArmFloatAbi::soft}}) {
            auto* header = reinterpret_cast<Elf32_Ehdr*>(bytes.data());
            header->e_shoff = 0; header->e_shnum = 0;
            header->e_flags = 0x5000002 | example.first;
            save();
            require(image.load(path.string(), error) && image.float_abi == example.second,
                    "stripped ELF did not use its header's float ABI flags");
        }
        runtime_discovery_test(path.parent_path() / (path.filename().string() + "-runtimes"));
        word(bytes, 0x104, 0x10000000); save();
        require(!image.load(path.string(), error), "out-of-range symbol count was accepted");
        bytes.resize(12); save();
        require(!image.load(path.string(), error), "truncated ELF header was accepted");
        std::filesystem::remove(path);
        puts("ARM flags/attributes, symbols, exception indexes and bundled runtime discovery passed");
        return 0;
    } catch (const std::exception& error) {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
        fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
