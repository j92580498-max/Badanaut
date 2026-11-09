#include "cpu.h"
#include "guest.h"
#include "hle/dispatch.h"
#include "pixel_accel.h"
#include "tlsf.h"
#include <SDL2/SDL.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <mutex>
#include <thread>
#include "compat/elf.h"

#ifndef R_ARM_ABS32
#define R_ARM_ABS32 2
#define R_ARM_GLOB_DAT 21
#define R_ARM_JUMP_SLOT 22
#define R_ARM_RELATIVE 23
#endif

static uint32_t align_down(uint32_t v, uint32_t a) { return v & ~(a - 1); }
static uint32_t align_up(uint32_t v, uint32_t a) { return (v + a - 1) & ~(a - 1); }

namespace {
struct AbiHelper { const char* name; std::array<uint32_t, 8> code; };
// Generated ARMv7-A arithmetic routines for the library boundary. AAPCS
// permits r2 and condition flags to be clobbered; r4-r11 and SP stay intact.
constexpr AbiHelper abi_helpers[] = {
    {"__aeabi_uidiv", {0xe730f110, 0xe12fff1e}}, // udiv r0,r0,r1; bx lr
    {"__aeabi_idiv", {0xe710f110, 0xe12fff1e}},  // sdiv r0,r0,r1; bx lr
    {"__aeabi_uidivmod", {
        0xe3510000, // cmp r1,#0
        0x03a00000, // moveq r0,#0 (zero-divisor result matches the HLE)
        0x012fff1e, // bxeq lr (r1 is already zero)
        0xe1a02000, // mov r2,r0
        0xe730f110, // udiv r0,r0,r1
        0xe0612190, // mls r1,r0,r1,r2 -> remainder = numerator - quotient*divisor
        0xe12fff1e, // bx lr
    }},
    {"__aeabi_idivmod", {
        0xe3510000, 0x03a00000, 0x012fff1e, 0xe1a02000,
        0xe710f110, 0xe0612190, 0xe12fff1e,
    }},
};
} // namespace

// Unicorn's built-in timeout creates a thread for every uc_emu_start and
// polls with usleep(2). Follow SDL's timer-worker model instead: wait until
// the deadline or a cancellation, and join before releasing the core.
// uc_emu_stop is the same cross-thread stop used by Unicorn's own timer.
struct Cpu::Watchdog {
    uc_engine* engine;
    std::mutex mutex;
    std::condition_variable changed;
    bool stopping = false, expired = false;
    unsigned depth = 0;
    uint64_t generation = 0;
    std::chrono::steady_clock::time_point deadline;
    std::thread worker;

    explicit Watchdog(uc_engine* uc) : engine(uc), worker([this] { run(); }) {}
    ~Watchdog() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            stopping = true;
        }
        changed.notify_one();
        worker.join();
    }
    void arm(uint64_t timeout_us) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (!depth++) {
                expired = false;
                ++generation;
                deadline = std::chrono::steady_clock::now() + std::chrono::microseconds(timeout_us);
            }
        }
        changed.notify_one();
    }
    bool disarm() {
        std::lock_guard<std::mutex> lock(mutex);
        --depth;
        changed.notify_one();
        return expired;
    }
    void run() {
        std::unique_lock<std::mutex> lock(mutex);
        while (!stopping) {
            changed.wait(lock, [this] { return stopping || depth; });
            if (stopping) break;
            const uint64_t current = generation;
            if (changed.wait_until(lock, deadline, [this, current] {
                    return stopping || !depth || generation != current;
                })) continue;
            expired = true;
            // A very short deadline can expire before uc_emu_start has entered
            // the JIT. Retry only after expiry so its startup cannot lose the
            // stop request. Normal frames never poll or create timer threads.
            while (!stopping && depth && generation == current) {
                uc_emu_stop(engine);
                changed.wait_for(lock, std::chrono::milliseconds(1), [this, current] {
                    return stopping || !depth || generation != current;
                });
            }
        }
    }
};

Cpu::Cpu() = default;

Cpu::~Cpu() {
    if (scheduler_context_) uc_context_free(scheduler_context_);
    for (auto& thread : threads_) if (thread->context) uc_context_free(thread->context);
    watchdog_.reset();
    // uc_mem_map_ptr borrows the pool: close the core before releasing it.
    if (uc) uc_close(uc);
    if (heap_pool_) tlsf_destroy(heap_pool_);
    std::free(heap_storage_);
}

bool Cpu::init(ElfImage& image, std::string& err, ElfImage* lib) {
    img = &image;
    // The library goes first: its constructors must run before the app's, and
    // its exports win when both images define a name.
    if (lib) {
        images.push_back(lib);
    }
    images.push_back(&image);
    uc_err e = uc_open(UC_ARCH_ARM, UC_MODE_ARM, &uc);
    if (e) {
        err = std::string("uc_open: ") + uc_strerror(e);
        return false;
    }
    try {
        watchdog_ = std::make_unique<Watchdog>(uc);
    } catch (const std::exception& ex) {
        err = std::string("guest watchdog: ") + ex.what();
        return false;
    }

    // Cortex-A15 is Unicorn's default ARM model. Select it explicitly because
    // the runtime helpers below use its integer divide instructions.
    e = uc_ctl_set_cpu_model(uc, UC_CPU_ARM_CORTEX_A15);
    if (e) {
        err = std::string("ARM CPU model: ") + uc_strerror(e);
        return false;
    }

    // Enable VFP (cp10/cp11 full access + FPEXC.EN) so hard-float code runs.
    uint32_t cpacr;
    uc_reg_read(uc, UC_ARM_REG_C1_C0_2, &cpacr);
    cpacr |= (0xf << 20);
    uc_reg_write(uc, UC_ARM_REG_C1_C0_2, &cpacr);
    uint32_t fpexc = 0x40000000;
    uc_reg_write(uc, UC_ARM_REG_FPEXC, &fpexc);

    if (!map_image(err)) {
        return false;
    }

    // Stack + heap.
    uc_mem_map(uc, guest::STACK_TOP - guest::STACK_SIZE, guest::STACK_SIZE, UC_PROT_ALL);
    // Use the upstream TLSF allocator in host-owned, zero-initialized memory.
    // Unicorn and HLE see the same bytes, and free() can reclaim them instead
    // of retaining every temporary mesh/string allocation for the whole run.
    heap_storage_ = std::calloc(1, guest::HEAP_SIZE);
    if (!heap_storage_) {
        err = "guest heap allocation failed";
        return false;
    }
    heap_pool_ = tlsf_create_with_pool(heap_storage_, guest::HEAP_SIZE);
    if (!heap_pool_) {
        err = "guest TLSF pool creation failed";
        return false;
    }
    e = uc_mem_map_ptr(uc, guest::HEAP_BASE, guest::HEAP_SIZE, UC_PROT_ALL, heap_storage_);
    if (e) {
        err = std::string("guest heap mapping: ") + uc_strerror(e);
        return false;
    }
    uint32_t sp = guest::STACK_TOP - 16;
    uc_reg_write(uc, UC_ARM_REG_SP, &sp);

    // HLE trampoline page: fill every slot with `bx lr` (0xe12fff1e).
    uc_mem_map(uc, guest::HLE_BASE, guest::HLE_SIZE, UC_PROT_ALL);
    std::vector<uint32_t> bxlr(guest::HLE_SIZE / 4, 0xe12fff1e);
    uc_mem_write(uc, guest::HLE_BASE, bxlr.data(), guest::HLE_SIZE);
    uc_mem_map(uc, guest::HLE_DATA_BASE, guest::HLE_DATA_SIZE, UC_PROT_ALL);

    const char* no_native = getenv("BADA_NO_NATIVE_ABI");
    native_abi_ = (!no_native || std::strcmp(no_native, "0") == 0) && !getenv("BADA_TRACE");
    if (native_abi_) {
        e = uc_mem_map(uc, guest::ABI_HELPER_BASE, guest::ABI_HELPER_SIZE, UC_PROT_READ | UC_PROT_EXEC);
        if (e) {
            err = std::string("ARM ABI helper mapping: ") + uc_strerror(e);
            return false;
        }
        for (size_t i = 0; i < std::size(abi_helpers); ++i) {
            e = uc_mem_write(uc, guest::ABI_HELPER_BASE + i * guest::ABI_HELPER_STRIDE,
                             abi_helpers[i].code.data(), guest::ABI_HELPER_STRIDE);
            if (e) {
                err = std::string("ARM ABI helper write: ") + uc_strerror(e);
                return false;
            }
        }
        puts("[cpu] integer ABI helpers run in the ARM JIT (no host callbacks)");
    }

    // Dispatch hook over the trampoline page.
    uc_hook h;
    uc_hook_add(uc, &h, UC_HOOK_CODE, (void*)&Cpu::hook_code, this,
                guest::HLE_BASE, guest::HLE_BASE + guest::HLE_SIZE - 1);

    // Float ABI: soft-float binaries import the __aeabi_f* helpers; hard-float don't.
    for (auto& n : img->imports) {
        if (n == "__aeabi_fmul" || n == "__aeabi_fadd") {
            hard_float = false;
            break;
        }
    }
    printf("[cpu] float ABI: %s\n", hard_float ? "hard (VFP)" : "soft");

    apply_relocs();
    hle::init_dispatch();
    pixel_accel::install(*this, image);
    return true;
}

bool Cpu::map_image(std::string& err) {
    for (ElfImage* im : images) {
        uint32_t lo = 0xffffffff, hi = 0;
        for (auto& s : im->segments) {
            lo = std::min(lo, s.vaddr);
            hi = std::max(hi, s.vaddr + s.memsz);
        }
        lo = align_down(lo, 0x1000);
        hi = align_up(hi, 0x1000);
        uc_err e = uc_mem_map(uc, lo, hi - lo, UC_PROT_ALL);
        if (e) {
            err = "map " + im->name + ": " + uc_strerror(e);
            return false;
        }
        for (auto& s : im->segments) {
            uc_mem_write(uc, s.vaddr, s.data, s.filesz); // bss stays zero (fresh map)
        }
        printf("[cpu] %s mapped 0x%x..0x%x (%u KB)\n", im->name.c_str(), lo, hi,
               (hi - lo) / 1024);
    }
    return true;
}

// One trampoline slot per distinct import name, shared by every image.
uint32_t Cpu::import_slot(const std::string& name) {
    if (native_abi_) {
        for (size_t i = 0; i < std::size(abi_helpers); ++i) {
            if (name == abi_helpers[i].name) return guest::ABI_HELPER_BASE + i * guest::ABI_HELPER_STRIDE;
        }
    }
    auto it = slot_of_.find(name);
    if (it != slot_of_.end()) {
        return guest::tramp_addr(it->second);
    }
    uint32_t idx = (uint32_t)slots_.size();
    slots_.push_back(name);
    slot_of_[name] = idx;
    return guest::tramp_addr(idx);
}

// Name an address using whichever image it falls in (app or library).
std::string Cpu::symbolize(uint32_t addr, uint32_t* off) const {
    if (native_abi_ && addr >= guest::ABI_HELPER_BASE &&
        addr < guest::ABI_HELPER_BASE + std::size(abi_helpers) * guest::ABI_HELPER_STRIDE) {
        const uint32_t relative = addr - guest::ABI_HELPER_BASE;
        if (off) *off = relative % guest::ABI_HELPER_STRIDE;
        return abi_helpers[relative / guest::ABI_HELPER_STRIDE].name;
    }
    for (ElfImage* im : images) {
        std::string n = im->nearest_symbol(addr, off);
        if (!n.empty()) {
            return n;
        }
    }
    return std::string();
}

void Cpu::apply_relocs() {
    uint32_t data_next = guest::HLE_DATA_BASE;
    int n_func = 0, n_data = 0, n_rel = 0, n_cross = 0;
    // Data-import stubs are shared by name too: two images importing the same
    // object must see the same storage.
    std::unordered_map<std::string, uint32_t> data_stub;
    for (ElfImage* im : images) {
        for (auto& r : im->relocs) {
            uint32_t value = 0;
            if (r.type == R_ARM_RELATIVE) {
                n_rel++;
                if (im->bias) {
                    write_u32(r.offset, read_u32(r.offset) + im->bias);
                }
                continue; // bias 0: in-place ok
            }
            auto it = im->sym_to_import.find(r.symidx);
            if (it != im->sym_to_import.end()) {
                const std::string& nm = im->imports[it->second];
                // Another image may define what this one imports.
                uint32_t def = 0;
                for (ElfImage* other : images) {
                    if (other != im && (def = other->resolve(nm))) {
                        break;
                    }
                }
                if (def) {
                    value = def;
                    n_cross++;
                } else if (im->import_is_data[it->second]) {
                    auto d = data_stub.find(nm);
                    if (d == data_stub.end()) {
                        d = data_stub.emplace(nm, data_next).first;
                        data_next += 16;
                        n_data++; // zeroed data stub
                    }
                    value = d->second;
                } else {
                    value = import_slot(nm);
                    n_func++; // trampoline
                }
            } else if (r.symidx < im->dynsyms.size()) {
                value = im->dynsyms[r.symidx].value; // defined symbol
            }
            if (r.type == R_ARM_ABS32) {
                // S + A. With no symbol the addend alone is an image-relative
                // address, so it needs the bias.
                value += read_u32(r.offset) + (r.symidx ? 0 : im->bias);
            }
            write_u32(r.offset, value);
        }
    }
    // Freeze the import range: synthetic trampolines start after it.
    synth_base_ = (uint32_t)slots_.size();
    printf("[cpu] relocs: %d func-imports, %d data-imports, %d cross-image, %d relative\n",
           n_func, n_data, n_cross, n_rel);
}

// ---- dispatch ----
void Cpu::hook_code(uc_engine*, uint64_t addr, uint32_t, void* user) {
    auto* self = static_cast<Cpu*>(user);
    uint32_t a = (uint32_t)addr;
    if (a == guest::HLE_RETURN) {
        uc_emu_stop(self->uc);
        return;
    }
    self->dispatch(guest::tramp_index(a));
}

static void synth_noop(Cpu& c) { c.set_ret32(0); }

void Cpu::dispatch(uint32_t idx) {
    if (idx < synth_base_) {
        // A host exception escaping a handler would unwind through Unicorn and
        // kill the process with no context. Name the handler and stop cleanly.
        try {
            hle::call(slots_[idx], *this);
        } catch (const std::exception& ex) {
            printf("[hle] EXCEPTION in %s: %s\n", hle::demangle(slots_[idx]).c_str(), ex.what());
            print_backtrace("bt");
            set_ret32(0);
            uc_emu_stop(uc);
        }
        return;
    }
    uint32_t s = idx - synth_base_; // synthetic vtable/method slot
    if (s < synth_.size() && synth_[s]) {
        synth_[s](*this);
    } else {
        set_ret32(0);
    }
}

uint32_t Cpu::bind_trampoline(hle::Handler h) {
    uint32_t idx = synth_base_ + synth_.size();
    synth_.push_back(h);
    return guest::tramp_addr(idx);
}

uint32_t Cpu::make_vtable(int n, std::initializer_list<std::pair<int, hle::Handler>> ov) {
    // A class shares one virtual table. Recreating it for every resource file
    // otherwise leaks table storage and consumes synthetic trampoline slots.
    const std::vector<std::pair<int, hle::Handler>> overrides(ov);
    for (const auto& table : vtables_)
        if (table.slots == n && table.overrides == overrides) return table.address;
    if (!noop_tramp_) {
        noop_tramp_ = bind_trampoline(synth_noop);
    }
    uint32_t base = alloc((n + 2) * 4, 4);
    write_u32(base, 0);     // offset-to-top
    write_u32(base + 4, 0); // typeinfo (null)
    uint32_t vt = base + 8; // vptr points at first virtual slot
    for (int i = 0; i < n; i++) {
        write_u32(vt + i * 4, noop_tramp_);
    }
    for (auto& p : ov) {
        write_u32(vt + p.first * 4, bind_trampoline(p.second));
    }
    vtables_.push_back({n, overrides, vt});
    return vt;
}

uint32_t Cpu::make_stub_object(uint32_t size, int nslots) {
    uint32_t obj = alloc(size, 8);
    std::vector<uint8_t> z(size, 0);
    write_mem(obj, z.data(), size);
    write_u32(obj, make_vtable(nslots)); // vptr at offset 0
    return obj;
}

uint32_t Cpu::noop_vtable() {
    if (!noop_vtable_) {
        noop_vtable_ = make_vtable(64);
    }
    return noop_vtable_;
}

void Cpu::request_exit() {
    exec_requested = true;
    uc_emu_stop(uc);
}

// ---- boot ----
void Cpu::run_init_array() {
    for (ElfImage* im : images) {
        for (uint32_t fn : im->init_array) {
            uint32_t ret;
            run_guest(fn, {}, ret);
        }
    }
}

bool Cpu::run_guest(uint32_t addr, const std::vector<uint32_t>& args, uint32_t& ret,
                    uint64_t timeout_us) {
    for (size_t i = 0; i < args.size() && i < 4; i++) {
        set_reg(UC_ARM_REG_R0 + i, args[i]);
    }
    uint32_t lr = guest::HLE_RETURN;
    uc_reg_write(uc, UC_ARM_REG_LR, &lr);
    // Unicorn selects ARM vs Thumb from the LSB of the start address passed to
    // uc_emu_start, so keep bit0 intact (set it in CPSR too for good measure).
    uint32_t cpsr;
    uc_reg_read(uc, UC_ARM_REG_CPSR, &cpsr);
    if (addr & 1) {
        cpsr |= (1 << 5);
    } else {
        cpsr &= ~(1 << 5);
    }
    uc_reg_write(uc, UC_ARM_REG_CPSR, &cpsr);
    return run_slice(addr, ret, timeout_us);
}

bool Cpu::run_slice(uint32_t addr, uint32_t& ret, uint64_t timeout_us) {
    // Keep the generous wall-clock guard for asset initialization. Passing
    // zero here disables Unicorn's busy polling timer; our worker owns the
    // deadline and reports expiry as failure instead of accepting partial init.
    if (timeout_us) watchdog_->arm(timeout_us);
    uc_err e = uc_emu_start(uc, addr, guest::HLE_RETURN, 0, 0);
    const bool expired = timeout_us && watchdog_->disarm();
    if (e || expired) {
        uint32_t pc;
        uc_reg_read(uc, UC_ARM_REG_PC, &pc);
        uint32_t poff = 0;
        std::string pn = symbolize(pc, &poff);
        printf("[cpu] emu stopped at pc=0x%x", pc);
        if (!pn.empty()) {
            printf(" (%s+0x%x)", hle::demangle(pn).c_str(), poff);
        }
        printf(": %s\n", expired ? "guest callback timed out" : uc_strerror(e));
        static const int rr[] = {UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2,
                                 UC_ARM_REG_R3, UC_ARM_REG_R4, UC_ARM_REG_R5, UC_ARM_REG_R6,
                                 UC_ARM_REG_R7, UC_ARM_REG_SP, UC_ARM_REG_LR};
        static const char* rn[] = {"r0", "r1", "r2", "r3", "r4", "r5", "r6", "r7", "sp", "lr"};
        printf("[cpu]  ");
        for (int i = 0; i < 10; i++) {
            printf("%s=0x%-9x", rn[i], reg(rr[i]));
        }
        printf("\n");
        print_backtrace("bt");
        return false;
    }
    ret = reg(UC_ARM_REG_R0);
    return true;
}

bool Cpu::construct_thread(uint32_t object, uint32_t runnable, uint32_t size) {
    for (const auto& t : threads_) if (t->object == object) return false;
    if (!object || !runnable || size > 8 * 1024 * 1024) return false;
    auto thread = std::make_unique<GuestThread>();
    thread->object = object;
    thread->runnable = runnable;
    thread->entry = read_u32(read_u32(runnable) + 8); // IRunnable::Run, after D1/D0
    if (!thread->entry) return false;
    thread->stack_size = std::max(size, 64u * 1024);
    thread->stack = alloc(thread->stack_size, 8);
    if (!thread->stack) return false;
    threads_.push_back(std::move(thread));
    return true;
}

bool Cpu::start_thread(uint32_t object) {
    for (auto& thread : threads_) if (thread->object == object && !thread->started) {
        thread->started = true;
        return true;
    }
    return false;
}
bool Cpu::destroy_thread(uint32_t object) {
    for (auto it = threads_.begin(); it != threads_.end(); ++it) {
        const auto& thread = *it;
        if (thread->object != object) continue;
        if (thread->started && !thread->finished) return false;
        if (thread->context) uc_context_free(thread->context);
        free(thread->stack);
        threads_.erase(it);
        for (auto& waiting : threads_) if (waiting->joining == object) waiting->joining = 0;
        return true;
    }
    return false;
}

bool Cpu::join_thread(uint32_t object) {
    for (const auto& thread : threads_) if (thread->object == object) {
        if (!thread->started || thread.get() == running_thread_) return false;
        if (thread->finished) return true;
        if (!running_thread_) return false;
        running_thread_->joining = object;
        running_thread_->suspended = true;
        do_return();
        uc_emu_stop(uc);
        return true;
    }
    return false;
}

uint32_t Cpu::current_thread() {
    if (running_thread_) return running_thread_->object;
    if (!main_thread_) main_thread_ = make_stub_object(32);
    return main_thread_;
}

uint32_t Cpu::allocated_memory() const {
    uint32_t bytes = 0;
    for (const auto& allocation : allocations_) bytes += allocation.second.size;
    return bytes;
}

void Cpu::sleep_thread(uint32_t milliseconds) {
    if (!running_thread_) return;
    running_thread_->deadline = SDL_GetTicks() + milliseconds;
    running_thread_->timed = true;
    running_thread_->suspended = true;
    do_return();
    uc_emu_stop(uc);
}

void Cpu::exit_thread() {
    if (!running_thread_) return;
    running_thread_->finished = true;
    uc_emu_stop(uc);
}

void Cpu::create_semaphore(uint32_t object, int count) { semaphores_[object] = std::max(0, count); }
bool Cpu::acquire_semaphore(uint32_t object, int32_t timeout) {
    auto semaphore = semaphores_.find(object);
    if (semaphore == semaphores_.end()) return false;
    if (semaphore->second > 0) { --semaphore->second; return true; }
    if (!timeout || !running_thread_) return false;
    running_thread_->semaphore = object;
    running_thread_->timed = timeout > 0;
    running_thread_->deadline = SDL_GetTicks() + std::max(0, timeout);
    running_thread_->suspended = true;
    do_return();
    uc_emu_stop(uc);
    return true; // the context resumes only once Release transfers the token
}
void Cpu::release_semaphore(uint32_t object) {
    auto semaphore = semaphores_.find(object);
    if (semaphore == semaphores_.end()) return;
    for (auto& thread : threads_) if (thread->semaphore == object && !thread->finished) {
        thread->semaphore = 0;
        thread->timed = false;
        return;
    }
    ++semaphore->second;
}

bool Cpu::pump_threads() {
    bool active = false;
    for (const auto& thread : threads_) active |= thread->started && !thread->finished;
    if (!active) return true;
    if (!scheduler_context_ && uc_context_alloc(uc, &scheduler_context_) != UC_ERR_OK) return false;
    uc_context* host = scheduler_context_;
    if (uc_context_save(uc, host) != UC_ERR_OK) return false;
    bool success = true;
    // A worker can create workers or destroy completed ones. Snapshot object
    // identities, then look them up before each slice; vector indices and
    // pointers to a future worker can become invalid during a guest call.
    std::vector<uint32_t> objects;
    for (const auto& thread : threads_) objects.push_back(thread->object);
    for (uint32_t object : objects) {
        GuestThread* thread = nullptr;
        for (const auto& candidate : threads_) if (candidate->object == object) { thread = candidate.get(); break; }
        if (!thread) continue;
        if (!thread->started || thread->finished) continue;
        if (thread->joining) {
            bool finished = false;
            for (const auto& joined : threads_) if (joined->object == thread->joining) finished = joined->finished;
            if (!finished) continue;
            thread->joining = 0;
        }
        if (thread->timed && (int32_t)(SDL_GetTicks() - thread->deadline) < 0) continue;
        if (thread->semaphore) {
            if (!thread->timed) continue;
            thread->semaphore = 0;
            if (uc_context_restore(uc, thread->context) != UC_ERR_OK) { success = false; break; }
            set_ret32(0xa0000000u + 3001); // E_TIMEOUT; no token was consumed
            if (uc_context_save(uc, thread->context) != UC_ERR_OK) { success = false; break; }
        }
        thread->timed = false;
        running_thread_ = thread;
        thread->suspended = false;
        uint32_t result = 0;
        bool ok;
        if (thread->context) {
            if (uc_context_restore(uc, thread->context) != UC_ERR_OK) { success = false; break; }
            uint32_t pc = reg(UC_ARM_REG_PC) | ((reg(UC_ARM_REG_CPSR) >> 5) & 1);
            ok = run_slice(pc, result, 120'000'000);
        } else {
            if (uc_context_alloc(uc, &thread->context) != UC_ERR_OK || uc_context_restore(uc, host) != UC_ERR_OK) { success = false; break; }
            set_reg(UC_ARM_REG_SP, thread->stack + thread->stack_size - 16);
            ok = run_guest(thread->entry, {thread->runnable}, result);
        }
        if (thread->suspended && ok) {
            if (uc_context_save(uc, thread->context) != UC_ERR_OK) { success = false; break; }
        }
        else thread->finished = true;
        if (!ok) { success = false; break; }
    }
    running_thread_ = nullptr;
    return uc_context_restore(uc, host) == UC_ERR_OK && success;
}

// ---- ABI accessors ----
static std::vector<std::pair<uint32_t, hle::Handler>> g_observers;
static void observer_hook(uc_engine*, uint64_t addr, uint32_t, void* user) {
    Cpu* self = static_cast<Cpu*>(user);
    for (auto& [a, fn] : g_observers) {
        if (a == (uint32_t)addr) {
            fn(*self);
            return;
        }
    }
}
void Cpu::add_observer(uint32_t addr, hle::Handler fn) {
    uint32_t a = addr & ~1u;
    g_observers.push_back({a, fn});
    uc_hook h;
    uc_hook_add(uc, &h, UC_HOOK_CODE, (void*)&observer_hook, this, a, a);
    // Drop any cached translation of that address: a hook added after the block
    // was already translated is otherwise silently ignored (matters for hooks
    // armed at runtime, e.g. the `args` debug command).
    uc_ctl_remove_cache(uc, a, a + 4);
}

static std::vector<std::pair<uint32_t, Cpu::PatchFn>> g_patches;
void Cpu::do_return() {
    uint32_t lr = reg(UC_ARM_REG_LR);
    uint32_t cpsr;
    uc_reg_read(uc, UC_ARM_REG_CPSR, &cpsr);
    if (lr & 1) {
        cpsr |= (1 << 5);
    } else {
        cpsr &= ~(1 << 5);
    }
    uc_reg_write(uc, UC_ARM_REG_CPSR, &cpsr);
    // Unicorn uses the written PC's low bit to select Thumb mode. Clearing it
    // here would undo CPSR.T and decode a Thumb caller's return as ARM code.
    uint32_t pc = lr;
    uc_reg_write(uc, UC_ARM_REG_PC, &pc);
}
static void patch_hook(uc_engine*, uint64_t addr, uint32_t, void* user) {
    Cpu* self = static_cast<Cpu*>(user);
    for (auto& [a, fn] : g_patches) {
        if (a == (uint32_t)addr) {
            if (fn(*self)) {
                self->do_return();
            }
            return;
        }
    }
}
void Cpu::patch(uint32_t addr, PatchFn fn) {
    uint32_t a = addr & ~1u;
    g_patches.push_back({a, fn});
    uc_hook h;
    uc_hook_add(uc, &h, UC_HOOK_CODE, (void*)&patch_hook, this, a, a);
    uc_ctl_remove_cache(uc, a, a + 4); // see add_observer
}

void Cpu::print_backtrace(const char* tag) {
    uint32_t sp = reg(UC_ARM_REG_SP);
    printf("[cpu]  %s:\n", tag);
    // Scan the stack for words that land inside a known FUNC (likely return
    // addresses) and name them. Generic across binaries (no hardcoded range).
    for (uint32_t a = sp, n = 0; a < guest::STACK_TOP && n < 12; a += 4) {
        uint32_t t = read_u32(a) & ~1u;
        uint32_t off = 0;
        std::string nm = symbolize(t, &off);
        if (!nm.empty() && off < 0x8000) {
            printf("[cpu]    0x%-8x %s+0x%x\n", t, hle::demangle(nm).c_str(), off);
            n++;
        }
    }
}

uint32_t Cpu::reg(int r) {
    uint32_t v;
    uc_reg_read(uc, r, &v);
    return v;
}
void Cpu::set_reg(int r, uint32_t v) { uc_reg_write(uc, r, &v); }

uint32_t Cpu::arg32(int i) {
    if (i < 4) {
        return reg(UC_ARM_REG_R0 + i);
    }
    return read_u32(reg(UC_ARM_REG_SP) + (i - 4) * 4);
}
// Float accessors honour the binary's float ABI. Hard-float: VFP S/D regs.
// Soft-float: floats ride in the core-register arg sequence as bit patterns.
float Cpu::argf32(int i) {
    if (hard_float) {
        float v;
        uc_reg_read(uc, UC_ARM_REG_S0 + i, &v);
        return v;
    }
    uint32_t b = arg32(i);
    float f;
    memcpy(&f, &b, 4);
    return f;
}
double Cpu::argf64(int i) {
    if (hard_float) {
        double v;
        uc_reg_read(uc, UC_ARM_REG_D0 + i, &v);
        return v;
    }
    uint64_t b = (uint64_t)arg32(2 * i) | ((uint64_t)arg32(2 * i + 1) << 32);
    double d;
    memcpy(&d, &b, 8);
    return d;
}
void Cpu::set_ret32(uint32_t v) { set_reg(UC_ARM_REG_R0, v); }
void Cpu::set_ret64(uint64_t v) {
    uint32_t lo = v, hi = v >> 32;
    set_reg(UC_ARM_REG_R0, lo);
    set_reg(UC_ARM_REG_R1, hi);
}
void Cpu::set_retf32(float v) {
    if (hard_float) {
        uc_reg_write(uc, UC_ARM_REG_S0, &v);
        return;
    }
    uint32_t b;
    memcpy(&b, &v, 4);
    set_ret32(b);
}
void Cpu::set_retf64(double v) {
    if (hard_float) {
        uc_reg_write(uc, UC_ARM_REG_D0, &v);
        return;
    }
    uint64_t b;
    memcpy(&b, &v, 8);
    set_ret64(b);
}

// ---- guest memory ----
uint32_t Cpu::read_u32(uint32_t a) {
    uint32_t v = 0;
    uc_mem_read(uc, a, &v, 4);
    return v;
}
void Cpu::write_u32(uint32_t a, uint32_t v) { uc_mem_write(uc, a, &v, 4); }
const uint8_t* Cpu::memory_ptr(uint32_t a, uint32_t n) const {
    if (!heap_storage_ || a < guest::HEAP_BASE ||
        static_cast<uint64_t>(a) + n > static_cast<uint64_t>(guest::HEAP_BASE) + guest::HEAP_SIZE)
        return nullptr;
    return static_cast<const uint8_t*>(heap_storage_) + (a - guest::HEAP_BASE);
}
void Cpu::read_mem(uint32_t a, void* d, uint32_t n) {
    if (!n) return;
    if (const auto* p = memory_ptr(a, n)) std::memcpy(d, p, n);
    else uc_mem_read(uc, a, d, n);
}
void Cpu::write_mem(uint32_t a, const void* s, uint32_t n) { uc_mem_write(uc, a, s, n); }
std::string Cpu::read_cstr(uint32_t a, uint32_t max) {
    if (const auto* p = memory_ptr(a, max)) {
        const auto* end = static_cast<const uint8_t*>(std::memchr(p, 0, max));
        return std::string(reinterpret_cast<const char*>(p), end ? end - p : max);
    }
    std::string s;
    for (uint32_t i = 0; i < max; i++) {
        uint8_t c = 0;
        if (uc_mem_read(uc, a + i, &c, 1) != UC_ERR_OK) break;
        if (!c) {
            break;
        }
        s.push_back((char)c);
    }
    return s;
}
uint32_t Cpu::alloc(uint32_t size, uint32_t align) {
    if (!heap_pool_ || !align || (align & (align - 1)) ||
        align > guest::HEAP_SIZE || size >= guest::HEAP_SIZE) return 0;
    // TLSF aligns host addresses. A calloc-backed pool may have a different
    // alignment from HEAP_BASE, so preserve the original TLSF block and align
    // the guest address inside it when those address spaces differ.
    const bool matching_alignment =
        (reinterpret_cast<uintptr_t>(heap_storage_) & (align - 1)) ==
        (guest::HEAP_BASE & (align - 1));
    const uint64_t requested = static_cast<uint64_t>(std::max(size, uint32_t(1))) +
                              (matching_alignment ? 0 : align - 1);
    if (requested >= guest::HEAP_SIZE) return 0;
    void* block = tlsf_memalign(heap_pool_, matching_alignment ?
        std::max<size_t>(align, tlsf_align_size()) : tlsf_align_size(),
        static_cast<size_t>(requested));
    if (!block) return 0;
    uint32_t p = guest::HEAP_BASE + static_cast<uint32_t>(
        static_cast<uint8_t*>(block) - static_cast<uint8_t*>(heap_storage_));
    if (!matching_alignment) p = (p + align - 1) & ~(align - 1);
    allocations_[p] = {size, block};
    // The previous zero-backed heap always supplied cleared storage. Keep
    // that behavior when TLSF reuses a block, as touchHLE's Mem::alloc does.
    // Fruit Ninja leaves optional HUD fields at their initial zero values;
    // stale payload bytes made its first frame bind a null coin texture.
    // Write through Unicorn so reused executable storage stays coherent.
    static const std::array<uint8_t, 64 * 1024> zero{};
    for (uint32_t offset = 0; offset < size;) {
        const uint32_t bytes = std::min<uint32_t>(zero.size(), size - offset);
        write_mem(p + offset, zero.data(), bytes);
        offset += bytes;
    }
    return p;
}

void Cpu::free(uint32_t address) {
    const auto allocation = allocations_.find(address);
    if (allocation == allocations_.end()) return; // includes free(nullptr)
    tlsf_free(heap_pool_, allocation->second.block);
    allocations_.erase(allocation);
}

uint32_t Cpu::allocation_size(uint32_t address) const {
    const auto allocation = allocations_.find(address);
    return allocation == allocations_.end() ? 0 : allocation->second.size;
}
