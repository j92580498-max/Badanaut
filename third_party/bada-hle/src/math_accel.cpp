#include "math_accel.h"
#include "cpu.h"
#include "guest.h"
#include "performance.h"
#include <algorithm>
#include <cfenv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace {
uint64_t fingerprint(const uint8_t* bytes, size_t count) {
    uint64_t value = 14695981039346656037ull;
    for (size_t i = 0; i < count; ++i) value = (value ^ bytes[i]) * 1099511628211ull;
    return value;
}
uint32_t word(const void* bytes) { uint32_t value; std::memcpy(&value, bytes, 4); return value; }
bool branch(uint32_t pc, uint32_t instruction, uint32_t opcode, uint32_t& target) {
    if ((instruction & 0xff000000u) != opcode) return false;
    const int32_t displacement = static_cast<int32_t>(instruction << 8) >> 6;
    const int64_t address = static_cast<int64_t>(pc) + 8 + displacement;
    if (address < 0 || address > UINT32_MAX) return false;
    target = static_cast<uint32_t>(address);
    return true;
}
bool normal_or_zero(float value) {
    const uint32_t magnitude = word(&value) & 0x7fffffffu;
    return !magnitude || (magnitude >= 0x00800000u && magnitude < 0x7f800000u);
}
bool safe_exact(double value) {
    const double magnitude = std::fabs(value);
    return magnitude == 0 || (magnitude >= std::numeric_limits<float>::min() &&
                             magnitude <= std::numeric_limits<float>::max());
}
} // namespace

MathAccelerator::MathAccelerator(Cpu& cpu) : cpu_(cpu),
    enabled_(!std::getenv("BADA_NO_MATH_ACCEL") && !std::getenv("BADA_TRACE")) {}

void MathAccelerator::allocated(uint32_t address, uint32_t bytes) {
    if (!enabled_ || bytes < 64 * 1024) return;
    // Runtime modules can be decompressed by guest code after allocation.
    // Data allocations exhaust two bounded passes and leave no steady work.
    candidates_.push_back({address, bytes, 0, 0, performance::now_ns() + 250'000'000});
    if (std::getenv("BADA_MATH_STATS")) std::printf("[math] candidate 0x%x bytes=%u\n", address, bytes);
}
void MathAccelerator::written(uint32_t address, uint32_t bytes) {
    if (!enabled_) return;
    for (auto& kernel : kernels_) {
        const auto overlaps = [&](uint32_t begin, size_t count) {
            return address < static_cast<uint64_t>(begin) + count &&
                static_cast<uint64_t>(address) + bytes > begin;
        };
        if (kernel.valid && (overlaps(kernel.address, kernel.code.size()) ||
            overlaps(kernel.add, kernel.add_code.size()) ||
            overlaps(kernel.multiply, kernel.multiply_code.size()) ||
            overlaps(kernel.subtract, kernel.subtract_code.size()))) {
            kernel.valid = false;
            cpu_.invalidate_code(address, bytes);
        }
    }
    if (bytes < 64 * 1024 || !cpu_.memory_ptr(address, bytes)) return;
    for (auto& candidate : candidates_) {
        if (address >= candidate.address && static_cast<uint64_t>(address) + bytes <=
                static_cast<uint64_t>(candidate.address) + candidate.bytes) {
            candidate.offset = 0;
            candidate.pass = 0;
            candidate.ready = performance::now_ns() + 250'000'000;
            return;
        }
    }
    allocated(address, bytes);
}
void MathAccelerator::freed(uint32_t address) {
    candidates_.erase(std::remove_if(candidates_.begin(), candidates_.end(),
        [address](const Candidate& candidate) { return candidate.address == address; }), candidates_.end());
    // Installed hooks retain guarded fallbacks if executable storage is reused.
}
void MathAccelerator::scan() {
    if (!enabled_) return;
    size_t budget = 256 * 1024;
    const uint64_t now = performance::now_ns();
    for (auto it = candidates_.begin(); it != candidates_.end() && budget;) {
        auto& candidate = *it;
        if (candidate.ready > now) { ++it; continue; }
        const auto* data = cpu_.memory_ptr(candidate.address, candidate.bytes);
        if (!data) { it = candidates_.erase(it); continue; }
        const uint32_t limit = ((candidate.bytes - 100) & ~3u) + 4;
        const uint32_t end = std::min<uint64_t>(limit, candidate.offset + budget);
        for (; candidate.offset < end; candidate.offset += 4, budget -= 4) {
            const auto* code = data + candidate.offset;
            if (word(code) == 0xe92d41f0u && word(code + 4) == 0xe1a04000u &&
                word(code + 8) == 0xe1a05001u) install(candidate.address + candidate.offset);
        }
        if (candidate.offset >= limit) {
            if (++candidate.pass == 2) { it = candidates_.erase(it); continue; }
            candidate.offset = 0;
            candidate.ready = now + 2'000'000'000;
        }
        ++it;
    }
}

bool MathAccelerator::install(uint32_t address) {
    if (!enabled_ || (address & 3)) return false;
    for (const auto& kernel : kernels_) if (kernel.address == address) return true;
    Kernel kernel{};
    kernel.address = address;
    const auto* code = cpu_.memory_ptr(address, kernel.code.size());
    if (!code) return false;
    std::memcpy(kernel.code.data(), code, kernel.code.size());
    auto normalized = kernel.code;
    constexpr unsigned calls[] = {5, 9, 13, 17, 19, 21, 24};
    for (unsigned index : calls) {
        uint32_t target = 0;
        const uint32_t opcode = index == 24 ? 0xea000000u : 0xeb000000u;
        if (!branch(address + index * 4, word(code + index * 4), opcode, target)) return false;
        uint32_t& helper = index < 19 ? kernel.multiply : kernel.add;
        if (helper && helper != target) return false;
        helper = target;
        std::memcpy(normalized.data() + index * 4, &opcode, 4);
    }
    // Fingerprints cover the full routine after normalizing only relative
    // calls, and every helper path reachable for normal/zero finite results.
    if (fingerprint(normalized.data(), normalized.size()) != 0x6bef9d25425857b8ull) return false;
    const auto* add = cpu_.memory_ptr(kernel.add, kernel.add_code.size());
    const auto* multiply = cpu_.memory_ptr(kernel.multiply, kernel.multiply_code.size());
    if (!add || !multiply || fingerprint(add, kernel.add_code.size()) != 0xfbd8fd6da2c762e7ull ||
        fingerprint(multiply, kernel.multiply_code.size()) != 0xb72ab19f5cb27771ull ||
        !branch(kernel.add + 8, word(add + 8), 0x4a000000u, kernel.subtract)) return false;
    const auto* subtract = cpu_.memory_ptr(kernel.subtract, kernel.subtract_code.size());
    if (!subtract || fingerprint(subtract, kernel.subtract_code.size()) != 0x0bf95d2f0a4b517eull) return false;
    std::memcpy(kernel.add_code.data(), add, kernel.add_code.size());
    std::memcpy(kernel.multiply_code.data(), multiply, kernel.multiply_code.size());
    std::memcpy(kernel.subtract_code.data(), subtract, kernel.subtract_code.size());
    kernels_.push_back(kernel);
    cpu_.patch(address, run);
    std::printf("[cpu] native soft-float dot product @ 0x%x (verified routine and arithmetic helpers)\n", address);
    return true;
}

bool MathAccelerator::evaluate(const float vector[4], const float matrix[13], float& result) {
    // These ARM helpers round to nearest and flush some underflows. Retain
    // their execution for exceptions/underflows and unsupported host FP modes.
    if (std::fegetround() != FE_TONEAREST) return false;
    volatile float products[4];
    for (unsigned i = 0; i < 4; ++i) {
        if (!normal_or_zero(vector[i]) || !normal_or_zero(matrix[i * 4])) return false;
        const double exact = static_cast<double>(vector[i]) * matrix[i * 4];
        if (!safe_exact(exact)) return false;
        products[i] = vector[i] * matrix[i * 4];
        if (!normal_or_zero(products[i])) return false;
    }
    volatile float sum = products[0];
    for (unsigned i = 1; i < 4; ++i) {
        const double exact = static_cast<double>(sum) + products[i];
        if (!safe_exact(exact)) return false;
        sum = sum + products[i]; // round after each operation; never use FMA
        if (!normal_or_zero(sum)) return false;
    }
    result = sum;
    return true;
}

bool MathAccelerator::run(Cpu& cpu) {
    const bool accelerated = cpu.math_accel_->execute(cpu.reg(UC_ARM_REG_PC));
    performance::math_call(accelerated);
    return accelerated;
}
bool MathAccelerator::execute(uint32_t address) {
    for (auto& kernel : kernels_) {
        if (kernel.address != address) continue;
        if (!kernel.valid) return false;
        const auto* code = cpu_.memory_ptr(address, kernel.code.size());
        const auto* add = cpu_.memory_ptr(kernel.add, kernel.add_code.size());
        const auto* multiply = cpu_.memory_ptr(kernel.multiply, kernel.multiply_code.size());
        const auto* subtract = cpu_.memory_ptr(kernel.subtract, kernel.subtract_code.size());
        if (!code || !add || !multiply || !subtract ||
            std::memcmp(code, kernel.code.data(), kernel.code.size()) ||
            std::memcmp(add, kernel.add_code.data(), kernel.add_code.size()) ||
            std::memcmp(multiply, kernel.multiply_code.data(), kernel.multiply_code.size()) ||
            std::memcmp(subtract, kernel.subtract_code.data(), kernel.subtract_code.size())) {
            kernel.valid = false;
            cpu_.invalidate_code(address, kernel.code.size());
            cpu_.invalidate_code(kernel.add, kernel.add_code.size());
            cpu_.invalidate_code(kernel.multiply, kernel.multiply_code.size());
            cpu_.invalidate_code(kernel.subtract, kernel.subtract_code.size());
            return false;
        }
        const uint32_t vector_address = cpu_.arg32(0), matrix_address = cpu_.arg32(1);
        const uint32_t sp = cpu_.reg(UC_ARM_REG_SP);
        if ((vector_address | matrix_address) & 3 || (sp & 7) || sp < 24 ||
            (cpu_.reg(UC_ARM_REG_CPSR) & (1u << 9))) return false;
        const auto overlaps_stack = [&](uint32_t begin, uint32_t bytes) {
            return begin < sp && static_cast<uint64_t>(begin) + bytes > sp - 24;
        };
        if (overlaps_stack(vector_address, 16) || overlaps_stack(matrix_address, 52)) return false;
        if (!cpu_.memory_ptr(sp - 24, 24) &&
            !(sp >= guest::STACK_TOP - guest::STACK_SIZE + 24 && sp <= guest::STACK_TOP)) return false;
        float vector[4], matrix[13], result = 0;
        const auto* vector_data = cpu_.memory_ptr(vector_address, sizeof vector);
        const auto* matrix_data = cpu_.memory_ptr(matrix_address, sizeof matrix);
        if (vector_data) std::memcpy(vector, vector_data, sizeof vector);
        else if (uc_mem_read(cpu_.uc, vector_address, vector, sizeof vector) != UC_ERR_OK) return false;
        if (matrix_data) std::memcpy(matrix, matrix_data, sizeof matrix);
        else if (uc_mem_read(cpu_.uc, matrix_address, matrix, sizeof matrix) != UC_ERR_OK) return false;
        if (!evaluate(vector, matrix, result)) return false;
        // Saved integer registers, SP, VFP registers and FPSCR stay untouched.
        // Write the integer soft-float result; Cpu returns through LR.
        cpu_.set_ret32(word(&result));
        return true;
    }
    return false;
}
