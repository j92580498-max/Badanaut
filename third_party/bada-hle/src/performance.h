#pragma once
#include <cstdint>

// Opt-in wall-clock profiling. No instruction hooks, allocations or log writes
// occur while recording a sample. GL times include host driver submission and
// stalls; they are not GPU timer-query measurements.
namespace performance {
enum class Work : uint8_t {
    hle, draw, state, texture, shader, present, query, gl_other, canvas,
    audio_hle, audio_mix, soft_float, other,
    driver_draw, driver_state, driver_texture, driver_shader, driver_query, driver_present, driver_other, count
};
struct CallMetrics {
    Work work = Work::other;
    uint64_t calls = 0, nanoseconds = 0;
};

bool enabled();
void start();
void stop();
uint64_t now_ns();
Work classify(const char* name);

class Scope {
  public:
    explicit Scope(Work work, CallMetrics* call = nullptr);
    ~Scope();
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
  private:
    Work work_;
    CallMetrics* call_;
    uint64_t start_;
};

void emulated(uint64_t nanoseconds);
void presented();
uint64_t presentations();
void texture_upload(uint64_t bytes);
void vertex_upload(uint64_t bytes);
void allocation(uint32_t bytes);
void audio_underrun(uint64_t frames);
void math_call(bool accelerated);
void poll(uint32_t heap_bytes = 0); // one summary per second, on emulation thread
} // namespace performance
