#define SDL_MAIN_HANDLED
#include <SDL2/SDL.h>
#include "cpu.h"
#include "guest.h"
#include <array>
#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <vector>

namespace {
using Pair = std::array<uint32_t, 2>;
constexpr const char* names[] = {"__aeabi_uidiv", "__aeabi_idiv", "__aeabi_uidivmod", "__aeabi_idivmod"};

void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

void count_host_calls(uc_engine*, uint64_t, uint32_t, void* value) {
    ++*static_cast<size_t*>(value);
}

struct Engine {
    ElfImage image;
    Cpu cpu;
    size_t callbacks = 0;
    Engine(bool native) {
        SDL_setenv("BADA_NO_NATIVE_ABI", native ? "0" : "1", 1);
        static const uint32_t arm_return = 0xe12fff1e;
        image.segments.push_back({0x100000, 4, 4096, 7, reinterpret_cast<const uint8_t*>(&arm_return)});
        for (uint32_t index = 0; index < 4; ++index) {
            image.imports.push_back(names[index]);
            image.import_is_data.push_back(false);
            image.sym_to_import[index + 1] = index;
            image.relocs.push_back({0x100300 + index * 4, 22, index + 1}); // R_ARM_JUMP_SLOT
        }
        std::string error;
        const bool initialized = cpu.init(image, error);
        require(initialized, error.c_str());
        // Generated callers: push {r3-r7,lr}, retain pointers/count in r4-r7,
        // load each pair, BLX the imported helper, store both ABI result regs.
        const uint32_t arm[] = {
            0xe92d40f8, 0xe1a04000, 0xe1a05001, 0xe1a06002, 0xe1a07003,
            0xe8b40003, 0xe12fff37, 0xe8a50003, 0xe2566001, 0x1afffffa, 0xe8bd80f8,
        };
        const uint16_t thumb[] = {
            0xb5f8, 0x4604, 0x460d, 0x4616, 0x461f, 0xcc03,
            0x47b8, 0xc503, 0x3e01, 0xd1fa, 0xbdf8,
        };
        cpu.write_mem(0x100100, arm, sizeof arm);
        cpu.write_mem(0x100180, thumb, sizeof thumb);
        uc_hook hook;
        require(uc_hook_add(cpu.uc, &hook, UC_HOOK_CODE, reinterpret_cast<void*>(count_host_calls),
                            &callbacks, guest::HLE_BASE + 4, guest::HLE_BASE + guest::HLE_SIZE - 1) == UC_ERR_OK,
                "cannot count host trampoline calls");
        for (uint32_t index = 0; index < 4; ++index) {
            const uint32_t address = cpu.read_u32(0x100300 + index * 4);
            require(native ? address >= guest::ABI_HELPER_BASE && address < guest::ABI_HELPER_BASE + guest::ABI_HELPER_SIZE
                           : guest::in_hle(address), "ELF relocation selected the wrong helper path");
        }
    }
    ~Engine() { if (cpu.uc) uc_close(cpu.uc); }

    std::vector<Pair> run(uint32_t helper, bool thumb, const std::vector<Pair>& input) {
        const uint32_t bytes = input.size() * sizeof(Pair);
        const uint32_t source = cpu.alloc(bytes), destination = cpu.alloc(bytes);
        cpu.write_mem(source, input.data(), bytes);
        const uint32_t sp = cpu.reg(UC_ARM_REG_SP);
        for (int reg = UC_ARM_REG_R4; reg <= UC_ARM_REG_R11; ++reg) cpu.set_reg(reg, 0x12340000 + reg);
        const size_t before = callbacks;
        uint32_t result = 0;
        require(cpu.run_guest(thumb ? 0x100181 : 0x100100,
                              {source, destination, static_cast<uint32_t>(input.size()), cpu.read_u32(0x100300 + helper * 4)}, result),
                "ARM/Thumb helper batch failed");
        require(callbacks - before == (guest::in_hle(cpu.read_u32(0x100300 + helper * 4)) ? input.size() : 0),
                "native arithmetic still crosses the host callback boundary");
        require(cpu.reg(UC_ARM_REG_SP) == sp, "division helpers changed the stack");
        for (int reg = UC_ARM_REG_R4; reg <= UC_ARM_REG_R11; ++reg)
            require(cpu.reg(reg) == 0x12340000u + reg, "division helpers clobbered a callee-saved register");
        std::vector<Pair> output(input.size());
        cpu.read_mem(destination, output.data(), bytes);
        return output;
    }
};

Pair expected(uint32_t helper, Pair input) {
    const bool signed_division = helper == 1 || helper == 3, remainder = helper >= 2;
    uint32_t quotient = 0, mod = 0;
    if (input[1]) {
        if (signed_division) {
            const int64_t numerator = static_cast<int32_t>(input[0]), denominator = static_cast<int32_t>(input[1]);
            quotient = static_cast<uint32_t>(numerator / denominator);
            mod = static_cast<uint32_t>(numerator % denominator);
        } else {
            quotient = input[0] / input[1];
            mod = input[0] % input[1];
        }
    }
    return {quotient, remainder ? mod : input[1]};
}
} // namespace

int main() {
    try {
        SDL_SetMainReady();
        require(SDL_Init(SDL_INIT_TIMER) == 0, SDL_GetError());
        std::vector<Pair> input;
        for (uint32_t numerator : {0u, 1u, 7u, 0xfffffff9u, 0x7fffffffu, 0x80000000u, 0xffffffffu})
            for (uint32_t denominator : {0u, 1u, 2u, 7u, 0xfffffffdu, 0x7fffffffu, 0x80000000u, 0xffffffffu})
                input.push_back({numerator, denominator});
        uint32_t random = 0x87654321;
        for (int index = 0; index < 512; ++index) {
            random ^= random << 13; random ^= random >> 17; random ^= random << 5;
            const uint32_t numerator = random;
            random ^= random << 13; random ^= random >> 17; random ^= random << 5;
            input.push_back({numerator, random});
        }
        for (bool native : {false, true}) {
            Engine engine(native);
            for (bool thumb : {false, true}) {
                for (uint32_t helper = 0; helper < 4; ++helper) {
                    const auto output = engine.run(helper, thumb, input);
                    for (size_t index = 0; index < input.size(); ++index)
                        require(output[index] == expected(helper, input[index]), "division ABI result differs from integer arithmetic");
                }
            }
            std::vector<Pair> stress(100000, {0x87654321, 37});
            const auto start = std::chrono::steady_clock::now();
            const auto output = engine.run(2, true, stress);
            for (const auto& value : output) require(value == expected(2, stress.front()), "repeated division returned incorrect data");
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            printf("%s: 100000 Thumb divmod calls in %.2f ms\n", native ? "ARM JIT helpers" : "Host dispatch", ms);
        }
        SDL_Quit();
        puts("ARM/Thumb integer ABI results, overflow, zero divisors, registers and callback elimination passed");
        return 0;
    } catch (const std::exception& error) {
        fprintf(stderr, "FAIL: %s\n", error.what());
        SDL_Quit();
        return 1;
    }
}
