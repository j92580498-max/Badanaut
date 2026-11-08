#define SDL_MAIN_HANDLED
#include <SDL2/SDL.h>
#include "cpu.h"
#include "guest.h"
#include "arm_pixel_fixture.h"
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace {
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

void entered_body(uc_engine*, uint64_t, uint32_t, void* value) { ++*static_cast<size_t*>(value); }

constexpr auto& fixture = arm_pixel_fixture;

struct Engine {
    std::array<uint8_t, 4096> program{};
    ElfImage image;
    Cpu cpu;
    size_t body_entries = 0;
    Engine(bool native, bool changed = false, bool executable = true, bool trace = false) {
        if (native) unsetenv("BADA_NO_PIXEL_ACCEL");
        else SDL_setenv("BADA_NO_PIXEL_ACCEL", "1", 1);
        if (trace) SDL_setenv("BADA_TRACE", "1", 1);
        else unsetenv("BADA_TRACE");
        std::memcpy(program.data(), fixture, sizeof fixture);
        if (changed) {
            const uint32_t different_green_shift = 0xe00c72a5; // and r7,ip,r5,lsr #5
            std::memcpy(program.data() + 6 * 4, &different_green_shift, 4);
        }
        const uint16_t thumb[] = {0xb510, 0x4798, 0xbd10}; // push {r4,lr}; blx r3; pop {r4,pc}
        std::memcpy(program.data() + 0x100, thumb, sizeof thumb);
        const uint32_t bytes = static_cast<uint32_t>(program.size());
        image.segments.push_back({0x100000, bytes, bytes, executable ? 7u : 6u, program.data()});
        std::string error;
        const bool initialized = cpu.init(image, error);
        require(initialized, error.c_str());
        uc_hook hook;
        require(uc_hook_add(cpu.uc, &hook, UC_HOOK_CODE, reinterpret_cast<void*>(entered_body),
                            &body_entries, 0x100004, 0x100004) == UC_ERR_OK, "cannot observe original pixel body");
    }
    ~Engine() { if (cpu.uc) uc_close(cpu.uc); }

    void call(uint32_t source, uint32_t destination, uint32_t rows, bool thumb, bool accelerated) {
        const uint32_t sp = cpu.reg(UC_ARM_REG_SP);
        for (int reg = UC_ARM_REG_R4; reg <= UC_ARM_REG_R12; ++reg) cpu.set_reg(reg, 0x12340000 + reg);
        const size_t before = body_entries;
        uint32_t result = 0;
        require(cpu.run_guest(thumb ? 0x100101 : 0x100000, {source, destination, rows, 0x100000}, result),
                "pixel-copy guest call failed");
        require(body_entries - before == (accelerated ? 0u : 1u), "pixel call used the wrong execution path");
        require(result == source + rows * 512 && cpu.reg(UC_ARM_REG_R1) == destination + rows * 3840 &&
                cpu.reg(UC_ARM_REG_R2) == 0 && cpu.reg(UC_ARM_REG_R3) == 0x100000,
                "pixel acceleration changed the routine's register results");
        require(cpu.reg(UC_ARM_REG_SP) == sp && cpu.reg(UC_ARM_REG_LR) == (thumb ? 0x100105u : guest::HLE_RETURN),
                "pixel acceleration changed SP or LR");
        for (int reg = UC_ARM_REG_R4; reg <= UC_ARM_REG_R12; ++reg)
            require(cpu.reg(reg) == 0x12340000u + reg, "pixel acceleration clobbered a saved register");
        require((cpu.reg(UC_ARM_REG_CPSR) & 0xf0000000) == 0x60000000, "pixel acceleration changed result flags");
    }
};

std::vector<uint32_t> reference(const std::vector<uint16_t>& source, uint32_t rows, bool changed) {
    std::vector<uint32_t> output(rows * 960);
    for (uint32_t y = 0; y < rows * 2; ++y) {
        for (uint32_t x = 0; x < 480; ++x) {
            const uint16_t rgb565 = source[y / 2 * 256 + x / 2];
            const uint32_t red = (rgb565 / 2048) & 31;
            // The modified instruction affects only the first pixel of each
            // paired 32-bit load; the second pixel retains its original shift.
            const uint32_t green = (rgb565 / (changed && !(x / 2 & 1) ? 32 : 64)) & 31;
            const uint32_t blue = rgb565 & 31;
            output[y * 480 + x] = (red * 8 << 16) | (green * 8 << 8) | blue * 8;
        }
    }
    return output;
}

void pixels_test(bool native, uint32_t rows, bool thumb, bool changed = false, bool executable = true, bool trace = false,
                 bool modified_after_load = false) {
    Engine engine(native, changed, executable, trace);
    if (modified_after_load) engine.cpu.write_u32(0x100018, 0xe00c72a5);
    std::vector<uint16_t> input(rows * 256 - 16, 0xdead);
    for (uint32_t y = 0; y < rows; ++y)
        for (uint32_t x = 0; x < 240; ++x) input[y * 256 + x] = (y * 240 + x) & 0xffff;
    const uint32_t source_bytes = input.size() * 2, destination_bytes = rows * 3840;
    const uint32_t source = engine.cpu.alloc(source_bytes + 32) + 16;
    const uint32_t destination = engine.cpu.alloc(destination_bytes + 32) + 16;
    engine.cpu.write_mem(source, input.data(), source_bytes);
    std::vector<uint32_t> initial(destination_bytes / 4 + 8, 0xabcdef01);
    engine.cpu.write_mem(destination - 16, initial.data(), destination_bytes + 32);
    engine.call(source, destination, rows, thumb, native && !changed && executable && !trace && !modified_after_load);
    std::vector<uint32_t> output(initial.size());
    engine.cpu.read_mem(destination - 16, output.data(), destination_bytes + 32);
    const auto expected = reference(input, rows, changed || modified_after_load);
    for (size_t index = 0; index < output.size(); ++index) {
        const uint32_t value = index < 4 || index >= 4 + expected.size() ? 0xabcdef01 : expected[index - 4];
        require(output[index] == value, "expanded pixels or destination guard bytes differ from ARM semantics");
    }
    std::vector<uint16_t> after(input.size());
    engine.cpu.read_mem(source, after.data(), source_bytes);
    require(after == input, "pixel expansion changed source pixels or source-row padding");
}

void overlap_test() {
    std::vector<uint32_t> results[2];
    for (bool native : {false, true}) {
        Engine engine(native);
        std::vector<uint32_t> input(1920, 0x12345678);
        const uint32_t buffer = engine.cpu.alloc(input.size() * 4);
        engine.cpu.write_mem(buffer, input.data(), input.size() * 4);
        engine.call(buffer, buffer, 2, true, false);
        results[native].resize(input.size());
        engine.cpu.read_mem(buffer, results[native].data(), input.size() * 4);
    }
    require(results[0] == results[1], "overlapping buffers did not retain original guest execution");
}

void protected_memory_test(bool source_readable) {
    Engine engine(true);
    const uint32_t source = engine.cpu.alloc(4096, 4096), destination = engine.cpu.alloc(4096, 4096);
    engine.cpu.write_u32(source, 0x12345678);
    engine.cpu.write_u32(destination, 0xabcdef01);
    require(uc_mem_protect(engine.cpu.uc, source_readable ? destination : source, 4096,
                           source_readable ? UC_PROT_READ : UC_PROT_WRITE) == UC_ERR_OK,
            "cannot protect test pixel memory");
    uint32_t result = 0;
    require(!engine.cpu.run_guest(0x100000, {source, destination, 1}, result) && engine.body_entries == 1,
            "protected memory bypassed guest access permissions");
    require(engine.cpu.read_u32(destination) == 0xabcdef01, "protected pixel call modified the destination");
}

void benchmark(bool native) {
    Engine engine(native);
    std::vector<uint16_t> input(320 * 256, 0x7bef);
    const uint32_t source = engine.cpu.alloc(input.size() * 2), destination = engine.cpu.alloc(320 * 3840);
    engine.cpu.write_mem(source, input.data(), input.size() * 2);
    engine.call(source, destination, 320, true, native); // warm code and staging storage
    const auto start = std::chrono::steady_clock::now();
    for (int frame = 0; frame < 30; ++frame) engine.call(source, destination, 320, true, native);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    printf("%s: 30 complete 480x640 framebuffer expansions in %.2f ms\n", native ? "Native pixel copy" : "Guest ARM pixel copy", ms);
}
} // namespace

int main() {
    try {
        SDL_SetMainReady();
        require(SDL_Init(SDL_INIT_TIMER) == 0, SDL_GetError());
        for (bool native : {false, true})
            for (bool thumb : {false, true})
                for (uint32_t rows : {1u, 2u, 320u}) pixels_test(native, rows, thumb);
        pixels_test(true, 2, true, true);          // one changed opcode must run normally
        pixels_test(true, 2, true, false, false); // matching non-executable data is not a kernel
        pixels_test(true, 2, true, false, true, true); // tracing keeps original guest execution
        pixels_test(true, 2, true, false, true, false, true); // code modified after installation runs normally
        overlap_test();
        protected_memory_test(false);
        protected_memory_test(true);
        benchmark(false);
        benchmark(true);
        unsetenv("BADA_NO_PIXEL_ACCEL");
        unsetenv("BADA_TRACE");
        SDL_Quit();
        puts("Pixel values, ARM/Thumb interworking, register results, guards and fallback paths passed");
        return 0;
    } catch (const std::exception& error) {
        fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
