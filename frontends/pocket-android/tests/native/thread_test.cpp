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
uint32_t invoke(Cpu& cpu, const char* name, std::initializer_list<uint32_t> args) {
    int index = 0;
    for (uint32_t value : args) cpu.set_reg(UC_ARM_REG_R0 + index++, value);
    hle::call(name, cpu);
    return cpu.reg(UC_ARM_REG_R0);
}
uint32_t thunk(Cpu& cpu, uint32_t address, uint16_t adjustment, hle::Handler handler) {
    // A real Thumb multiple-inheritance thunk adjusts the interface pointer
    // before tail-calling the host test callback.
    const uint16_t instructions[] = {static_cast<uint16_t>(0x3800 | adjustment), 0x4b01, 0x4718, 0xbf00};
    cpu.write_mem(address, instructions, sizeof instructions);
    cpu.write_u32(address + 8, cpu.bind_trampoline(handler));
    return address | 1;
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

void event_threads(Cpu& cpu) {
    constexpr const char* construct = "_ZN3Osp4Base7Runtime6Thread9ConstructENS1_10ThreadTypeElNS1_14ThreadPriorityE";
    constexpr const char* start = "_ZN3Osp4Base7Runtime6Thread5StartEv";
    constexpr const char* stop = "_ZN3Osp4Base7Runtime6Thread4StopEv";
    constexpr const char* timer_start = "_ZN3Osp4Base7Runtime5Timer5StartEi";
    constexpr const char* timer_construct = "_ZN3Osp4Base7Runtime5Timer9ConstructERKNS1_19ITimerEventListenerE";
    const uint32_t owner = cpu.alloc(64), worker = owner + 4, listener = owner + 8, timer = cpu.alloc(32);
    cpu.write_u32(owner + 24, timer);
    const uint32_t run = thunk(cpu, 0x100380, 4, [](Cpu& c) {
        const uint32_t object = c.arg32(0);
        c.write_u32(object + 40, c.read_u32(object + 40) + 1);
        c.set_ret32(0);
    });
    const uint32_t on_start = thunk(cpu, 0x1003a0, 4, [](Cpu& c) {
        const uint32_t object = c.arg32(0);
        c.write_u32(object + 12, c.read_u32(object + 12) + 1);
        c.write_u32(object + 16, c.current_thread());
        c.write_u32(object + 52, c.start_timer(c.read_u32(object + 24), 1));
        c.set_ret32(1);
    });
    const uint32_t on_stop = thunk(cpu, 0x1003c0, 4, [](Cpu& c) {
        const uint32_t object = c.arg32(0);
        c.write_u32(object + 44, c.read_u32(object + 44) + 1);
        c.write_u32(object + 48, c.current_thread());
        c.cancel_timer(c.read_u32(object + 24));
        c.set_ret32(0);
    });
    const uint32_t on_timer = thunk(cpu, 0x1003e0, 8, [](Cpu& c) {
        const uint32_t object = c.arg32(0), timer = c.arg32(1);
        c.write_u32(object + 20, c.read_u32(object + 20) + 1);
        c.write_u32(object + 28, timer);
        c.write_u32(object + 32, c.current_thread());
        c.write_u32(object + 36, c.reg(UC_ARM_REG_SP));
        c.set_ret32(0);
    });
    const uint32_t table = cpu.make_vtable(6);
    cpu.write_u32(table + 8, run);
    cpu.write_u32(table + 12, on_start);
    cpu.write_u32(table + 16, on_stop);
    cpu.write_u32(worker, table);
    const uint32_t timer_table = cpu.make_vtable(3);
    cpu.write_u32(timer_table + 8, on_timer);
    cpu.write_u32(listener, timer_table);
    invoke(cpu, "_ZN3Osp4Base7Runtime5TimerC1Ev", {timer});
    require(invoke(cpu, timer_construct, {timer, listener}) == 0, "worker timer construction failed");
    require(invoke(cpu, construct, {worker, 2, 65536, 1}) != 0, "invalid ThreadType was accepted");
    require(invoke(cpu, construct, {worker, 1, 0xffffffffu, 1}) != 0, "negative thread stack size was accepted");
    require(invoke(cpu, construct, {worker, 1, 65536, 1}) == 0, "event-driven Thread::Construct is missing");
    require(invoke(cpu, construct, {worker, 1, 65536, 1}) != 0, "event-driven thread was constructed twice");
    require(invoke(cpu, start, {worker}) == 0 && cpu.read_u32(owner + 12) == 0, "Start ran OnStart inside an HLE callback");
    const uint32_t main_stack = cpu.reg(UC_ARM_REG_SP), main_thread = cpu.current_thread();
    cpu.set_reg(UC_ARM_REG_R5, 0x89abcdef);
    require(cpu.pump_threads() && cpu.read_u32(owner + 12) == 1 && cpu.read_u32(owner + 40) == 0 &&
            cpu.read_u32(owner + 16) == worker && cpu.read_u32(owner + 52) == 1,
            "event thread used Run or lost the adjusted OnStart pointer/current-thread identity");
    require(!cpu.join_thread(worker), "event loop finished when OnStart returned true");
    uint32_t dispatched = 0;
    SDL_Delay(3);
    require(cpu.pump_timers(dispatched) && dispatched == 1 && cpu.read_u32(owner + 20) == 0,
            "worker timer callback ran on the main context");
    require(cpu.pump_threads() && cpu.read_u32(owner + 20) == 1 && cpu.read_u32(owner + 28) == timer &&
            cpu.read_u32(owner + 32) == worker && cpu.read_u32(owner + 36) != main_stack,
            "timer lost its listener adjustment, Timer argument or worker stack");
    require(cpu.current_thread() == main_thread && cpu.reg(UC_ARM_REG_SP) == main_stack &&
            cpu.reg(UC_ARM_REG_R5) == 0x89abcdef, "event callback changed the main ARM context");

    // Foreground code can restart the timer from the main thread without
    // moving the listener out of its original event loop.
    require(invoke(cpu, timer_start, {timer, 1}) == 0, "foreground timer restart failed");
    SDL_Delay(3);
    require(cpu.pump_timers(dispatched) && cpu.pump_threads() && cpu.read_u32(owner + 20) == 2 &&
            cpu.read_u32(owner + 32) == worker, "foreground restart changed the timer owner");
    invoke(cpu, timer_start, {timer, 1});
    SDL_Delay(3);
    require(cpu.pump_timers(dispatched) && dispatched == 1, "timer event was not queued");
    invoke(cpu, "_ZN3Osp4Base7Runtime5Timer6CancelEv", {timer});
    require(cpu.pump_threads() && cpu.read_u32(owner + 20) == 2, "Cancel delivered an already queued timer callback");

    invoke(cpu, timer_start, {timer, 1});
    SDL_Delay(3);
    require(cpu.pump_timers(dispatched) && dispatched == 1, "destruction test did not queue a timer");
    invoke(cpu, "_ZN3Osp4Base7Runtime5TimerD1Ev", {timer});
    invoke(cpu, "_ZN3Osp4Base7Runtime5TimerC1Ev", {timer});
    require(invoke(cpu, timer_construct, {timer, listener}) == 0, "Timer address could not be reused after destruction");
    require(cpu.pump_threads() && cpu.read_u32(owner + 20) == 2, "destroyed Timer event reached a new object at the same address");
    require(invoke(cpu, stop, {worker}) == 0 && cpu.read_u32(owner + 44) == 0, "Stop ran OnStop inside an HLE callback");
    require(cpu.pump_threads() && cpu.read_u32(owner + 44) == 1 && cpu.read_u32(owner + 48) == worker &&
            cpu.join_thread(worker) && cpu.destroy_thread(worker), "OnStop did not finish the event loop on its own context");

    const uint32_t failed_start = thunk(cpu, 0x100400, 4, [](Cpu& c) {
        const uint32_t object = c.arg32(0);
        c.start_timer(c.read_u32(object + 24), 1);
        c.set_ret32(0);
    });
    cpu.write_u32(table + 12, failed_start);
    require(invoke(cpu, construct, {worker, 1, 65536, 1}) == 0 && invoke(cpu, start, {worker}) == 0 &&
            cpu.pump_threads() && !cpu.timer_active && cpu.join_thread(worker) && cpu.destroy_thread(worker) &&
            cpu.read_u32(owner + 44) == 1, "failed OnStart left a live event loop/timer or called OnStop");

    require(invoke(cpu, construct, {worker, 0, 0, 1}) == 0 && invoke(cpu, start, {worker}) == 0 &&
            cpu.pump_threads() && cpu.read_u32(owner + 40) == 1 && cpu.join_thread(worker) && cpu.destroy_thread(worker),
            "ThreadType worker did not execute its virtual Run");
    invoke(cpu, "_ZN3Osp4Base7Runtime5TimerD1Ev", {timer});
    cpu.free(timer);
    cpu.free(owner);
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
        event_threads(cpu);
        SDL_Quit();
        puts("Worker/event-loop lifecycle, listener adjustment, timer ownership/cancellation, ARM/VFP isolation, Join and semaphores passed");
        return 0;
    } catch (const std::exception& error) {
        fprintf(stderr, "FAIL: %s\n", error.what());
        SDL_Quit();
        return 1;
    }
}
