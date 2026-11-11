#define SDL_MAIN_HANDLED
#include <SDL2/SDL.h>
#include "cpu.h"
#include "fixtures/mp3_tone.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <vector>

namespace {
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
uint32_t call(Cpu& cpu, const char* name, std::initializer_list<uint32_t> args) {
    unsigned index = 0;
    for (const uint32_t arg : args) cpu.set_reg(UC_ARM_REG_R0 + index++, arg);
    hle::call(name, cpu);
    return cpu.reg(UC_ARM_REG_R0);
}
constexpr const char* open_file = "_ZN3Osp5Media6Player8OpenFileERKNS_4Base6StringEb";
constexpr const char* open_buffer = "_ZN3Osp5Media6Player10OpenBufferERKNS_4Base10ByteBufferEb";
constexpr const char* play = "_ZN3Osp5Media6Player4PlayEv";
constexpr const char* pause = "_ZN3Osp5Media6Player5PauseEv";
constexpr const char* stop = "_ZN3Osp5Media6Player4StopEv";
constexpr const char* close = "_ZN3Osp5Media6Player5CloseEv";
constexpr const char* state = "_ZNK3Osp5Media6Player8GetStateEv";
constexpr const char* position = "_ZNK3Osp5Media6Player11GetPositionEv";
constexpr const char* duration = "_ZNK3Osp5Media6Player11GetDurationEv";
constexpr const char* seek = "_ZN3Osp5Media6Player6SeekToEl";
constexpr const char* volume = "_ZN3Osp5Media6Player9SetVolumeEi";
constexpr const char* mute = "_ZN3Osp5Media6Player7SetMuteEb";
constexpr const char* looping = "_ZN3Osp5Media6Player10SetLoopingEb";

void put16(std::vector<uint8_t>& bytes, uint16_t value) {
    bytes.push_back(value & 255); bytes.push_back(value >> 8);
}
void put32(std::vector<uint8_t>& bytes, uint32_t value) {
    put16(bytes, value & 65535); put16(bytes, value >> 16);
}
std::vector<uint8_t> wav(int rate, int channels, int frames, int16_t left, int16_t right) {
    std::vector<uint8_t> bytes{'R','I','F','F'};
    put32(bytes, 36 + frames * channels * 2);
    for (const char c : std::string("WAVEfmt ")) bytes.push_back(c);
    put32(bytes, 16); put16(bytes, 1); put16(bytes, channels);
    put32(bytes, rate); put32(bytes, rate * channels * 2);
    put16(bytes, channels * 2); put16(bytes, 16);
    for (const char c : std::string("data")) bytes.push_back(c);
    put32(bytes, frames * channels * 2);
    for (int frame = 0; frame < frames; ++frame) {
        put16(bytes, left);
        if (channels == 2) put16(bytes, right);
    }
    return bytes;
}

uint32_t buffer(Cpu& cpu, const std::vector<uint8_t>& bytes) {
    const uint32_t object = cpu.alloc(36);
    call(cpu, "_ZN3Osp4Base10ByteBufferC1Ev", {object});
    call(cpu, "_ZN3Osp4Base10ByteBuffer9ConstructEi", {object, static_cast<uint32_t>(bytes.size())});
    cpu.write_mem(call(cpu, "_ZNK3Osp4Base10ByteBuffer10GetPointerEv", {object}), bytes.data(), bytes.size());
    return object;
}

struct Endpoint { uint32_t object, owner; };
Endpoint player(Cpu& cpu) {
    Endpoint p{cpu.alloc(32), cpu.alloc(32)};
    const uint32_t vtable = cpu.make_vtable(9, {{2, [](Cpu& c) {
        const uint32_t owner = c.arg32(0) - 12;
        c.write_u32(owner + 16, c.read_u32(owner + 16) + 1);
        c.write_u32(owner + 20, c.arg32(1));
        c.set_ret32(0);
    }}, {8, [](Cpu& c) {
        const uint32_t owner = c.arg32(0) - 12;
        c.write_u32(owner + 24, c.read_u32(owner + 24) + 1);
        c.write_u32(owner + 20, c.arg32(1));
        c.set_ret32(0);
    }}});
    // Preserve the registered listener subobject and execute its real Thumb
    // adjustment thunk before touching the owner, just as Tower Bloxx does.
    const uint16_t thunk[] = {0x4601, 0x380c, 0x6842, 0x3201, 0x6042, 0x6081, 0x4770};
    cpu.write_mem(0x100100, thunk, sizeof thunk);
    cpu.write_u32(vtable + 3 * 4, 0x100101);
    cpu.write_u32(p.owner + 12, vtable);
    call(cpu, "_ZN3Osp5Media6PlayerC1Ev", {p.object});
    call(cpu, "_ZN3Osp5Media6Player9ConstructERKNS0_20IPlayerEventListenerEPKNS_8Graphics10BufferInfoE",
         {p.object, p.owner + 12, 0});
    return p;
}

void open_owned_buffer(Cpu& cpu, Endpoint p, const std::vector<uint8_t>& bytes) {
    const uint32_t input = buffer(cpu, bytes);
    require(call(cpu, open_buffer, {p.object, input, 0}) == 0, "encoded ByteBuffer failed to open");
    const uint32_t data = call(cpu, "_ZNK3Osp4Base10ByteBuffer10GetPointerEv", {input});
    const std::vector<uint8_t> overwritten(bytes.size(), 0);
    cpu.write_mem(data, overwritten.data(), overwritten.size());
    call(cpu, "_ZN3Osp4Base10ByteBufferD1Ev", {input});
    cpu.free(input);
}

void await_end(Cpu& cpu, Endpoint p) {
    const uint32_t deadline = SDL_GetTicks() + 1500;
    while (call(cpu, state, {p.object}) != 3 && static_cast<int32_t>(deadline - SDL_GetTicks()) > 0)
        SDL_Delay(5);
    require(call(cpu, state, {p.object}) == 3, "Player failed to reach real audio EOF");
}

std::vector<int16_t> recording(const std::filesystem::path& path) {
    hle::audio_shutdown(); // closes/flushed SDL's disk driver before reading
    std::ifstream input(path, std::ios::binary);
    require(static_cast<bool>(input), "SDL did not record any audio");
    std::vector<int16_t> samples;
    int16_t value;
    while (input.read(reinterpret_cast<char*>(&value), sizeof value)) samples.push_back(value);
    require(samples.size() % 2 == 0, "partial stereo PCM frame");
    return samples;
}

size_t count_constant(const std::vector<int16_t>& samples, int16_t left, int16_t right) {
    size_t count = 0;
    for (size_t i = 0; i < samples.size(); i += 2) {
        if (samples[i] == left && samples[i + 1] == right) ++count;
        else require(samples[i] == 0 && samples[i + 1] == 0, "unexpected PCM value/channel order");
    }
    return count;
}

void file_playback(Cpu& cpu, const std::filesystem::path& package, const std::filesystem::path& output) {
    const auto bytes = wav(8000, 2, 1600, 6000, -4000); // 200 ms
    std::filesystem::create_directories(package / "Res/Music");
    std::ofstream file(package / "Res/Music/Tone.wav", std::ios::binary);
    file.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    file.close();
    hle::set_package_root(package.string());
    const uint32_t name = cpu.alloc(32);
    hle::osp_string_set(cpu, name, "/rEs/mUsIc/tOnE.WaV");
    const Endpoint p = player(cpu);
    require(call(cpu, open_file, {p.object, name, 1}) == 0, "case-insensitive Player file lookup failed");
    require(call(cpu, duration, {p.object}) == 200 && call(cpu, state, {p.object}) == 2,
            "Player did not expose the WAV duration/open state");
    require(cpu.read_u32(p.owner + 16) == 0, "OpenFile re-entered the guest listener");
    hle::player_pump(cpu);
    require(cpu.read_u32(p.owner + 16) == 1 && cpu.read_u32(p.owner + 20) == 0,
            "asynchronous OnPlayerOpened did not report success");
    hle::audio_pause(true);
    call(cpu, volume, {p.object, 50});
    require(call(cpu, play, {p.object}) == 0, "Player did not start audio");
    SDL_Delay(40);
    require(call(cpu, position, {p.object}) == 0, "background suspension consumed Player audio");
    hle::audio_pause(false);
    SDL_Delay(25);
    call(cpu, pause, {p.object});
    const uint32_t paused_at = call(cpu, position, {p.object});
    require(paused_at > 0 && paused_at < 200 && call(cpu, state, {p.object}) == 5,
            "pause did not retain an audible position");
    SDL_Delay(50);
    require(call(cpu, position, {p.object}) == paused_at, "paused audio continued advancing");
    call(cpu, play, {p.object});
    await_end(cpu, p);
    require(cpu.read_u32(p.owner + 4) == 0, "SDL's audio thread called into Unicorn");
    hle::player_pump(cpu);
    hle::player_pump(cpu);
    require(cpu.read_u32(p.owner + 4) == 1 && cpu.read_u32(p.owner + 8) == p.owner + 12,
            "EOF did not call the Thumb listener exactly once");
    require(call(cpu, position, {p.object}) == 200, "Player stopped at a fabricated duration");
    call(cpu, stop, {p.object});
    require(call(cpu, position, {p.object}) == 0, "Stop did not rewind");
    call(cpu, close, {p.object});
    require(call(cpu, play, {p.object}) != 0 && call(cpu, duration, {p.object}) == 0,
            "Close retained a playable audio source");
    require(count_constant(recording(output), 3000, -2000) == 9600,
            "WAV resampling/volume/pause changed the number of audible samples");
}

void buffer_and_seek(Cpu& cpu, const std::filesystem::path& output) {
    const Endpoint p = player(cpu);
    open_owned_buffer(cpu, p, wav(44100, 1, 4410, 1234, 0));
    require(call(cpu, duration, {p.object}) == 100, "mono WAV duration was wrong");
    require(call(cpu, seek, {p.object, static_cast<uint32_t>(-1)}) != 0, "negative seek was accepted");
    require(call(cpu, seek, {p.object, 50}) == 0 && call(cpu, position, {p.object}) == 50,
            "seek did not move to the requested sample frame");
    require(cpu.read_u32(p.owner + 24) == 0, "SeekTo re-entered the listener");
    hle::player_pump(cpu);
    require(cpu.read_u32(p.owner + 24) == 1 && cpu.read_u32(p.owner + 20) == 0 &&
            cpu.read_u32(p.owner + 16) == 0, "seek/synchronous open callbacks were incorrect");
    call(cpu, play, {p.object});
    await_end(cpu, p);
    hle::player_pump(cpu);
    require(cpu.read_u32(p.owner + 4) == 1, "buffer-backed playback did not finish");
    require(call(cpu, seek, {p.object, 20}) == 0, "seek after EOF failed");
    call(cpu, play, {p.object});
    await_end(cpu, p);
    hle::player_pump(cpu);
    require(cpu.read_u32(p.owner + 4) == 2, "seek after EOF did not create a fresh playback completion");
    require(count_constant(recording(output), 1234, 1234) == 6240,
            "Player borrowed destroyed guest data or failed to seek/resample mono PCM");
}

void mixed_loop(Cpu& cpu, const std::filesystem::path& output) {
    const Endpoint p = player(cpu);
    open_owned_buffer(cpu, p, wav(48000, 2, 96, 1000, -2000));
    call(cpu, looping, {p.object, 1});
    call(cpu, volume, {p.object, 150}); // clamp to 100
    hle::audio_pause(true);
    call(cpu, play, {p.object});
    const uint32_t pcm = cpu.alloc(32);
    call(cpu, "_ZN3Osp5Media8AudioOutC1Ev", {pcm});
    require(call(cpu, "_ZN3Osp5Media8AudioOut7PrepareENS0_15AudioSampleTypeENS0_16AudioChannelTypeEi",
                 {pcm, 5, 2, 48000}) == 0, "Player occupied a second/exclusive playback device");
    std::vector<uint8_t> raw;
    for (int frame = 0; frame < 2400; ++frame) { put16(raw, 32000); put16(raw, -32000); }
    const uint32_t queued = buffer(cpu, raw);
    require(call(cpu, "_ZN3Osp5Media8AudioOut11WriteBufferERKNS_4Base10ByteBufferE", {pcm, queued}) == 0,
            "PCM could not queue while music was open");
    call(cpu, "_ZN3Osp4Base10ByteBufferD1Ev", {queued});
    call(cpu, "_ZN3Osp5Media8AudioOut5StartEv", {pcm});
    hle::audio_pause(false);
    SDL_Delay(100);
    hle::player_pump(cpu);
    require(call(cpu, state, {p.object}) == 6 && call(cpu, position, {p.object}) < 2 &&
            cpu.read_u32(p.owner + 4) == 0, "looping playback stopped or sent EOF");
    call(cpu, mute, {p.object, 1});
    SDL_Delay(40);
    call(cpu, volume, {p.object, 25});
    call(cpu, mute, {p.object, 0});
    SDL_Delay(50);
    call(cpu, looping, {p.object, 0});
    await_end(cpu, p);
    hle::player_pump(cpu);
    require(cpu.read_u32(p.owner + 4) == 1, "disabling looping did not reach EOF");
    const auto samples = recording(output);
    size_t clipped = 0, full = 0, quiet = 0, silent = 0;
    for (size_t i = 0; i < samples.size(); i += 2) {
        if (samples[i] == 32767 && samples[i + 1] == -32768) ++clipped;
        else if (samples[i] == 1000 && samples[i + 1] == -2000) ++full;
        else if (samples[i] == 250 && samples[i + 1] == -500) ++quiet;
        else if (samples[i] == 0 && samples[i + 1] == 0) ++silent;
        else require(false, "music/PCM mixing, mute or live volume produced incorrect samples");
    }
    require(clipped == 2400 && full > 0 && quiet > 0 && silent >= 1024,
            "shared music/PCM mixing, volume clamp or mute was inaudible/incorrect");
}

void mp3_playback(Cpu& cpu, const std::filesystem::path& output) {
    const Endpoint p = player(cpu);
    open_owned_buffer(cpu, p, {std::begin(mp3_tone), std::end(mp3_tone)});
    require(call(cpu, duration, {p.object}) == 100, "MP3 delay/padding metadata produced a wrong duration");
    call(cpu, looping, {p.object, 1});
    call(cpu, play, {p.object});
    SDL_Delay(250);
    hle::player_pump(cpu);
    require(call(cpu, state, {p.object}) == 6 && cpu.read_u32(p.owner + 4) == 0,
            "MP3 looping failed or used the fake video timer");
    call(cpu, pause, {p.object});
    const uint32_t paused_at = call(cpu, position, {p.object});
    SDL_Delay(40);
    require(call(cpu, position, {p.object}) == paused_at, "MP3 pause failed");
    call(cpu, looping, {p.object, 0});
    call(cpu, play, {p.object});
    await_end(cpu, p);
    hle::player_pump(cpu);
    require(cpu.read_u32(p.owner + 4) == 1, "MP3 completion callback missing/duplicated");
    const auto samples = recording(output);
    uint64_t left_energy = 0, right_energy = 0;
    size_t audible = 0, distinct = 0;
    for (size_t i = 0; i < samples.size(); i += 2) {
        const int left = samples[i], right = samples[i + 1];
        left_energy += left * left;
        right_energy += right * right;
        if (std::abs(left) > 128 || std::abs(right) > 128) ++audible;
        if (std::abs(left - right) > 128) ++distinct;
    }
    require(audible > 8000 && distinct > 4000 && left_energy > right_energy * 15 / 10 && right_energy > 100000000,
            "MP3 decoder did not produce the original stereo tone over repeated loops");
}

void stale_events(Cpu& cpu) {
    Endpoint a = player(cpu), b = player(cpu);
    if (a.object > b.object) std::swap(a, b);
    cpu.write_u32(a.owner + 28, b.object);
    const uint32_t listener = a.owner + 12;
    const uint32_t callback = cpu.bind_trampoline([](Cpu& c) {
        const uint32_t owner = c.arg32(0) - 12;
        c.write_u32(owner + 4, c.read_u32(owner + 4) + 1);
        c.set_reg(UC_ARM_REG_R0, c.read_u32(owner + 28));
        hle::call(close, c);
    });
    cpu.write_u32(cpu.read_u32(listener) + 3 * 4, callback);
    hle::audio_pause(true);
    for (const Endpoint p : {a, b}) {
        open_owned_buffer(cpu, p, wav(8000, 1, 80, 1000, 0));
        call(cpu, play, {p.object});
    }
    hle::audio_pause(false);
    await_end(cpu, a); await_end(cpu, b);
    hle::player_pump(cpu);
    require(cpu.read_u32(a.owner + 4) == 1 && cpu.read_u32(b.owner + 4) == 0 &&
            call(cpu, state, {b.object}) == 7, "Close left a stale callback captured by the pump");

    const Endpoint destroyed = player(cpu);
    open_owned_buffer(cpu, destroyed, wav(8000, 1, 80, 1000, 0));
    call(cpu, play, {destroyed.object});
    await_end(cpu, destroyed);
    const uint32_t deleting_destructor = cpu.read_u32(cpu.read_u32(destroyed.object) + 4);
    uint32_t result = 0;
    require(cpu.run_guest(deleting_destructor, {destroyed.object}, result), "virtual Player destructor failed");
    hle::player_pump(cpu);
    require(cpu.read_u32(destroyed.owner + 4) == 0, "destroyed Player delivered a queued completion");

    const Endpoint failed = player(cpu);
    const uint32_t invalid = buffer(cpu, std::vector<uint8_t>(64, 0));
    require(call(cpu, open_buffer, {failed.object, invalid, 0}) != 0 && call(cpu, play, {failed.object}) != 0 &&
            call(cpu, state, {failed.object}) == 8, "malformed audio was accepted as a silent playable clip");
    call(cpu, "_ZN3Osp4Base10ByteBufferD1Ev", {invalid});
    hle::audio_shutdown();
}

void video_placeholder(Cpu& cpu, const std::filesystem::path& package) {
    const uint8_t header[] = {0,0,0,32,'f','t','y','p','i','s','o','m'};
    std::ofstream file(package / "Res/intro.mp4", std::ios::binary);
    file.write(reinterpret_cast<const char*>(header), sizeof header);
    file.seekp(24 * 1024 * 1024 - 1); // sparse movie, larger than the encoded-audio limit
    file.put(0); file.close();
    const uint32_t name = cpu.alloc(32);
    hle::osp_string_set(cpu, name, "/Res/intro.mp4");
    const Endpoint p = player(cpu);
    require(call(cpu, open_file, {p.object, name, 1}) == 0 && call(cpu, duration, {p.object}) == 300,
            "large intro movie lost its existing completion placeholder");
    call(cpu, close, {p.object});
    hle::player_pump(cpu);
    require(cpu.read_u32(p.owner + 16) == 0, "closed movie delivered a stale OnPlayerOpened callback");
    hle::audio_shutdown();
}
} // namespace

int main() {
    const auto package = std::filesystem::temp_directory_path() /
        ("badanaut-player-" + std::to_string(SDL_GetPerformanceCounter()));
    const auto output = package / "recording.pcm";
    try {
        std::filesystem::create_directories(package);
        SDL_SetMainReady();
        SDL_setenv("SDL_AUDIODRIVER", "disk", 1);
        SDL_setenv("SDL_DISKAUDIOFILE", output.string().c_str(), 1);
        SDL_setenv("SDL_DISKAUDIODELAY", "10", 1);
        require(SDL_Init(SDL_INIT_TIMER) == 0, SDL_GetError());
        const uint32_t instruction = 0xe12fff1e;
        ElfImage image;
        image.segments.push_back({0x100000, 4, 4096, 5, reinterpret_cast<const uint8_t*>(&instruction)});
        Cpu cpu;
        std::string error;
        require(cpu.init(image, error), error.c_str());
        file_playback(cpu, package, output);
        buffer_and_seek(cpu, output);
        mixed_loop(cpu, output);
        mp3_playback(cpu, output);
        stale_events(cpu);
        video_placeholder(cpu, package);
        SDL_Quit();
        std::filesystem::remove_all(package);
        puts("Player MP3/WAV output, mixing, lifecycle, looping and Thumb callbacks passed");
        return 0;
    } catch (const std::exception& error) {
        fprintf(stderr, "FAIL: %s\n", error.what());
        hle::audio_shutdown();
        SDL_Quit();
        std::filesystem::remove_all(package);
        return 1;
    }
}
