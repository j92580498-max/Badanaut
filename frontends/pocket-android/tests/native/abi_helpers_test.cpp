#define SDL_MAIN_HANDLED
#include <SDL2/SDL.h>
#include "cpu.h"
#include "guest.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <limits>
#include <iterator>
#include <stdexcept>
#include <vector>

namespace {
using Pair = std::array<uint32_t, 2>;
using Quad = std::array<uint32_t, 4>;
constexpr const char* names[] = {"__aeabi_uidiv", "__aeabi_idiv", "__aeabi_uidivmod", "__aeabi_idivmod",
                                "__aeabi_ldivmod", "__aeabi_uldivmod"};

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
        for (uint32_t index = 0; index < 6; ++index) {
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
        // The 64-bit helpers consume and return all four core argument regs.
        uint32_t arm64[sizeof arm / sizeof *arm];
        std::copy(std::begin(arm), std::end(arm), arm64);
        arm64[5] = 0xe8b4000f;
        arm64[7] = 0xe8a5000f;
        uint16_t thumb64[sizeof thumb / sizeof *thumb];
        std::copy(std::begin(thumb), std::end(thumb), thumb64);
        thumb64[5] = 0xcc0f;
        thumb64[7] = 0xc50f;
        cpu.write_mem(0x100200, arm64, sizeof arm64);
        cpu.write_mem(0x100280, thumb64, sizeof thumb64);
        uc_hook hook;
        require(uc_hook_add(cpu.uc, &hook, UC_HOOK_CODE, reinterpret_cast<void*>(count_host_calls),
                            &callbacks, guest::HLE_BASE + 4, guest::HLE_BASE + guest::HLE_SIZE - 1) == UC_ERR_OK,
                "cannot count host trampoline calls");
        for (uint32_t index = 0; index < 6; ++index) {
            const uint32_t address = cpu.read_u32(0x100300 + index * 4);
            require(native && index < 4 ? address >= guest::ABI_HELPER_BASE && address < guest::ABI_HELPER_BASE + guest::ABI_HELPER_SIZE
                                        : guest::in_hle(address), "ELF relocation selected the wrong helper path");
        }
    }

    template <size_t Words>
    std::vector<std::array<uint32_t, Words>> run(uint32_t helper, bool thumb,
                                               const std::vector<std::array<uint32_t, Words>>& input) {
        const uint32_t bytes = input.size() * sizeof(input.front());
        const uint32_t source = cpu.alloc(bytes), destination = cpu.alloc(bytes);
        cpu.write_mem(source, input.data(), bytes);
        const uint32_t sp = cpu.reg(UC_ARM_REG_SP);
        const uint64_t vfp = 0x400921fb54442d18ull;
        uc_reg_write(cpu.uc, UC_ARM_REG_D0, &vfp);
        for (int reg = UC_ARM_REG_R4; reg <= UC_ARM_REG_R11; ++reg) cpu.set_reg(reg, 0x12340000 + reg);
        const size_t before = callbacks;
        uint32_t result = 0;
        const uint32_t caller = Words == 4 ? (thumb ? 0x100281 : 0x100200) : (thumb ? 0x100181 : 0x100100);
        require(cpu.run_guest(caller,
                              {source, destination, static_cast<uint32_t>(input.size()), cpu.read_u32(0x100300 + helper * 4)}, result),
                "ARM/Thumb helper batch failed");
        require(callbacks - before == (guest::in_hle(cpu.read_u32(0x100300 + helper * 4)) ? input.size() : 0),
                "native arithmetic still crosses the host callback boundary");
        require(cpu.reg(UC_ARM_REG_SP) == sp, "division helpers changed the stack");
        for (int reg = UC_ARM_REG_R4; reg <= UC_ARM_REG_R11; ++reg)
            require(cpu.reg(reg) == 0x12340000u + reg, "division helpers clobbered a callee-saved register");
        uint64_t restored = 0;
        uc_reg_read(cpu.uc, UC_ARM_REG_D0, &restored);
        require(restored == vfp, "integer division touched the VFP result registers");
        std::vector<std::array<uint32_t, Words>> output(input.size());
        cpu.read_mem(destination, output.data(), bytes);
        cpu.free(source);
        cpu.free(destination);
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

Quad quad(uint64_t first, uint64_t second) {
    return {static_cast<uint32_t>(first), static_cast<uint32_t>(first >> 32),
            static_cast<uint32_t>(second), static_cast<uint32_t>(second >> 32)};
}
Quad expected64(uint32_t helper, const Quad& input) {
    const uint64_t a = input[0] | (uint64_t(input[1]) << 32), b = input[2] | (uint64_t(input[3]) << 32);
    if (!b) return quad(0, 0);
    if (helper == 5) return quad(a / b, a % b);
    const int64_t signed_a = static_cast<int64_t>(a), signed_b = static_cast<int64_t>(b);
    if (signed_a == std::numeric_limits<int64_t>::min() && signed_b == -1) return quad(a, 0);
    return quad(static_cast<uint64_t>(signed_a / signed_b), static_cast<uint64_t>(signed_a % signed_b));
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
            std::vector<Quad> wide;
            for (uint64_t numerator : {0ull, 1ull, 7ull, 0xfffffffffffffff9ull, 0x100000003ull,
                                       0x7fffffffffffffffull, 0x8000000000000000ull, 0xffffffffffffffffull})
                for (uint64_t denominator : {0ull, 1ull, 2ull, 7ull, 1000ull, 0xfffffffffffffffdull,
                                             0x100000001ull, 0x7fffffffffffffffull, 0x8000000000000000ull,
                                             0xffffffffffffffffull})
                    wide.push_back(quad(numerator, denominator));
            for (bool hard_float : {false, true}) {
                engine.cpu.hard_float = hard_float;
                for (bool thumb : {false, true}) for (uint32_t helper : {4u, 5u}) {
                    const auto output = engine.run(helper, thumb, wide);
                    for (size_t index = 0; index < wide.size(); ++index)
                        require(output[index] == expected64(helper, wide[index]), "64-bit quotient/remainder ABI is incorrect");
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
        puts("ARM/Thumb 32/64-bit integer ABI results, overflow, zero divisors, registers and callback elimination passed");
        return 0;
    } catch (const std::exception& error) {
        fprintf(stderr, "FAIL: %s\n", error.what());
        SDL_Quit();
        return 1;
    }
}
