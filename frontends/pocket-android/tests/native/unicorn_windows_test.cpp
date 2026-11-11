#include <unicorn/unicorn.h>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
void check(uc_err error, const char* operation) {
    if (error != UC_ERR_OK) throw std::runtime_error(std::string(operation) + ": " + uc_strerror(error));
}

void run(uint32_t cache_size) {
    uc_engine* raw = nullptr;
    check(uc_open(UC_ARCH_ARM, UC_MODE_ARM, &raw), "open ARM engine");
    std::unique_ptr<uc_engine, decltype(&uc_close)> cpu(raw, uc_close);
    check(uc_ctl_set_tcg_buffer_size(cpu.get(), cache_size), "set JIT cache size");
    check(uc_ctl_set_cpu_model(cpu.get(), UC_CPU_ARM_CORTEX_A15), "select ARM model");
    constexpr uint32_t address = 0x100000, blocks = 65536;
    std::vector<uint32_t> code(blocks * 2);
    for (uint32_t index = 0; index < blocks; ++index) {
        code[index * 2] = 0xe2800001; // ADD r0,r0,#1
        code[index * 2 + 1] = 0xeaffffff; // B to the next basic block
    }
    check(uc_mem_map(cpu.get(), address, code.size() * 4, UC_PROT_ALL), "map ARM blocks");
    check(uc_mem_write(cpu.get(), address, code.data(), code.size() * 4), "write ARM blocks");
    for (unsigned pass = 0; pass < 2; ++pass) {
        uint32_t result = 0;
        check(uc_reg_write(cpu.get(), UC_ARM_REG_R0, &result), "reset result");
        check(uc_emu_start(cpu.get(), address, address + code.size() * 4, 10'000'000, 0), "execute ARM blocks");
        check(uc_reg_read(cpu.get(), UC_ARM_REG_R0, &result), "read result");
        if (result != blocks) throw std::runtime_error("JIT cache growth/recycling lost an ARM block");
    }
    printf("Windows JIT cache growth and recycling passed with %u MiB\n", cache_size / (1024 * 1024));
}
} // namespace

int main() {
    setbuf(stdout, nullptr);
    try {
        for (uint32_t size : {1u, 8u}) run(size * 1024 * 1024);
        return 0;
    } catch (const std::exception& error) {
        fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
