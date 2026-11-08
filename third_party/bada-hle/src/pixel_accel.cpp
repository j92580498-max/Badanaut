#include "pixel_accel.h"
#include "cpu.h"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <vector>

namespace {
// ARM pixel-copy kernel: 240 RGB565 pixels per row, a 256-pixel source
// stride, and 2x nearest-neighbour expansion to 480 XRGB8888 pixels.
// Green deliberately uses five bits, matching the guest's >>6 conversion.
// Check the entire routine, including loop bounds, strides and saved regs.
constexpr std::array<uint32_t, 32> kernel = {
    0xe92d5ff0, 0xe3a0c01f, 0xe3a04078, 0xe4905004,
    0xe2544001, 0xe00c6005, 0xe00c7325, 0xe00c85a5,
    0xe1a09186, 0xe0899587, 0xe0899988, 0xe00c6825,
    0xe00c7b25, 0xe00c8da5, 0xe1a0a186, 0xe08aa587,
    0xe08aa988, 0xe5819000, 0xe5819004, 0xe581a008,
    0xe581a00c, 0xe5819780, 0xe5819784, 0xe581a788,
    0xe581a78c, 0xe2811010, 0x1affffe7, 0xe2522001,
    0xe2800020, 0xe2811d1e, 0x1affffe2, 0xe8bd9ff0,
};
std::vector<uint16_t> source_pixels;
std::vector<uint32_t> expanded_pixels;
uc_engine* cached_engine = nullptr;
uint32_t cached_destination = 0, cached_rows = 0;

bool permitted(Cpu& cpu, uint32_t source, uint64_t source_end, uint32_t destination, uint64_t destination_end) {
    uc_mem_region* regions = nullptr;
    uint32_t count = 0;
    if (uc_mem_regions(cpu.uc, &regions, &count) != UC_ERR_OK) return false;
    bool readable = false, writable = false;
    for (uint32_t index = 0; index < count; ++index) {
        const auto& region = regions[index];
        readable |= source >= region.begin && source_end - 1 <= region.end && (region.perms & UC_PROT_READ);
        writable |= destination >= region.begin && destination_end - 1 <= region.end && (region.perms & UC_PROT_WRITE);
    }
    uc_free(regions);
    return readable && writable;
}

bool expand(Cpu& cpu) {
    cached_engine = nullptr;
    std::array<uint32_t, kernel.size()> current;
    if (uc_mem_read(cpu.uc, cpu.reg(UC_ARM_REG_PC), current.data(), sizeof current) != UC_ERR_OK || current != kernel) return false;
    const uint32_t source = cpu.arg32(0), destination = cpu.arg32(1), rows = cpu.arg32(2);
    // Unsupported calls retain the original ARM execution, including its
    // overlap, alignment and memory-fault behaviour. Bound staging allocations.
    if (!rows || rows > 4096 || ((source | destination) & 3)) return false;
    const uint64_t source_bytes = static_cast<uint64_t>(rows) * 512 - 32;
    const uint64_t destination_bytes = static_cast<uint64_t>(rows) * 3840;
    const uint64_t source_end = source + source_bytes, destination_end = destination + destination_bytes;
    if (source_end > 0x100000000ull || destination_end > 0x100000000ull ||
        (source < destination_end && destination < source_end) ||
        !permitted(cpu, source, source_end, destination, destination_end)) return false;

    try {
        source_pixels.resize(source_bytes / sizeof(uint16_t));
        expanded_pixels.resize(destination_bytes / sizeof(uint32_t));
    } catch (const std::bad_alloc&) {
        return false;
    }
    if (uc_mem_read(cpu.uc, source, source_pixels.data(), source_bytes) != UC_ERR_OK) return false;
    for (uint32_t row = 0; row < rows; ++row) {
        const uint16_t* input = source_pixels.data() + row * 256;
        uint32_t* output = expanded_pixels.data() + row * 960;
        for (uint32_t x = 0; x < 240; ++x) {
            const uint32_t pixel = input[x];
            const uint32_t rgb = ((pixel & 31) << 3) | (((pixel >> 6) & 31) << 11) | (((pixel >> 11) & 31) << 19);
            output[x * 2] = output[x * 2 + 1] = rgb;
        }
        std::memcpy(output + 480, output, 480 * sizeof(uint32_t));
    }
    if (uc_mem_write(cpu.uc, destination, expanded_pixels.data(), destination_bytes) != UC_ERR_OK) return false;

    // Preserve the complete routine's observable register results, not just
    // its framebuffer: pointer advances, zero row count, saved r3-r12 and SP.
    cpu.set_reg(UC_ARM_REG_R0, source + rows * 512);
    cpu.set_reg(UC_ARM_REG_R1, destination + rows * 3840);
    cpu.set_reg(UC_ARM_REG_R2, 0);
    const uint32_t cpsr = cpu.reg(UC_ARM_REG_CPSR);
    cpu.set_reg(UC_ARM_REG_CPSR, (cpsr & ~0xf0000000u) | 0x60000000u); // final subs: Z,C set; N,V clear
    cached_engine = cpu.uc;
    cached_destination = destination;
    cached_rows = rows;
    return true;
}
} // namespace

namespace pixel_accel {
size_t install(Cpu& cpu, const ElfImage& image) {
    cached_engine = nullptr;
    if (getenv("BADA_NO_PIXEL_ACCEL") || getenv("BADA_TRACE")) return 0;
    size_t found = 0;
    for (const auto& segment : image.segments) {
        if (!(segment.flags & 1) || segment.filesz < sizeof kernel) continue; // executable bytes only
        for (uint32_t offset = 0; offset <= segment.filesz - sizeof kernel; offset += 4) {
            if ((segment.vaddr + offset) & 3) continue;
            if (std::memcmp(segment.data + offset, kernel.data(), 3 * sizeof(uint32_t)) != 0 ||
                std::memcmp(segment.data + offset, kernel.data(), sizeof kernel) != 0) continue;
            // Relocations or runtime loading must not have changed the bytes.
            std::array<uint32_t, kernel.size()> mapped;
            if (uc_mem_read(cpu.uc, segment.vaddr + offset, mapped.data(), sizeof mapped) != UC_ERR_OK || mapped != kernel) continue;
            cpu.patch(segment.vaddr + offset, expand);
            printf("[cpu] native 2x pixel expansion @ 0x%x (verified ARM kernel)\n", segment.vaddr + offset);
            ++found;
        }
    }
    return found;
}

bool snapshot(Cpu& cpu, uint32_t destination, int width, int height, Expansion& output) {
    if (cached_engine != cpu.uc || cached_destination != destination || width != 480 ||
        height != static_cast<int>(cached_rows * 2)) return false;
    output = {source_pixels.data(), expanded_pixels.data(), cached_rows};
    return true;
}
} // namespace pixel_accel
