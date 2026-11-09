#include "cpu.h"
#include "dispatch.h"
#include <SDL2/SDL.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <vector>

namespace {
constexpr uint32_t unsupported_format = 0x80000000;
constexpr uint32_t invalid_state = 0x80000000;
constexpr int sample_s16 = 5; // bada AUDIO_SAMPLE_TYPE_16BIT
constexpr int mono = 1, stereo = 2;

struct Block {
    std::vector<int16_t> samples;
    size_t position = 0; // input sample frames, independent of the host rate
};

struct AudioOut {
    uint32_t listener = 0;
    uint64_t generation = 0;
    bool prepared = false, playing = false;
    int rate = 44100, channels = mono, volume = 100;
    uint32_t phase = 0, completed = 0;
    size_t queued_frames = 0;
    std::deque<Block> blocks;
};

// Android's SDL backend has one playback device. Mix AudioOut instances into
// that device, and keep all guest calls on the emulation thread.
std::map<uint32_t, AudioOut> outputs;
SDL_AudioDeviceID device = 0;
SDL_AudioSpec device_spec{};
bool suspended = false;
uint64_t next_generation = 0;
float master_gain = 1.0f;

bool normalize_master_volume(Cpu& cpu) {
    // The Megal Bada port stores a deliberately attenuated master level in
    // its settings. Undo that port-specific attenuation before its own mixer,
    // retaining the user's slider, mute, voice gains and saturation behavior.
    const float level = cpu.argf32(0);
    if (std::isfinite(level)) {
        const float normalized = std::clamp(level * master_gain, 0.0f, 1.0f);
        uc_reg_write(cpu.uc, UC_ARM_REG_S0, &normalized);
    }
    return false; // continue through the original guest mixer-volume setter
}

struct AudioLock {
    AudioLock() { if (device) SDL_LockAudioDevice(device); }
    ~AudioLock() { if (device) SDL_UnlockAudioDevice(device); }
};

void reset(AudioOut& out) {
    out.blocks.clear();
    out.phase = out.completed = 0;
    out.queued_frames = 0;
    out.generation = ++next_generation;
}

int sample(const AudioOut& out, int channel, bool next) {
    const auto& block = out.blocks.front();
    size_t position = block.position + (next ? 1 : 0);
    const Block* source = &block;
    if (position * out.channels >= block.samples.size()) {
        if (out.blocks.size() > 1) {
            source = &out.blocks[1];
            position = 0;
        } else {
            position = block.position;
        }
    }
    return source->samples[position * out.channels + (out.channels == mono ? 0 : channel)];
}

void mix(void*, Uint8* bytes, int length) {
    SDL_memset(bytes, 0, length);
    if (suspended) return;
    auto* destination = reinterpret_cast<int16_t*>(bytes);
    const int frames = length / (stereo * sizeof(int16_t));
    for (int frame = 0; frame < frames; ++frame) {
        int left = 0, right = 0;
        for (auto& entry : outputs) {
            auto& out = entry.second;
            if (!out.playing || out.blocks.empty()) continue;
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
            while (out.phase >= static_cast<uint32_t>(device_spec.freq) && !out.blocks.empty()) {
                out.phase -= device_spec.freq;
                auto& block = out.blocks.front();
                ++block.position;
                --out.queued_frames;
                if (block.position * out.channels == block.samples.size()) {
                    out.blocks.pop_front();
                    ++out.completed;
                }
            }
            if (out.blocks.empty()) out.phase = 0;
        }
        destination[frame * stereo] = static_cast<int16_t>(std::clamp(left, -32768, 32767));
        destination[frame * stereo + 1] = static_cast<int16_t>(std::clamp(right, -32768, 32767));
    }
}

bool ensure_device() {
    if (device) return true;
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
    cpu.set_ret32(0);
}

void audio_write(Cpu& cpu) {
    uint32_t data = 0, length = 0;
    if (!hle::byte_buffer_view(cpu.arg32(1), data, length) || !data || !length || length > 4 * 1024 * 1024) {
        cpu.set_ret32(invalid_state);
        return;
    }
    // The game can overwrite or destroy the ByteBuffer as soon as this returns.
    Block block;
    block.samples.resize(length / sizeof(int16_t));
    cpu.read_mem(data, block.samples.data(), length & ~1u);
    AudioLock lock;
    auto& out = outputs[cpu.arg32(0)];
    const size_t frames = block.samples.size() / out.channels;
    if (!out.prepared || length % (out.channels * sizeof(int16_t)) || !frames ||
        out.queued_frames + frames > static_cast<size_t>(out.rate)) {
        cpu.set_ret32(invalid_state);
        return;
    }
    out.queued_frames += frames;
    out.blocks.push_back(std::move(block));
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
bool audio_configure_game(Cpu& cpu, const ElfImage& image) {
    master_gain = 1.0f;
    const uint32_t settings = image.resolve("_ZN13CGameSettings15SetMasterVolumeEf");
    const uint32_t mixer = image.resolve("_ZN16BaseSoundManager15setGlobalVolumeEf");
    if (!cpu.hard_float || !settings || (settings & 1) || !mixer || !image.resolve("megalMixMono16")) return false;
    // Match the scaled float setter, including its load of a literal native
    // master limit. Other games and changed binaries keep unity gain.
    const uint32_t pattern[] = {0xeddf7a02, 0xee607a27, 0xedc07a0c, 0xe12fff1e};
    uint32_t instructions[4]{};
    cpu.read_mem(settings, instructions, sizeof instructions);
    if (memcmp(pattern, instructions, sizeof pattern) != 0) return false;
    float native_limit = 0;
    cpu.read_mem(settings + sizeof pattern, &native_limit, sizeof native_limit);
    if (!std::isfinite(native_limit) || native_limit < 0.01f || native_limit >= 1.0f) return false;
    master_gain = 1.0f / native_limit;
    cpu.patch(mixer, normalize_master_volume);
    printf("[audio] master gain=%.3f native master limit=%.3f (Megal/Bada compatibility)\n", master_gain, native_limit);
    return true;
}

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

void audio_pump(Cpu& cpu) {
    struct Completion { uint32_t object, listener, count; uint64_t generation; };
    std::vector<Completion> pending;
    {
        AudioLock lock;
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
    master_gain = 1.0f;
}
} // namespace hle
