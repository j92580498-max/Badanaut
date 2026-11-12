#pragma once
#include "elf_loader.h"
#include "hle/dispatch.h" // hle::Handler
#include <cstdint>
#include <deque>
#include <initializer_list>
#include <memory>
#include <string>
#include <unicorn/unicorn.h>
#include <unordered_map>
#include <utility>
#include <vector>

// Wraps a Unicorn ARM core configured for HLE: ELF mapped, trampolines bound,
// VFP enabled, plus host<->guest calling-convention helpers used by HLE handlers.
class Cpu {
  public:
    Cpu();
    ~Cpu();
    Cpu(const Cpu&) = delete;
    Cpu& operator=(const Cpu&) = delete;
    uc_engine* uc = nullptr;
    ElfImage* img = nullptr;             // the app image
    std::vector<ElfImage*> images;       // every mapped image, load order

    // Guest libstdc++, and optional libgcc/libsupc++ exception support, map
    // alongside the app. Imports resolve against these libraries before an
    // HLE trampoline, so the real ARM runtime code handles C++ ABI details.
    bool init(ElfImage& image, std::string& err, ElfImage* lib = nullptr, ElfImage* libgcc = nullptr,
              ElfImage* libsupcxx = nullptr);

    // --- Boot ---
    void run_init_array(); // run DT_INIT_ARRAY constructors
    bool run_guest(uint32_t addr, const std::vector<uint32_t>& args, uint32_t& ret,
                   uint64_t timeout_us = 120'000'000);
    // Enter a guest callback from an HLE handler in the existing emulation
    // slice, then restore the caller and return the callback's result.
    void call_guest_from_handler(uint32_t address, uint32_t object,
                                 std::function<void(Cpu&, uint32_t)> finish = {});
    // Cooperative Bada workers share memory, but retain separate ARM/VFP
    // contexts and stacks. Pump only from the host, outside uc_emu_start.
    bool construct_thread(uint32_t object, uint32_t runnable, uint32_t stack_size);
    bool construct_thread_type(uint32_t object, uint32_t type, uint32_t stack_size);
    bool start_thread(uint32_t object);
    bool stop_thread(uint32_t object);
    bool join_thread(uint32_t object);
    bool destroy_thread(uint32_t object);
    bool pump_threads();
    void sleep_thread(uint32_t milliseconds);
    uint32_t current_thread();
    void exit_thread();
    void create_semaphore(uint32_t object, int count);
    bool acquire_semaphore(uint32_t object, int32_t timeout);
    void release_semaphore(uint32_t object);

    // One-shot timers dispatch through the registered ITimerEventListener,
    // including an event-driven worker's own stack and current-thread identity.
    bool construct_timer(uint32_t object, uint32_t listener);
    bool start_timer(uint32_t object, uint32_t milliseconds);
    void cancel_timer(uint32_t object);
    void destroy_timer(uint32_t object);
    bool has_timers() const;
    bool pump_timers(uint32_t& dispatched);

    // --- Calling-convention accessors (for HLE handlers) ---
    uint32_t arg32(int i); // i-th integer/pointer arg (r0-r3, then stack)
    float argf32(int i);   // i-th VFP single arg (s0..)
    double argf64(int i);  // i-th VFP double arg (d0..)
    void set_ret32(uint32_t v);
    void set_ret64(uint64_t v);
    void set_retf32(float v);
    void set_retf64(double v);

    // --- Guest memory ---
    uint32_t read_u32(uint32_t a);
    void write_u32(uint32_t a, uint32_t v);
    void read_mem(uint32_t a, void* dst, uint32_t n);
    void write_mem(uint32_t a, const void* src, uint32_t n);
    std::string read_cstr(uint32_t a, uint32_t max = 4096);
    // Read-only view of the owned heap, for immediate HLE/GL transfers.
    // Writes still go through Unicorn to retain its code-cache invalidation.
    const uint8_t* memory_ptr(uint32_t a, uint32_t n) const;
    bool memory_mapped(uint32_t address, uint32_t length) const;

    // --- Guest heap (TLSF pool, shared with Unicorn) ---
    uint32_t alloc(uint32_t size, uint32_t align = 8);
    void free(uint32_t address);
    uint32_t allocation_size(uint32_t address) const;
    uint32_t allocated_memory() const;

    // --- Synthetic trampolines / vtables for HLE-built guest objects ---
    // The guest dispatches virtual calls through an object's vtable, so HLE
    // objects need a guest vtable whose slots are trampolines bound to handlers.
    uint32_t bind_trampoline(hle::Handler h); // -> guest addr that dispatches to h
    uint32_t make_vtable(int nslots, std::initializer_list<std::pair<int, hle::Handler>> overrides = {});
    uint32_t make_stub_object(uint32_t size, int nslots = 40); // zeroed obj + no-op vtable
    uint32_t noop_vtable();                                    // shared 64-slot all-no-op vtable for stubbed Osp objects

    // --- Application::Execute handoff (host drives the lifecycle, not nested emu) ---
    uint32_t exec_factory = 0, exec_args = 0;
    bool exec_requested = false;
    bool termination_requested = false;
    void request_exit(); // stop emulation, return control to the host loop

    // --- Application and earliest active timer (host wake/diagnostics) ---
    uint32_t app_instance = 0; // the real app object (factory result)
    bool timer_active = false;
    uint32_t timer_interval_ms = 0; // last Timer::Start(msec); 0 = never armed
    uint32_t timer_deadline_ms = 0; // SDL monotonic clock at Start(), not after Draw()

    uint32_t reg(int ucreg);
    void set_reg(int ucreg, uint32_t v);
    // Nearest defined FUNC at or below addr, across every mapped image.
    std::string symbolize(uint32_t addr, uint32_t* off = nullptr) const;
    void print_backtrace(const char* tag);             // scan guest stack for code return addrs
    void add_observer(uint32_t addr, hle::Handler fn); // log-only hook at a guest addr

    // Conditional patch of an internal binary function: fn runs at the entry; if
    // it returns true the call is short-circuited (set r0, return to LR), else
    // the original body runs normally.
    using PatchFn = bool (*)(Cpu&);
    void patch(uint32_t addr, PatchFn fn);
    void do_return(); // force PC=LR (used by patches)

    bool hard_float = true; // VFP float args (FruitNinja) vs soft-float (GlesCube)
    uint32_t strtok_cursor = 0; // tokenizer state belongs to this guest process
    uint64_t rand_state = 1; // bada/newlib's default seed, independent of the host CRT

  private:
    struct TimerEvent {
        uint32_t object;
        uint64_t generation;
    };
    struct GuestTimer {
        uint32_t listener = 0, owner = 0, interval = 0, deadline = 0;
        uint64_t generation = 0;
        bool active = false;
    };
    std::unordered_map<uint32_t, GuestTimer> timers_;
    uint64_t timer_generation_ = 0;
    void refresh_timer_deadline();
    void cancel_thread_timers(uint32_t object);
    bool timer_callback(const TimerEvent& event, uint32_t& entry, uint32_t& listener);
    bool construct_thread_impl(uint32_t object, uint32_t runnable, uint32_t size, bool event_driven);
    enum class ThreadCall { run, start, timer, stop };
    struct GuestThread {
        uint32_t object = 0, runnable = 0, entry = 0, stack = 0, stack_size = 0;
        uint32_t deadline = 0, semaphore = 0, joining = 0;
        bool started = false, finished = false, suspended = false, timed = false;
        bool event_driven = false, initialized = false, callback_active = false, stopping = false;
        ThreadCall call = ThreadCall::run;
        std::deque<TimerEvent> pending_timers;
        uc_context* context = nullptr;
    };
    std::vector<std::unique_ptr<GuestThread>> threads_;
    uc_context* scheduler_context_ = nullptr;
    GuestThread* running_thread_ = nullptr;
    uint32_t main_thread_ = 0;
    std::unordered_map<uint32_t, int> semaphores_;
    bool run_slice(uint32_t address, uint32_t& result, uint64_t timeout_us);
    struct GuestCall {
        uc_context* context = nullptr;
        uint32_t owner = 0, resume = 0;
        std::function<void(Cpu&, uint32_t)> finish;
        ~GuestCall() { if (context) uc_context_free(context); }
    };
    std::vector<std::unique_ptr<GuestCall>> guest_calls_;
    uint32_t guest_call_return_ = 0;
    void jump_guest(uint32_t address);
    static void finish_guest_call(Cpu& cpu);
    struct Watchdog;
    std::unique_ptr<Watchdog> watchdog_;
    void* heap_storage_ = nullptr;
    void* heap_pool_ = nullptr;
    struct Allocation {
        uint32_t size;
        void* block;
    };
    std::unordered_map<uint32_t, Allocation> allocations_;
    struct VTable {
        int slots;
        std::vector<std::pair<int, hle::Handler>> overrides;
        uint32_t address;
    };
    std::vector<VTable> vtables_;
    // Trampoline slots are keyed by symbol name and shared by all images, so the
    // same import in two images lands on one slot and one HLE handler.
    std::vector<std::string> slots_;
    std::unordered_map<std::string, uint32_t> slot_of_;
    uint32_t synth_base_ = 0;         // first slot index used by synth_
    std::vector<hle::Handler> synth_; // synthetic handlers (slot = synth_base_ + index)
    uint32_t noop_tramp_ = 0;
    uint32_t noop_vtable_ = 0;
    bool native_abi_ = false;
    uint64_t handler_failures_ = 0;
    uint32_t import_slot(const std::string& name); // slot address for an import
    bool map_image(std::string& err);
    void apply_relocs();
    static void hook_code(uc_engine* uc, uint64_t addr, uint32_t size, void* user);
    void dispatch(uint32_t tramp_index);
};
