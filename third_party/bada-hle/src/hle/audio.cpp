#include "cpu.h"
#include "dispatch.h"
#include "performance.h"
#include "audio_ring.h"
#include <SDL2/SDL.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <map>
#include <vector>

namespace {
constexpr uint32_t unsupported_format = 0x80000000;
constexpr uint32_t invalid_state = 0x80000000;
constexpr int sample_s16 = 5; // bada AUDIO_SAMPLE_TYPE_16BIT
constexpr int mono = 1, stereo = 2;

struct AudioOut {
    uint32_t listener = 0;
    uint64_t generation = 0;
    bool prepared = false, playing = false;
    int rate = 44100, channels = mono, volume = 100;
    uint32_t phase = 0, completed = 0;
    PcmRing ring;
};

// Android's SDL backend has one playback device. Mix AudioOut instances into
// that device, and keep all guest calls on the emulation thread.
std::map<uint32_t, AudioOut> outputs;
SDL_AudioDeviceID device = 0;
SDL_AudioSpec device_spec{};
bool suspended = false;
uint64_t next_generation = 0;

struct AudioLock {
    AudioLock() { if (device) SDL_LockAudioDevice(device); }
    ~AudioLock() { if (device) SDL_UnlockAudioDevice(device); }
};

void reset(AudioOut& out) {
    out.ring.clear();
    out.phase = out.completed = 0;
    out.generation = ++next_generation;
}

int sample(const AudioOut& out, int channel, bool next) {
    return out.ring.sample(channel, next);
}

void mix(void*, Uint8* bytes, int length) {
    performance::Scope sample_time(performance::Work::audio_mix);
    uint64_t missing_frames = 0;
    SDL_memset(bytes, 0, length);
    if (suspended) return;
    auto* destination = reinterpret_cast<int16_t*>(bytes);
    const int frames = length / (stereo * sizeof(int16_t));
    for (int frame = 0; frame < frames; ++frame) {
        int left = 0, right = 0;
        for (auto& entry : outputs) {
            auto& out = entry.second;
            if (!out.playing) continue;
            if (out.ring.empty()) { ++missing_frames; continue; }
            int values[2];
            for (int channel = 0; channel < stereo; ++channel) {
                const int current = sample(out, channel, false);
                const int following = sample(out, channel, true);
                values[channel] = (current + static_cast<int>(
                    static_cast<int64_t>(following - current) * out.phase / device_spec.freq))
                    * out.volume / 100;
            }
            left += values[0];
            right += values[1];
            out.phase += out.rate;
            while (out.phase >= static_cast<uint32_t>(device_spec.freq) && !out.ring.empty()) {
                out.phase -= device_spec.freq;
                out.completed += out.ring.consume();
            }
            if (out.ring.empty()) out.phase = 0;
        }
        destination[frame * stereo] = static_cast<int16_t>(std::clamp(left, -32768, 32767));
        destination[frame * stereo + 1] = static_cast<int16_t>(std::clamp(right, -32768, 32767));
    }
    performance::audio_underrun(missing_frames);
}

bool ensure_device() {
    if (device) return true;
#ifdef __ANDROID__
    if (!SDL_getenv("SDL_AUDIODRIVER")) SDL_SetHint(SDL_HINT_AUDIODRIVER, "aaudio,openslES,android");
#endif
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) return false;
    SDL_AudioSpec desired{};
    desired.freq = 48000;
    desired.format = AUDIO_S16SYS;
    desired.channels = stereo;
    desired.samples = 512;
    desired.callback = mix;
    device = SDL_OpenAudioDevice(nullptr, 0, &desired, &device_spec, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
    if (!device) {
        printf("[audio] output unavailable: %s\n", SDL_GetError());
        return false;
    }
    printf("[audio] %s: %d Hz, stereo S16\n", SDL_GetCurrentAudioDriver(), device_spec.freq);
    SDL_PauseAudioDevice(device, 0);
    return true;
}

void audio_ctor(Cpu& cpu) {
    const uint32_t object = cpu.arg32(0);
    cpu.write_u32(object, cpu.noop_vtable());
    AudioLock lock;
    auto& out = outputs[object];
    out = AudioOut{};
    out.generation = ++next_generation;
    cpu.set_ret32(object);
}

void audio_dtor(Cpu& cpu) {
    AudioLock lock;
    outputs.erase(cpu.arg32(0));
    cpu.set_ret32(0);
}

void audio_construct(Cpu& cpu) {
    AudioLock lock;
    outputs[cpu.arg32(0)].listener = cpu.arg32(1);
    cpu.set_ret32(0);
}

void audio_prepare(Cpu& cpu) {
    const int type = cpu.arg32(1), channels = cpu.arg32(2), rate = cpu.arg32(3);
    if (type != sample_s16 || (channels != mono && channels != stereo) || rate < 1000 || rate > 192000) {
        cpu.set_ret32(unsupported_format);
        return;
    }
    if (!ensure_device()) {
        cpu.set_ret32(invalid_state);
        return;
    }
    AudioLock lock;
    auto& out = outputs[cpu.arg32(0)];
    reset(out);
    out.prepared = true;
    out.playing = false;
    out.rate = rate;
    out.channels = channels;
    out.ring.prepare(rate, channels); // retain the existing one-second queue bound
    cpu.set_ret32(0);
}

void audio_write(Cpu& cpu) {
    uint32_t data = 0, length = 0;
    if (!hle::byte_buffer_view(cpu.arg32(1), data, length) || !data || !length || length > 4 * 1024 * 1024) {
        cpu.set_ret32(invalid_state);
        return;
    }
    // The game can overwrite or destroy the ByteBuffer as soon as this returns.
    AudioLock lock;
    auto& out = outputs[cpu.arg32(0)];
    const size_t frames = length / (out.channels * sizeof(int16_t));
    if (!out.prepared || length % (out.channels * sizeof(int16_t)) || !frames ||
        out.ring.size() + frames > out.ring.capacity()) {
        cpu.set_ret32(invalid_state);
        return;
    }
    out.ring.append(frames, [&](int16_t* destination, size_t offset, size_t samples) {
        const uint32_t address = data + offset * sizeof(int16_t);
        const uint32_t bytes = samples * sizeof(int16_t);
        if (const auto* source = cpu.memory_ptr(address, bytes)) std::memcpy(destination, source, bytes);
        else cpu.read_mem(address, destination, bytes);
    });
    cpu.set_ret32(0);
}

void audio_start(Cpu& cpu) {
    AudioLock lock;
    auto& out = outputs[cpu.arg32(0)];
    out.playing = out.prepared;
    cpu.set_ret32(out.prepared ? 0 : invalid_state);
}

void audio_stop(Cpu& cpu) {
    AudioLock lock;
    auto& out = outputs[cpu.arg32(0)];
    out.playing = false;
    reset(out);
    cpu.set_ret32(0);
}

void audio_unprepare(Cpu& cpu) {
    AudioLock lock;
    auto& out = outputs[cpu.arg32(0)];
    out.playing = out.prepared = false;
    reset(out);
    cpu.set_ret32(0);
}

void audio_volume(Cpu& cpu) {
    AudioLock lock;
    outputs[cpu.arg32(0)].volume = std::clamp(static_cast<int>(cpu.arg32(1)), 0, 100);
    cpu.set_ret32(0);
}

void optimized_sample(Cpu& cpu) { cpu.set_ret32(sample_s16); }
void optimized_channels(Cpu& cpu) { cpu.set_ret32(mono); }
void optimized_rate(Cpu& cpu) { cpu.set_ret32(44100); }
void minimum_buffer(Cpu& cpu) { cpu.set_ret32(2048); }
} // namespace

namespace hle {
void register_audio() {
    register_handler("_ZN3Osp5Media8AudioOutC1Ev", audio_ctor);
    register_handler("_ZN3Osp5Media8AudioOutC2Ev", audio_ctor);
    register_handler("_ZN3Osp5Media8AudioOutD1Ev", audio_dtor);
    register_handler("_ZN3Osp5Media8AudioOutD2Ev", audio_dtor);
    register_handler("_ZN3Osp5Media8AudioOut9ConstructERKNS0_22IAudioOutEventListenerE", audio_construct);
    register_handler("_ZN3Osp5Media8AudioOut7PrepareENS0_15AudioSampleTypeENS0_16AudioChannelTypeEi", audio_prepare);
    register_handler("_ZN3Osp5Media8AudioOut11WriteBufferERKNS_4Base10ByteBufferE", audio_write);
    register_handler("_ZN3Osp5Media8AudioOut5StartEv", audio_start);
    register_handler("_ZN3Osp5Media8AudioOut4StopEv", audio_stop);
    register_handler("_ZN3Osp5Media8AudioOut9UnprepareEv", audio_unprepare);
    register_handler("_ZN3Osp5Media8AudioOut9SetVolumeEi", audio_volume);
    register_handler("_ZNK3Osp5Media8AudioOut22GetOptimizedSampleTypeEv", optimized_sample);
    register_handler("_ZNK3Osp5Media8AudioOut23GetOptimizedChannelTypeEv", optimized_channels);
    register_handler("_ZNK3Osp5Media8AudioOut22GetOptimizedSampleRateEv", optimized_rate);
    register_handler("_ZNK3Osp5Media8AudioOut16GetMinBufferSizeEv", minimum_buffer);
}

bool audio_needs_service() {
    AudioLock lock;
    for (const auto& entry : outputs) if (entry.second.playing && !suspended) return true;
    return false;
}
void audio_pump(Cpu& cpu) {
    // Buffers are refilled by guest callbacks, always on the emulation thread.
    struct Completion { uint32_t object, listener, count; uint64_t generation; };
    static std::vector<Completion> pending;
    pending.clear();
    {
        AudioLock lock;
        if (pending.capacity() < outputs.size()) pending.reserve(outputs.size());
        for (auto& entry : outputs) {
            auto& out = entry.second;
            const uint32_t count = std::min(out.completed, 8u);
            if (!count) continue;
            out.completed -= count;
            pending.push_back({entry.first, out.listener, count, out.generation});
        }
    }
    for (const auto& event : pending) {
        for (uint32_t n = 0; n < event.count; ++n) {
            {
                AudioLock lock;
                const auto found = outputs.find(event.object);
                if (found == outputs.end() || found->second.generation != event.generation || !found->second.playing) break;
            }
            const uint32_t vtable = event.listener ? cpu.read_u32(event.listener) : 0;
            // Two destructor entries precede OnAudioOutBufferEndReached. Keep
            // the function's Thumb bit and the listener's multiple-inheritance
            // adjustment; its vtable thunk handles both.
            const uint32_t callback = vtable ? cpu.read_u32(vtable + 2 * 4) : 0;
            if (!callback) break;
            uint32_t result = 0;
            if (!cpu.run_guest(callback, {event.listener, event.object}, result)) {
                AudioLock lock;
                auto found = outputs.find(event.object);
                if (found != outputs.end()) {
                    found->second.playing = false;
                    reset(found->second);
                }
                break;
            }
        }
    }
}

void audio_pause(bool pause) {
    AudioLock lock;
    suspended = pause;
}

void audio_shutdown() {
    if (device) SDL_CloseAudioDevice(device);
    device = 0;
    outputs.clear();
    suspended = false;
}
} // namespace hle
