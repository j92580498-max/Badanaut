#define SDL_MAIN_HANDLED
#include <SDL2/SDL.h>
#include "cpu.h"
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace {
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
uint32_t runnable(Cpu& cpu, uint32_t entry) {
    const uint32_t object = cpu.alloc(32), table = cpu.alloc(12);
    cpu.write_u32(table + 8, entry);
    cpu.write_u32(object, table);
    return object;
}
void code(Cpu& cpu, uint32_t address, std::vector<uint32_t> words, size_t load, uint32_t target) {
    words[load] = 0xe59f3000 | ((words.size() - load - 2) * 4); // ldr r3, [pc, #literal]
    words.push_back(target);
    cpu.write_mem(address, words.data(), words.size() * 4);
}
void workers(Cpu& cpu) {
    const uint32_t sleep = cpu.bind_trampoline([](Cpu& c) {
        hle::call("_ZN3Osp4Base7Runtime6Thread5SleepEl", c);
    });
    const uint32_t join = cpu.bind_trampoline([](Cpu& c) {
        hle::call("_ZN3Osp4Base7Runtime6Thread4JoinEv", c);
    });
    // Preserve both a callee-saved core register and a VFP value across Sleep.
    code(cpu, 0x100200, {0xe92d4070, 0xe1a04000, 0xe3a0505a, 0xed940a04,
         0xe3a00000, 0, 0xe12fff33, 0xe5845008, 0xed840a05, 0xe8bd8070}, 5, sleep);
    code(cpu, 0x100280, {0xe92d4070, 0xe1a04000, 0xe5940004, 0, 0xe12fff33,
         0xe3a00009, 0xe5840008, 0xe8bd8070}, 3, join);
    const uint32_t sleeper = cpu.alloc(24), waiter = cpu.alloc(24);
    const uint32_t sleeping_target = runnable(cpu, 0x100200), waiting_target = runnable(cpu, 0x100280);
    cpu.write_u32(waiting_target + 4, sleeper);
    const float value = 3.5f;
    cpu.write_mem(sleeping_target + 16, &value, 4);
    require(cpu.construct_thread(waiter, waiting_target, 0) && cpu.construct_thread(sleeper, sleeping_target, 0), "worker construction failed");
    require(cpu.start_thread(waiter) && cpu.start_thread(sleeper), "worker start failed");
    require(!cpu.start_thread(sleeper), "worker started twice");
    const uint32_t stack = cpu.reg(UC_ARM_REG_SP);
    cpu.set_reg(UC_ARM_REG_R5, 0x1234);
    const float main_value = 9.25f;
    uc_reg_write(cpu.uc, UC_ARM_REG_S0, &main_value);
    require(cpu.pump_threads(), "initial worker slice failed");
    require(cpu.read_u32(sleeping_target + 8) == 0 && cpu.read_u32(waiting_target + 8) == 0, "Sleep or Join returned before suspension");
    require(cpu.pump_threads(), "sleeping worker did not resume");
    require(cpu.read_u32(sleeping_target + 8) == 0x5a && cpu.read_u32(waiting_target + 8) == 0, "Join did not wait for the worker to finish");
    float restored = 0;
    cpu.read_mem(sleeping_target + 20, &restored, 4);
    require(restored == value, "worker VFP context was lost");
    require(cpu.pump_threads() && cpu.read_u32(waiting_target + 8) == 9, "joined worker did not resume");
    uc_reg_read(cpu.uc, UC_ARM_REG_S0, &restored);
    require(cpu.reg(UC_ARM_REG_SP) == stack && cpu.reg(UC_ARM_REG_R5) == 0x1234 && restored == main_value, "workers changed the main context");
    require(cpu.join_thread(sleeper) && cpu.destroy_thread(sleeper) && cpu.destroy_thread(waiter), "finished worker cleanup failed");
    require(cpu.construct_thread(sleeper, sleeping_target, 0), "destroyed worker object could not be reused");
    require(cpu.destroy_thread(sleeper), "unstarted worker cleanup failed");

    // A later worker may delete an earlier completed worker while the host
    // is iterating its schedule. The remaining worker must still run.
    const uint32_t first = cpu.alloc(24), second = cpu.alloc(24), third = cpu.alloc(24);
    const uint32_t destroy_first = cpu.bind_trampoline([](Cpu& c) {
        c.set_ret32(c.destroy_thread(c.read_u32(c.arg32(0) + 4)) ? 1 : 0);
    });
    const uint32_t target1 = runnable(cpu, 0x100000), target2 = runnable(cpu, destroy_first), target3 = runnable(cpu, 0x100200);
    cpu.write_u32(target2 + 4, first);
    require(cpu.construct_thread(first, target1, 0) && cpu.construct_thread(second, target2, 0) && cpu.construct_thread(third, target3, 0), "cleanup schedule construction failed");
    require(cpu.start_thread(first) && cpu.start_thread(second) && cpu.start_thread(third) && cpu.pump_threads(), "deleting a completed worker broke the schedule");
    require(cpu.pump_threads() && cpu.read_u32(target3 + 8) == 0x5a, "remaining worker was skipped after deletion");
    require(cpu.destroy_thread(second) && cpu.destroy_thread(third), "cleanup schedule destruction failed");
}
void semaphores(Cpu& cpu) {
    const uint32_t acquire = cpu.bind_trampoline([](Cpu& c) {
        hle::call("_ZNK3Osp4Base7Runtime9Semaphore7AcquireEl", c);
    });
    code(cpu, 0x100300, {0xe92d4070, 0xe1a04000, 0xe5940004, 0xe594100c, 0,
         0xe12fff33, 0xe5840008, 0xe8bd8070}, 4, acquire);
    const uint32_t semaphore = cpu.alloc(24), worker = cpu.alloc(24), target = runnable(cpu, 0x100300);
    cpu.create_semaphore(semaphore, 0);
    cpu.write_u32(target + 4, semaphore);
    cpu.write_u32(target + 8, 0xfeed);
    cpu.write_u32(target + 12, -1);
    require(cpu.construct_thread(worker, target, 0) && cpu.start_thread(worker) && cpu.pump_threads(), "semaphore worker failed");
    require(cpu.read_u32(target + 8) == 0xfeed && cpu.pump_threads(), "empty semaphore did not suspend");
    cpu.release_semaphore(semaphore);
    require(cpu.pump_threads() && cpu.read_u32(target + 8) == 0, "Release did not wake Acquire successfully");
    require(!cpu.acquire_semaphore(semaphore, 0), "released semaphore token was consumed twice");
    require(cpu.destroy_thread(worker), "semaphore worker cleanup failed");
    cpu.write_u32(target + 8, 0xfeed);
    cpu.write_u32(target + 12, 1);
    require(cpu.construct_thread(worker, target, 0) && cpu.start_thread(worker) && cpu.pump_threads(), "timed worker failed");
    SDL_Delay(5);
    require(cpu.pump_threads() && cpu.read_u32(target + 8) == 0xa0000000u + 3001, "Acquire did not report E_TIMEOUT");
    cpu.release_semaphore(semaphore);
    require(cpu.acquire_semaphore(semaphore, 0), "timeout consumed a future token");
    require(cpu.destroy_thread(worker), "timed worker cleanup failed");
}
}
int main() {
    try {
        SDL_SetMainReady();
        require(SDL_Init(SDL_INIT_TIMER) == 0, SDL_GetError());
        const uint32_t instruction = 0xe12fff1e;
        ElfImage image;
        image.segments.push_back({0x100000, 4, 4096, 5, reinterpret_cast<const uint8_t*>(&instruction)});
        Cpu cpu;
        std::string error;
        require(cpu.init(image, error), error.c_str());
        workers(cpu);
        semaphores(cpu);
        SDL_Quit();
        puts("Worker suspension, ARM/VFP isolation, Join and semaphore token/timeout behavior passed");
        return 0;
    } catch (const std::exception& error) {
        fprintf(stderr, "FAIL: %s\n", error.what());
        SDL_Quit();
        return 1;
    }
}
