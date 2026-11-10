#include "cpu.h"
#include "dispatch.h"
#include "dr_mp3.h"
#include "dr_wav.h"
#include <SDL2/SDL.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fstream>
#include <map>
#include <memory>
#include <vector>

namespace {
constexpr uint32_t unsupported_format = 0x80000000;
constexpr uint32_t invalid_state = 0x80000000;
constexpr int sample_s16 = 5; // bada AUDIO_SAMPLE_TYPE_16BIT
constexpr int mono = 1, stereo = 2;
constexpr size_t max_media_bytes = 16 * 1024 * 1024;
constexpr size_t max_total_media_bytes = 64 * 1024 * 1024;

bool video_header(const uint8_t* bytes, size_t size) {
    return size >= 12 &&
        ((std::memcmp(bytes, "RIFF", 4) == 0 && std::memcmp(bytes + 8, "AVI ", 4) == 0) ||
         std::memcmp(bytes + 4, "ftyp", 4) == 0);
}

// The decoders borrow host-owned encoded bytes. Their memory APIs decode into
// fixed blocks without file I/O, allocations or guest access in SDL's callback.
struct AudioClip {
    std::vector<uint8_t> bytes;
    drmp3 mp3{};
    drwav wav{};
    enum { none, mpeg, wave } format = none;
    int rate = 0, channels = 0;
    uint64_t frames = 0;
    std::array<int16_t, 1024 * stereo> pcm{};
    size_t available = 0, cursor = 0;

    ~AudioClip() {
        if (format == mpeg) drmp3_uninit(&mp3);
        if (format == wave) drwav_uninit(&wav);
    }

    bool init() {
        if (bytes.size() >= 12 && std::memcmp(bytes.data() + 8, "WAVE", 4) == 0) {
            if (!drwav_init_memory(&wav, bytes.data(), bytes.size(), nullptr)) return false;
            format = wave;
            rate = wav.sampleRate;
            channels = wav.channels;
            frames = wav.totalPCMFrameCount;
        } else {
            if (!drmp3_init_memory(&mp3, bytes.data(), bytes.size(), nullptr)) return false;
            format = mpeg;
            rate = mp3.sampleRate;
            channels = mp3.channels;
            frames = drmp3_get_pcm_frame_count(&mp3);
        }
        return (channels == mono || channels == stereo) && rate >= 1000 && rate <= 192000 &&
            frames && frames <= static_cast<uint64_t>(rate) * 3600;
    }

    bool seek(uint64_t frame) {
        available = cursor = 0;
        return format == mpeg ? drmp3_seek_to_pcm_frame(&mp3, frame) != 0 :
            drwav_seek_to_pcm_frame(&wav, frame) != 0;
    }

    bool read_frame(std::array<int16_t, 2>& value, bool looping, bool& wrapped) {
        wrapped = false;
        if (cursor == available) {
            auto decode = [&]() -> size_t {
                return format == mpeg ? drmp3_read_pcm_frames_s16(&mp3, 1024, pcm.data()) :
                    drwav_read_pcm_frames_s16(&wav, 1024, pcm.data());
            };
            cursor = 0;
            available = decode();
            if (!available && looping && seek(0)) {
                available = decode();
                wrapped = true;
            }
            if (!available) return false;
        }
        value[0] = pcm[cursor * channels];
        value[1] = pcm[cursor * channels + (channels == mono ? 0 : 1)];
        ++cursor;
        return true;
    }
};

enum PlayerState { initialized, opening, opened, end_of_clip, stopped, paused, playing, closed, error };
enum PlayerEvent { opened_event = 1, ended_event = 2, seek_event = 4 };
struct Player {
    std::unique_ptr<AudioClip> clip;
    uint32_t listener = 0, pending = 0, phase = 0;
    uint64_t generation = 0, position = 0;
    PlayerState state = initialized;
    int volume = 100;
    bool looping = false, muted = false, video = false;
    bool ready = false, next_valid = false, next_wraps = false;
    std::array<int16_t, 2> current{}, next{};
    std::chrono::steady_clock::time_point video_started;
};
std::map<uint32_t, Player> players;
uint32_t player_vtable = 0;
constexpr uint64_t video_duration_ms = 300; // retain the unsupported intro-video placeholder

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

// Android's SDL backend has one playback device. Mix AudioOut and Player into
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

uint64_t video_position(const Player& p) {
    return p.position + (p.state == playing && !suspended ?
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - p.video_started).count() : 0);
}

uint64_t duration(const Player& p) {
    return p.clip ? p.clip->frames * 1000 / p.clip->rate : p.video ? video_duration_ms : 0;
}

void invalidate(Player& p) {
    p.pending = 0;
    p.generation = ++next_generation;
}

void release_clip(Player& p) {
    invalidate(p);
    p.clip.reset();
    p.video = p.ready = false;
    p.position = p.phase = 0;
}

bool rewind(Player& p, uint64_t position = 0) {
    p.position = position;
    p.phase = 0;
    p.ready = false;
    return !p.clip || position == p.clip->frames || p.clip->seek(position);
}

void finish(Player& p) {
    p.state = end_of_clip;
    p.pending |= ended_event;
    p.ready = false;
}

void update_video(Player& p) {
    if (p.video && p.state == playing && !p.looping && video_position(p) >= video_duration_ms) {
        p.position = video_duration_ms;
        finish(p);
    }
}

bool prime(Player& p) {
    if (p.position >= p.clip->frames) { finish(p); return false; }
    bool wrapped = false;
    if (!p.clip->read_frame(p.current, p.looping, wrapped)) {
        finish(p);
        return false;
    }
    if (wrapped) p.position = 0;
    p.next_valid = p.clip->read_frame(p.next, p.looping, p.next_wraps);
    if (!p.next_valid) p.next = p.current;
    p.ready = true;
    return true;
}

void advance(Player& p) {
    p.position = p.next_wraps ? 0 : p.position + 1;
    if (!p.next_valid) {
        finish(p);
        return;
    }
    p.current = p.next;
    p.next_valid = p.clip->read_frame(p.next, p.looping, p.next_wraps);
    if (!p.next_valid) p.next = p.current;
}

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
        for (auto& entry : players) {
            auto& p = entry.second;
            if (p.state != playing || !p.clip || (!p.ready && !prime(p))) continue;
            const int volume = p.muted ? 0 : p.volume;
            int values[2];
            for (int channel = 0; channel < stereo; ++channel) {
                values[channel] = (p.current[channel] + static_cast<int>(
                    static_cast<int64_t>(p.next[channel] - p.current[channel]) * p.phase / device_spec.freq))
                    * volume / 100;
            }
            left += values[0];
            right += values[1];
            p.phase += p.clip->rate;
            while (p.phase >= static_cast<uint32_t>(device_spec.freq) && p.state == playing) {
                p.phase -= device_spec.freq;
                advance(p);
            }
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

void player_dtor(Cpu& cpu) {
    AudioLock lock;
    players.erase(cpu.arg32(0));
    cpu.set_ret32(0);
}

void player_delete(Cpu& cpu) {
    const uint32_t object = cpu.arg32(0);
    player_dtor(cpu);
    cpu.free(object);
}

void player_ctor(Cpu& cpu) {
    const uint32_t object = cpu.arg32(0);
    if (!player_vtable) player_vtable = cpu.make_vtable(8, {{0, player_dtor}, {1, player_delete}});
    cpu.write_u32(object, player_vtable);
    AudioLock lock;
    auto& p = players[object];
    p = Player{};
    invalidate(p);
    cpu.set_ret32(object);
}

void player_construct(Cpu& cpu) {
    AudioLock lock;
    players[cpu.arg32(0)].listener = cpu.arg32(1);
    cpu.set_ret32(0);
}

bool media_budget(size_t bytes) {
    if (!bytes || bytes > max_media_bytes) return false;
    AudioLock lock;
    size_t total = bytes, count = 0;
    for (const auto& entry : players) if (entry.second.clip) {
        total += entry.second.clip->bytes.size();
        ++count;
    }
    return total <= max_total_media_bytes && count < 64;
}

void begin_open(Cpu& cpu) {
    AudioLock lock;
    auto& p = players[cpu.arg32(0)];
    release_clip(p);
    p.state = error; // failed reads/decodes must not leave a silent playable clip
}

void open_media(Cpu& cpu, std::unique_ptr<AudioClip> clip, const std::string& source) {
    const bool video = video_header(clip->bytes.data(), clip->bytes.size());
    if (!video && !clip->init()) {
        printf("[player] %s: unsupported or invalid audio\n", source.c_str());
        cpu.set_ret32(unsupported_format);
        return;
    }
    AudioLock lock;
    auto& p = players[cpu.arg32(0)];
    p.video = video;
    if (!video) p.clip = std::move(clip);
    p.state = opened;
    if (cpu.arg32(2)) p.pending |= opened_event;
    printf("[player] %s: %s, %llu ms\n", source.c_str(), video ? "intro video placeholder" :
        p.clip->format == AudioClip::mpeg ? "MP3" : "WAV", static_cast<unsigned long long>(duration(p)));
    cpu.set_ret32(0);
}

void player_openfile(Cpu& cpu) {
    begin_open(cpu);
    const std::string guest = hle::osp_string_get(cpu, cpu.arg32(1));
    std::ifstream input(hle::resolve_package_path(guest), std::ios::binary | std::ios::ate);
    const auto size = input ? input.tellg() : std::streampos(-1);
    if (size <= 0) {
        cpu.set_ret32(invalid_state);
        return;
    }
    // Unsupported intro videos still advance through the existing placeholder.
    // Recognize their container without loading a large movie into memory.
    std::array<uint8_t, 12> header{};
    input.seekg(0);
    if (size >= static_cast<std::streamoff>(header.size()) &&
        input.read(reinterpret_cast<char*>(header.data()), header.size()) && video_header(header.data(), header.size())) {
        auto clip = std::make_unique<AudioClip>();
        clip->bytes.assign(header.begin(), header.end());
        open_media(cpu, std::move(clip), guest);
        return;
    }
    if (size > static_cast<std::streamoff>(max_media_bytes) || !media_budget(static_cast<size_t>(size))) {
        cpu.set_ret32(invalid_state);
        return;
    }
    auto clip = std::make_unique<AudioClip>();
    clip->bytes.resize(static_cast<size_t>(size));
    input.seekg(0);
    if (!input.read(reinterpret_cast<char*>(clip->bytes.data()), clip->bytes.size())) {
        cpu.set_ret32(invalid_state);
        return;
    }
    open_media(cpu, std::move(clip), guest);
}

void player_openbuffer(Cpu& cpu) {
    begin_open(cpu);
    uint32_t data = 0, length = 0;
    if (!hle::byte_buffer_view(cpu.arg32(1), data, length) || !length || !cpu.memory_mapped(data, length)) {
        cpu.set_ret32(invalid_state);
        return;
    }
    std::array<uint8_t, 12> header{};
    if (length >= header.size()) cpu.read_mem(data, header.data(), header.size());
    const bool video = video_header(header.data(), length);
    if (!video && !media_budget(length)) { cpu.set_ret32(invalid_state); return; }
    auto clip = std::make_unique<AudioClip>();
    clip->bytes.resize(video ? header.size() : length);
    cpu.read_mem(data, clip->bytes.data(), clip->bytes.size());
    open_media(cpu, std::move(clip), "OpenBuffer(" + std::to_string(length) + " bytes)");
}

void player_play(Cpu& cpu) {
    bool needs_device = false;
    {
        AudioLock lock;
        const auto& p = players[cpu.arg32(0)];
        if ((!p.clip && !p.video) || p.state == error || p.state == closed) {
            cpu.set_ret32(invalid_state);
            return;
        }
        needs_device = static_cast<bool>(p.clip);
    }
    if (needs_device && !ensure_device()) {
        cpu.set_ret32(invalid_state);
        return;
    }
    AudioLock lock;
    auto& p = players[cpu.arg32(0)];
    if (p.state == playing) { cpu.set_ret32(0); return; }
    if (p.state == end_of_clip) {
        invalidate(p);
        if (!rewind(p)) { cpu.set_ret32(invalid_state); return; }
    }
    p.pending &= ~ended_event;
    p.video_started = std::chrono::steady_clock::now();
    p.state = playing;
    cpu.set_ret32(0);
}

void player_pause(Cpu& cpu) {
    AudioLock lock;
    auto& p = players[cpu.arg32(0)];
    if (p.state == playing) {
        if (p.video) p.position = video_position(p);
        p.state = paused;
    }
    cpu.set_ret32(0);
}

void player_stop(Cpu& cpu) {
    AudioLock lock;
    auto& p = players[cpu.arg32(0)];
    invalidate(p);
    p.state = stopped;
    cpu.set_ret32(rewind(p) ? 0 : invalid_state);
}

void player_close(Cpu& cpu) {
    AudioLock lock;
    auto& p = players[cpu.arg32(0)];
    release_clip(p);
    p.state = closed;
    cpu.set_ret32(0);
}

void player_seek(Cpu& cpu) {
    AudioLock lock;
    auto& p = players[cpu.arg32(0)];
    const int32_t milliseconds = cpu.arg32(1);
    if ((!p.clip && !p.video) || milliseconds < 0) { cpu.set_ret32(invalid_state); return; }
    uint64_t position = p.clip ? std::min(static_cast<uint64_t>(milliseconds) * p.clip->rate / 1000,
        p.clip->frames) : std::min(static_cast<uint64_t>(milliseconds), video_duration_ms);
    if (p.looping && static_cast<uint64_t>(milliseconds) >= duration(p)) position = 0;
    if (!rewind(p, position)) { cpu.set_ret32(invalid_state); return; }
    p.video_started = std::chrono::steady_clock::now();
    const uint32_t opened_pending = p.pending & opened_event;
    invalidate(p);
    p.pending = opened_pending | seek_event;
    if (p.state == end_of_clip) p.state = opened;
    cpu.set_ret32(0);
}

void player_looping(Cpu& cpu) {
    AudioLock lock;
    auto& p = players[cpu.arg32(0)];
    p.looping = cpu.arg32(1) != 0;
    // Refresh interpolation at a loop boundary if looping changes mid-play.
    if (p.clip && p.ready && (p.next_wraps || !p.next_valid)) {
        const uint32_t phase = p.phase;
        rewind(p, p.position);
        p.phase = phase;
    }
    cpu.set_ret32(0);
}

void player_islooping(Cpu& cpu) { AudioLock lock; cpu.set_ret32(players[cpu.arg32(0)].looping); }
void player_mute(Cpu& cpu) { AudioLock lock; players[cpu.arg32(0)].muted = cpu.arg32(1) != 0; cpu.set_ret32(0); }
void player_volume(Cpu& cpu) {
    AudioLock lock;
    players[cpu.arg32(0)].volume = std::clamp(static_cast<int>(cpu.arg32(1)), 0, 100);
    cpu.set_ret32(0);
}
void player_getstate(Cpu& cpu) {
    AudioLock lock;
    auto& p = players[cpu.arg32(0)];
    update_video(p);
    cpu.set_ret32(p.state);
}
void player_getduration(Cpu& cpu) { AudioLock lock; cpu.set_ret32(duration(players[cpu.arg32(0)])); }
void player_getposition(Cpu& cpu) {
    AudioLock lock;
    const auto& p = players[cpu.arg32(0)];
    const uint64_t milliseconds = p.clip ? p.position * 1000 / p.clip->rate : video_position(p);
    cpu.set_ret32(p.looping && duration(p) ? milliseconds % duration(p) : std::min(milliseconds, duration(p)));
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
void register_audio() {
    register_handler("_ZN3Osp5Media6PlayerC1Ev", player_ctor);
    register_handler("_ZN3Osp5Media6PlayerC2Ev", player_ctor);
    register_handler("_ZN3Osp5Media6PlayerD1Ev", player_dtor);
    register_handler("_ZN3Osp5Media6PlayerD2Ev", player_dtor);
    register_handler("_ZN3Osp5Media6PlayerD0Ev", player_delete);
    register_handler("_ZN3Osp5Media6Player9ConstructERKNS0_20IPlayerEventListenerEPKNS_8Graphics10BufferInfoE", player_construct);
    register_handler("_ZN3Osp5Media6Player8OpenFileERKNS_4Base6StringEb", player_openfile);
    register_handler("_ZN3Osp5Media6Player10OpenBufferERKNS_4Base10ByteBufferEb", player_openbuffer);
    register_handler("_ZN3Osp5Media6Player4PlayEv", player_play);
    register_handler("_ZN3Osp5Media6Player4StopEv", player_stop);
    register_handler("_ZN3Osp5Media6Player5PauseEv", player_pause);
    register_handler("_ZN3Osp5Media6Player5CloseEv", player_close);
    register_handler("_ZN3Osp5Media6Player6SeekToEl", player_seek);
    register_handler("_ZN3Osp5Media6Player10SetLoopingEb", player_looping);
    register_handler("_ZN3Osp5Media6Player9IsLoopingEv", player_islooping);
    register_handler("_ZN3Osp5Media6Player7SetMuteEb", player_mute);
    register_handler("_ZN3Osp5Media6Player9SetVolumeEi", player_volume);
    register_handler("_ZNK3Osp5Media6Player8GetStateEv", player_getstate);
    register_handler("_ZNK3Osp5Media6Player11GetDurationEv", player_getduration);
    register_handler("_ZNK3Osp5Media6Player11GetPositionEv", player_getposition);
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

void player_pump(Cpu& cpu) {
    struct Event { uint32_t object, listener, kind; uint64_t generation; };
    std::vector<Event> pending;
    {
        AudioLock lock;
        for (auto& entry : players) {
            auto& p = entry.second;
            update_video(p);
            for (uint32_t kind : {opened_event, seek_event, ended_event})
                if (p.pending & kind) pending.push_back({entry.first, p.listener, kind, p.generation});
            p.pending = 0;
        }
    }
    for (const auto& event : pending) {
        {
            AudioLock lock;
            const auto found = players.find(event.object);
            if (found == players.end() || found->second.generation != event.generation ||
                (event.kind == ended_event && found->second.state != end_of_clip)) continue;
        }
        const uint32_t vtable = event.listener ? cpu.read_u32(event.listener) : 0;
        const uint32_t slot = event.kind == opened_event ? 2 : event.kind == ended_event ? 3 : 8;
        const uint32_t callback = vtable ? cpu.read_u32(vtable + slot * 4) : 0;
        if (!callback) continue;
        uint32_t result = 0;
        // Keep the listener subobject and Thumb bit: the guest vtable thunk
        // adjusts multiple inheritance. Never hold SDL's lock across Unicorn.
        const bool ok = event.kind == ended_event ? cpu.run_guest(callback, {event.listener}, result) :
            cpu.run_guest(callback, {event.listener, 0}, result);
        if (!ok) {
            AudioLock lock;
            const auto found = players.find(event.object);
            if (found != players.end() && found->second.generation == event.generation) {
                found->second.state = error;
                invalidate(found->second);
            }
        }
    }
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
    if (suspended == pause) return;
    for (auto& entry : players) {
        auto& p = entry.second;
        if (p.video && p.state == playing) {
            p.position = video_position(p);
            p.video_started = std::chrono::steady_clock::now();
        }
    }
    suspended = pause;
}

void audio_shutdown() {
    if (device) SDL_CloseAudioDevice(device);
    device = 0;
    outputs.clear();
    players.clear();
    player_vtable = 0;
    suspended = false;
}
} // namespace hle
