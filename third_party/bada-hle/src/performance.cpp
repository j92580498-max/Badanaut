#include "performance.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#ifdef __ANDROID__
#include <SDL2/SDL.h>
#include <jni.h>
#include <cstdlib>
#endif

namespace performance {
namespace {
constexpr size_t work_count = static_cast<size_t>(Work::count);
struct Counter { std::atomic<uint64_t> ns{0}, calls{0}; };
std::array<Counter, work_count> counters;
std::atomic<bool> active{false};
std::atomic<uint64_t> emulation_ns{0}, frame_count{0}, texture_bytes{0}, vertex_bytes{0};
std::atomic<uint64_t> allocations{0}, allocated_bytes{0}, underrun_frames{0};
std::atomic<uint64_t> math_calls{0}, math_fallbacks{0};
struct Totals {
    std::array<uint64_t, work_count> ns{}, calls{};
    uint64_t emulation = 0, frames = 0, textures = 0, vertices = 0;
    uint64_t allocs = 0, bytes = 0, underruns = 0;
    uint64_t math = 0, math_fallback = 0;
};
Totals previous;
uint64_t epoch = 0, window_start = 0, last_frame = 0;
// Frame-rate samples are written/polled by the emulation thread. The audio
// callback touches only atomic counters. The fixed buffer covers 4096 Hz.
std::array<uint64_t, 4096> intervals{};
size_t interval_count = 0;
uint64_t interval_sum = 0, interval_max = 0, interval_total = 0;
FILE* frame_log = nullptr;

void flush_frame_log() {
    if (!frame_log) return;
    for (size_t i = 0; i < interval_count; ++i)
        std::fprintf(frame_log, "%.6f\n", intervals[i] / 1'000'000.0);
    std::fflush(frame_log);
}

Totals totals() {
    Totals out;
    for (size_t i = 0; i < work_count; ++i) {
        out.ns[i] = counters[i].ns.load(std::memory_order_relaxed);
        out.calls[i] = counters[i].calls.load(std::memory_order_relaxed);
    }
    out.emulation = emulation_ns.load(std::memory_order_relaxed);
    out.frames = frame_count.load(std::memory_order_relaxed);
    out.textures = texture_bytes.load(std::memory_order_relaxed);
    out.vertices = vertex_bytes.load(std::memory_order_relaxed);
    out.allocs = allocations.load(std::memory_order_relaxed);
    out.bytes = allocated_bytes.load(std::memory_order_relaxed);
    out.underruns = underrun_frames.load(std::memory_order_relaxed);
    out.math = math_calls.load(std::memory_order_relaxed);
    out.math_fallback = math_fallbacks.load(std::memory_order_relaxed);
    return out;
}

#ifdef __ANDROID__
// ART exposes cumulative GC work and time spent waiting for blocking GC. Keep
// both: concurrent collector work must not be mislabeled as a frame pause.
bool gc_times(uint64_t& work, uint64_t& blocking) {
    auto* env = static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv());
    if (!env) return false;
    static jclass debug = nullptr;
    static jmethodID get_stat = nullptr;
    static jstring work_key = nullptr, blocking_key = nullptr;
    if (!debug) {
        jclass local = env->FindClass("android/os/Debug");
        if (local) {
            debug = static_cast<jclass>(env->NewGlobalRef(local));
            env->DeleteLocalRef(local);
            get_stat = env->GetStaticMethodID(debug, "getRuntimeStat", "(Ljava/lang/String;)Ljava/lang/String;");
            jstring key = env->NewStringUTF("art.gc.gc-time");
            work_key = static_cast<jstring>(env->NewGlobalRef(key));
            env->DeleteLocalRef(key);
            key = env->NewStringUTF("art.gc.blocking-gc-time");
            blocking_key = static_cast<jstring>(env->NewGlobalRef(key));
            env->DeleteLocalRef(key);
        }
    }
    if (env->ExceptionCheck()) { env->ExceptionClear(); return false; }
    if (!debug || !get_stat) return false;
    const auto read = [&](jstring key, uint64_t& result) {
        auto value = static_cast<jstring>(env->CallStaticObjectMethod(debug, get_stat, key));
        if (env->ExceptionCheck()) { env->ExceptionClear(); return false; }
        if (!value) return false;
        const char* chars = env->GetStringUTFChars(value, nullptr);
        if (!chars) { env->DeleteLocalRef(value); return false; }
        result = std::strtoull(chars, nullptr, 10);
        env->ReleaseStringUTFChars(value, chars);
        env->DeleteLocalRef(value);
        return true;
    };
    return read(work_key, work) && read(blocking_key, blocking);
}
uint64_t previous_gc = 0, previous_blocking_gc = 0;
#endif
} // namespace

bool enabled() { return active.load(std::memory_order_relaxed); }
uint64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
void start() {
    if (frame_log) { flush_frame_log(); std::fclose(frame_log); frame_log = nullptr; }
    if (const char* path = std::getenv("BADA_PROFILE_FRAMES")) {
        frame_log = std::fopen(path, "w");
        if (frame_log) std::fputs("frame_ms\n", frame_log);
        else std::printf("[perf] cannot write frame samples: %s\n", path);
    }
    previous = totals();
    epoch = window_start = now_ns();
    last_frame = 0;
    interval_count = 0;
    interval_sum = interval_max = interval_total = 0;
#ifdef __ANDROID__
    gc_times(previous_gc, previous_blocking_gc);
#endif
    active.store(true, std::memory_order_relaxed);
    puts("[perf] enabled: presentation FPS; CPU excludes HLE; render is CPU/driver time; milliseconds per presented frame; bytes/calls per window");
}
void stop() {
    active.store(false, std::memory_order_relaxed);
    if (frame_log) { flush_frame_log(); std::fclose(frame_log); frame_log = nullptr; }
}

Work classify(const char* name) {
    if (!std::strncmp(name, "__aeabi_f", 9) || !std::strncmp(name, "__aeabi_d", 9)) return Work::soft_float;
    if (std::strstr(name, "AudioOut")) return Work::audio_hle;
    if (!std::strcmp(name, "sglSwapBuffers")) return Work::present;
    if (!std::strncmp(name, "gl", 2)) {
        if (!std::strncmp(name, "glDraw", 6)) return Work::draw;
        if (std::strstr(name, "TexImage") || std::strstr(name, "TexSubImage") || std::strstr(name, "BufferData") ||
            std::strstr(name, "BufferSubData")) return Work::texture;
        if (std::strstr(name, "CompileShader") || std::strstr(name, "LinkProgram") ||
            std::strstr(name, "ShaderSource") || std::strstr(name, "ShaderBinary")) return Work::shader;
        if (!std::strncmp(name, "glGet", 5) || !std::strncmp(name, "glIs", 4) ||
            !std::strncmp(name, "glRead", 6) || !std::strncmp(name, "glFinish", 8)) return Work::query;
        return Work::state;
    }
    if (!std::strncmp(name, "sgl", 3)) return Work::gl_other;
    return Work::other;
}

Scope::Scope(Work work, CallMetrics* call) : work_(work), call_(call), start_(enabled() ? now_ns() : 0) {}
Scope::~Scope() {
    if (!start_) return;
    const uint64_t ns = now_ns() - start_;
    auto& counter = counters[static_cast<size_t>(work_)];
    counter.ns.fetch_add(ns, std::memory_order_relaxed);
    counter.calls.fetch_add(1, std::memory_order_relaxed);
    if (call_) { ++call_->calls; call_->nanoseconds += ns; }
}
void emulated(uint64_t ns) { emulation_ns.fetch_add(ns, std::memory_order_relaxed); }
uint64_t presentations() { return frame_count.load(std::memory_order_relaxed); }
void presented() {
    frame_count.fetch_add(1, std::memory_order_relaxed);
    if (!enabled()) return;
    const uint64_t time = now_ns();
    if (last_frame) {
        const uint64_t ns = time - last_frame;
        if (interval_count < intervals.size()) intervals[interval_count++] = ns;
        ++interval_total;
        interval_sum += ns;
        interval_max = std::max(interval_max, ns);
    }
    last_frame = time;
}
void texture_upload(uint64_t bytes) { if (enabled()) texture_bytes.fetch_add(bytes, std::memory_order_relaxed); }
void vertex_upload(uint64_t bytes) { if (enabled()) vertex_bytes.fetch_add(bytes, std::memory_order_relaxed); }
void allocation(uint32_t bytes) {
    if (!enabled()) return;
    allocations.fetch_add(1, std::memory_order_relaxed);
    allocated_bytes.fetch_add(bytes, std::memory_order_relaxed);
}
void audio_underrun(uint64_t frames) { if (enabled()) underrun_frames.fetch_add(frames, std::memory_order_relaxed); }
void math_call(bool accelerated) {
    if (!enabled()) return;
    math_calls.fetch_add(1, std::memory_order_relaxed);
    if (!accelerated) math_fallbacks.fetch_add(1, std::memory_order_relaxed);
}

void poll(uint32_t heap_bytes) {
    if (!enabled()) return;
    const uint64_t time = now_ns(), elapsed = time - window_start;
    if (elapsed < 1'000'000'000) return;
    const Totals current = totals();
    const uint64_t frames = current.frames - previous.frames;
    const double divisor = 1'000'000.0 * (frames ? frames : 1);
    const auto ns = [&](Work work) { const auto i = static_cast<size_t>(work); return current.ns[i] - previous.ns[i]; };
    const auto ms = [&](Work work) { return ns(work) / divisor; };
    const auto calls = [&](Work work) { const auto i = static_cast<size_t>(work); return current.calls[i] - previous.calls[i]; };
    const double gl_cpu = ms(Work::draw) + ms(Work::state) + ms(Work::texture) + ms(Work::shader) +
        ms(Work::present) + ms(Work::query) + ms(Work::gl_other) + ms(Work::canvas);
    const double driver = ms(Work::driver_draw) + ms(Work::driver_state) + ms(Work::driver_texture) +
        ms(Work::driver_shader) + ms(Work::driver_query) + ms(Work::driver_present) + ms(Work::driver_other);
    const double render = driver > 0 ? driver : gl_cpu;
    const uint64_t emu = current.emulation - previous.emulation, hle = ns(Work::hle);
    flush_frame_log();
    std::sort(intervals.begin(), intervals.begin() + interval_count);
    const auto percentile = [&](double p) {
        return interval_count ? intervals[static_cast<size_t>(std::ceil(p * interval_count)) - 1] / 1'000'000.0 : 0.0;
    };
    char gc[128];
#ifdef __ANDROID__
    uint64_t work = 0, blocking = 0;
    if (gc_times(work, blocking)) {
        std::snprintf(gc, sizeof gc, "gc_ms=%llu gc_block_ms=%llu", static_cast<unsigned long long>(work - previous_gc),
            static_cast<unsigned long long>(blocking - previous_blocking_gc));
        previous_gc = work; previous_blocking_gc = blocking;
    } else std::snprintf(gc, sizeof gc, "gc_ms=unavailable gc_block_ms=unavailable");
#else
    std::snprintf(gc, sizeof gc, "gc_ms=na gc_block_ms=na");
#endif
    std::printf("[perf] t=%.3f window_ms=%.3f frames=%llu fps=%.3f frame_ms=%.3f p50_ms=%.3f p95_ms=%.3f p99_ms=%.3f max_ms=%.3f "
        "cpu_ms=%.3f hle_ms=%.3f render_ms=%.3f draw_ms=%.3f state_ms=%.3f upload_ms=%.3f shader_ms=%.3f present_ms=%.3f query_ms=%.3f canvas_ms=%.3f "
        "audio_ms=%.3f audio_mix_ms=%.3f soft_float_ms=%.3f draw_calls=%llu state_calls=%llu upload_calls=%llu shader_calls=%llu "
        "texture_bytes=%llu vertex_bytes=%llu allocs=%llu alloc_bytes=%llu heap_bytes=%u underrun_frames=%llu "
        "gl_cpu_ms=%.3f driver_ms=%.3f driver_draw_ms=%.3f driver_state_ms=%.3f driver_upload_ms=%.3f "
        "driver_shader_ms=%.3f driver_query_ms=%.3f driver_present_ms=%.3f math_calls=%llu math_fallbacks=%llu %s\n",
        (time - epoch) / 1'000'000'000.0, elapsed / 1'000'000.0, static_cast<unsigned long long>(frames), frames * 1'000'000'000.0 / elapsed,
        interval_total ? interval_sum / (1'000'000.0 * interval_total) : 0.0, percentile(.50), percentile(.95), percentile(.99), interval_max / 1'000'000.0,
        (emu >= hle ? emu - hle : 0) / divisor, hle / divisor, render, ms(Work::draw), ms(Work::state), ms(Work::texture), ms(Work::shader),
        ms(Work::present), ms(Work::query), ms(Work::canvas), ms(Work::audio_hle) + ms(Work::audio_mix), ms(Work::audio_mix), ms(Work::soft_float),
        static_cast<unsigned long long>(calls(Work::draw)), static_cast<unsigned long long>(calls(Work::state)),
        static_cast<unsigned long long>(calls(Work::texture)), static_cast<unsigned long long>(calls(Work::shader)),
        static_cast<unsigned long long>(current.textures - previous.textures), static_cast<unsigned long long>(current.vertices - previous.vertices),
        static_cast<unsigned long long>(current.allocs - previous.allocs), static_cast<unsigned long long>(current.bytes - previous.bytes), heap_bytes,
        static_cast<unsigned long long>(current.underruns - previous.underruns), gl_cpu, driver,
        ms(Work::driver_draw), ms(Work::driver_state), ms(Work::driver_texture), ms(Work::driver_shader),
        ms(Work::driver_query), ms(Work::driver_present),
        static_cast<unsigned long long>(current.math - previous.math),
        static_cast<unsigned long long>(current.math_fallback - previous.math_fallback), gc);
    previous = current;
    window_start = time;
    interval_count = 0;
    interval_sum = interval_max = interval_total = 0;
}
} // namespace performance
