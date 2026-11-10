#define SDL_MAIN_HANDLED
#include <SDL2/SDL.h>
#include "cpu.h"
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <stdexcept>
#include <vector>

namespace {
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

uint32_t call(Cpu& cpu, const char* symbol, std::initializer_list<uint32_t> args) {
    int index = 0;
    for (uint32_t arg : args) cpu.set_reg(UC_ARM_REG_R0 + index++, arg);
    hle::call(symbol, cpu);
    return cpu.reg(UC_ARM_REG_R0);
}

constexpr const char* construct = "_ZN3Osp2Io4File9ConstructERKNS_4Base6StringES5_b";
constexpr const char* read = "_ZN3Osp2Io4File4ReadEPvi";
constexpr const char* seek = "_ZN3Osp2Io4File4SeekENS0_16FileSeekPositionEl";
constexpr const char* tell = "_ZNK3Osp2Io4File4TellEv";

void file_test(Cpu& cpu, const std::filesystem::path& package) {
    std::vector<uint8_t> pattern(128 * 1024 + 17);
    for (size_t i = 0; i < pattern.size(); ++i) pattern[i] = static_cast<uint8_t>(i * 13 + 7);
    std::filesystem::create_directories(package / "Res/data");
    std::ofstream fixture(package / "Res/data/resources.pack", std::ios::binary);
    fixture.write(reinterpret_cast<const char*>(pattern.data()), pattern.size());
    fixture.close();
    hle::set_package_root(package.string());

    const uint32_t name = cpu.alloc(32), mode = cpu.alloc(32);
    hle::osp_string_set(cpu, name, "/Res/data/resources.pack");
    constexpr const char* exists = "_ZN3Osp2Io4File11IsFileExistERKNS_4Base6StringE";
    require(call(cpu, exists, {name}) == 1, "file existence check rejected a packaged resource");
    hle::osp_string_set(cpu, name, "/rEs/DaTa/RESOURCES.PACK");
    require(call(cpu, exists, {name}) == 1, "file existence check did not use case-insensitive package lookup");
    hle::osp_string_set(cpu, name, "/Res/data/missing.pack");
    require(call(cpu, exists, {name}) == 0, "file existence check accepted a missing resource");
    hle::osp_string_set(cpu, name, "/Res/data/resources.pack");
    hle::osp_string_set(cpu, mode, "r");
    uint32_t first = cpu.alloc(20);
    const uint32_t second = cpu.alloc(20);
    for (const auto object : {first, second}) {
        call(cpu, "_ZN3Osp2Io4FileC1Ev", {object});
        require(call(cpu, construct, {object, name, mode, 0}) == 0, "resource pack open failed");
    }
    require(cpu.read_u32(first) == cpu.read_u32(second), "resource files did not share their class vtable");
    const uint32_t returned_name = cpu.alloc(32), attributes = cpu.alloc(128);
    require(call(cpu, "_ZN3Osp2Io4File7GetNameEv", {returned_name, first}) == returned_name &&
            hle::osp_string_get(cpu, returned_name) == "/Res/data/resources.pack",
            "File::GetName did not construct the returned guest String");
    call(cpu, "_ZN3Osp2Io14FileAttributesC1Ev", {attributes});
    require(call(cpu, "_ZN3Osp2Io4File13GetAttributesERKNS_4Base6StringERNS0_14FileAttributesE",
                 {returned_name, attributes}) == 0,
            "resource attributes could not resolve the opened file name");
    require(call(cpu, "_ZNK3Osp2Io14FileAttributes11GetFileSizeEv", {attributes}) == pattern.size() &&
            cpu.reg(UC_ARM_REG_R1) == 0,
            "resource size obtained through File::GetName was wrong");
    call(cpu, "_ZN3Osp4Base6StringD1Ev", {returned_name});

    const uint32_t destination = cpu.alloc(pattern.size());
    require(call(cpu, read, {first, destination, 13}) == 13, "short pack read failed");
    require(call(cpu, tell, {first}) == 13 && call(cpu, tell, {second}) == 0,
            "readers of one pack share their cursor");
    require(call(cpu, read, {second, destination, static_cast<uint32_t>(pattern.size())}) == pattern.size(),
            "read spanning the staging chunks was truncated");
    std::vector<uint8_t> actual(pattern.size());
    cpu.read_mem(destination, actual.data(), actual.size());
    require(actual == pattern, "streaming pack read changed resource bytes");
    require(call(cpu, read, {second, destination, 1}) == 0, "pack read passed EOF");
    require(call(cpu, seek, {first, 2, static_cast<uint32_t>(-4)}) == 0, "seek from pack end failed");
    require(call(cpu, tell, {first}) == pattern.size() - 4, "end-relative seek used the wrong cursor");
    require(call(cpu, read, {first, destination, 8}) == 4, "EOF did not shorten the final read");

    // Nova's CReadFile calls its Osp file's deleting destructor through vtable
    // slot 1, without importing FileD1. Exercise both destructor entries.
    for (const auto& entry : {std::pair<uint32_t, unsigned>{first, 1}, {second, 0}}) {
        const uint32_t vtable = cpu.read_u32(entry.first);
        uint32_t result = 0;
        require(cpu.run_guest(cpu.read_u32(vtable + entry.second * 4), {entry.first}, result),
                "virtual file destructor faulted");
        require(call(cpu, seek, {entry.first, 0, 0}) != 0,
                "virtual file destructor retained the open resource pack");
        call(cpu, "_ZN3Osp2Io4File7GetNameEv", {returned_name, entry.first});
        require(hle::osp_string_get(cpu, returned_name).empty(), "closed file retained its resource name");
        call(cpu, "_ZN3Osp4Base6StringD1Ev", {returned_name});
    }

    // ByteBuffer reads use the same stream translation and publish the actual
    // byte count through the buffer limit.
    require(cpu.allocation_size(first) == 0 && cpu.allocation_size(second) == 20,
            "complete and deleting destructors have the same allocation lifetime");
    first = cpu.alloc(20);
    call(cpu, "_ZN3Osp2Io4FileC1Ev", {first});
    require(call(cpu, construct, {first, name, mode, 0}) == 0, "pack reopen failed");
    const uint32_t buffer = cpu.alloc(36);
    call(cpu, "_ZN3Osp4Base10ByteBufferC1Ev", {buffer});
    call(cpu, "_ZN3Osp4Base10ByteBuffer9ConstructEi", {buffer, 32});
    call(cpu, seek, {first, 2, static_cast<uint32_t>(-4)});
    require(call(cpu, "_ZN3Osp2Io4File4ReadERNS_4Base10ByteBufferE", {first, buffer}) == 0,
            "ByteBuffer pack read failed");
    require(call(cpu, "_ZNK3Osp4Base10BufferBase8GetLimitEv", {buffer}) == 4,
            "ByteBuffer limit includes bytes beyond EOF");
    call(cpu, "_ZN3Osp2Io4FileD1Ev", {first});
    call(cpu, "_ZN3Osp4Base10ByteBufferD1Ev", {buffer});

    // New Game removes an old checkpoint and writes a fresh one through the
    // Osp raw-file API. Its contents must survive the virtual destructor.
    hle::osp_string_set(cpu, name, "/Home/session/checkpoint.dat");
    hle::osp_string_set(cpu, mode, "w");
    call(cpu, "_ZN3Osp2Io4FileC1Ev", {first});
    require(call(cpu, construct, {first, name, mode, 0}) == 0, "new checkpoint open failed");
    // Bada Write returns E_SUCCESS, while Read returns the byte count. The
    // Marmalade wrapper reports a save failure if Write returns that count.
    cpu.write_mem(destination, pattern.data(), pattern.size());
    require(call(cpu, "_ZN3Osp2Io4File5WriteEPKvi", {first, destination, static_cast<uint32_t>(pattern.size())}) == 0,
            "checkpoint write spanning staging chunks was truncated");
    uint32_t result = 0;
    require(cpu.run_guest(cpu.read_u32(cpu.read_u32(first) + 4), {first}, result),
            "checkpoint virtual close failed");
    first = cpu.alloc(20);
    call(cpu, "_ZN3Osp2Io4FileC1Ev", {first});
    hle::osp_string_set(cpu, mode, "r");
    require(call(cpu, construct, {first, name, mode, 0}) == 0, "written checkpoint could not be read");
    require(call(cpu, read, {first, destination, static_cast<uint32_t>(pattern.size())}) == pattern.size(),
            "checkpoint read returned the wrong length");
    cpu.read_mem(destination, actual.data(), actual.size());
    require(actual == pattern, "checkpoint bytes changed on disk");
    call(cpu, "_ZN3Osp2Io4FileD1Ev", {first});
    hle::osp_string_set(cpu, mode, "w");
    require(call(cpu, construct, {first, name, mode, 0}) == 0, "checkpoint truncate open failed");
    require(call(cpu, "_ZN3Osp2Io4File5WriteEPKvi", {first, destination, 4}) == 0, "checkpoint replacement failed");
    call(cpu, "_ZN3Osp2Io4FileD1Ev", {first});
    require(std::filesystem::file_size(package / "Home/session/checkpoint.dat") == 4,
            "write mode retained the old checkpoint tail");
    hle::osp_string_set(cpu, mode, "a");
    require(call(cpu, construct, {first, name, mode, 0}) == 0, "checkpoint append open failed");
    call(cpu, seek, {first, 0, 0});
    require(call(cpu, "_ZN3Osp2Io4File5WriteEPKvi", {first, destination, 4}) == 0 &&
            call(cpu, tell, {first}) == 8, "append did not update the checkpoint cursor");
    call(cpu, "_ZN3Osp2Io4FileD1Ev", {first});
    require(call(cpu, "_ZN3Osp2Io4File6RemoveERKNS_4Base6StringE", {name}) == 0,
            "New Game did not remove the checkpoint");
    hle::osp_string_set(cpu, mode, "r");
    require(call(cpu, construct, {first, name, mode, 0}) != 0,
            "removed checkpoint is still readable");
}
void directory_test(Cpu& cpu, const std::filesystem::path& package) {
    std::filesystem::create_directories(package / "Data");
    std::ofstream(package / "Data/Game.s3e") << "package";
    std::ofstream(package / "Home/game.S3E") << "overlay";
    std::ofstream(package / "Home/save.dat") << "save";
    const uint32_t directory = cpu.alloc(32), name = cpu.alloc(32), output = cpu.alloc(32);
    hle::osp_string_set(cpu, name, "/Home");
    call(cpu, "_ZN3Osp2Io9DirectoryC1Ev", {directory});
    require(call(cpu, "_ZN3Osp2Io9Directory9ConstructERKNS_4Base6StringE", {directory, name}) == 0, "private directory construction failed");
    const uint32_t enumeration = call(cpu, "_ZN3Osp2Io9Directory5ReadNEv", {directory});
    require(enumeration != 0, "private directory enumeration failed");
    const uint32_t table = cpu.read_u32(enumeration);
    uint32_t result = 0;
    std::set<std::string> names;
    while (true) {
        require(cpu.run_guest(cpu.read_u32(table + 20), {enumeration}, result), "MoveNext faulted");
        if (result != 0) break;
        require(cpu.run_guest(cpu.read_u32(table + 16), {enumeration}, result) && result, "GetCurrent lost its entry");
        call(cpu, "_ZNK3Osp2Io8DirEntry7GetNameEv", {output, result});
        names.insert(hle::osp_string_get(cpu, output));
    }
    require(names.count("game.S3E") == 1 && names.count("Game.s3e") == 0 && names.count("save.dat") == 1, "Home did not merge package data with overlay precedence");
    require(cpu.run_guest(cpu.read_u32(table + 24), {enumeration}, result), "enumerator reset failed");
    require(cpu.run_guest(cpu.read_u32(table + 20), {enumeration}, result) && result == 0, "reset did not restart enumeration");
    require(cpu.run_guest(cpu.read_u32(table + 4), {enumeration}, result), "enumerator deleting destructor failed");
    require(cpu.allocation_size(enumeration) == 0, "enumerator cleanup retained the object");
    call(cpu, "_ZN3Osp2Io9DirectoryD1Ev", {directory});
}
} // namespace

int main() {
    const auto package = std::filesystem::temp_directory_path() /
        ("badanaut-files-" + std::to_string(SDL_GetPerformanceCounter()));
    try {
        SDL_SetMainReady();
        require(SDL_Init(SDL_INIT_TIMER) == 0, SDL_GetError());
        ElfImage image;
        const uint32_t instruction = 0xe12fff1e;
        image.segments.push_back({0x100000, 4, 4096, 5, reinterpret_cast<const uint8_t*>(&instruction)});
        Cpu cpu;
        std::string error;
        require(cpu.init(image, error), error.c_str());
        file_test(cpu, package);
        directory_test(cpu, package);
        SDL_Quit();
        std::filesystem::remove_all(package);
        puts("Streaming resource reads, independent cursors and virtual file cleanup passed");
        return 0;
    } catch (const std::exception& error) {
        fprintf(stderr, "FAIL: %s\n", error.what());
        SDL_Quit();
        std::filesystem::remove_all(package);
        return 1;
    }
}
