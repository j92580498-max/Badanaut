#define SDL_MAIN_HANDLED
#include <SDL2/SDL.h>
#include "cpu.h"
#include "hle/gl_api.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <vector>

namespace {
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

uint32_t call(Cpu& cpu, const char* symbol, std::initializer_list<uint32_t> args = {}) {
    int index = 0;
    for (uint32_t arg : args) {
        if (index < 4) cpu.set_reg(UC_ARM_REG_R0 + index, arg);
        else cpu.write_u32(cpu.reg(UC_ARM_REG_SP) + (index - 4) * 4, arg);
        ++index;
    }
    hle::call(symbol, cpu);
    return cpu.reg(UC_ARM_REG_R0);
}

void timer_test(Cpu& cpu) {
    const uint32_t before = SDL_GetTicks();
    call(cpu, "_ZN3Osp4Base7Runtime5Timer5StartEi", {0, 30});
    require(cpu.timer_active && cpu.timer_interval_ms == 30, "timer was not armed");
    require(static_cast<int32_t>(cpu.timer_deadline_ms - before) >= 30 &&
            static_cast<int32_t>(cpu.timer_deadline_ms - before) < 50, "timer deadline does not start at Timer::Start");
    const uint32_t deadline = cpu.timer_deadline_ms;
    // Sky Force re-arms BEFORE rendering. Work must use up the interval instead
    // of receiving another 30 ms of host delay after rendering.
    SDL_Delay(40);
    require(cpu.timer_deadline_ms == deadline &&
            static_cast<int32_t>(SDL_GetTicks() - deadline) >= 0, "render work moved the timer deadline");
    call(cpu, "_ZN3Osp4Base7Runtime5Timer6CancelEv", {0});
    require(!cpu.timer_active, "Timer::Cancel was overridden by a success stub");
}

void buffer_test(Cpu& cpu) {
    const uint32_t buffer = cpu.alloc(36);
    call(cpu, "_ZN3Osp4Base10ByteBufferC1Ev", {buffer});
    call(cpu, "_ZN3Osp4Base10ByteBuffer9ConstructEi", {buffer, 16});
    const uint32_t data = call(cpu, "_ZNK3Osp4Base10ByteBuffer10GetPointerEv", {buffer});
    const uint32_t source = cpu.alloc(8);
    const uint32_t pattern[] = {0x12345678, 0x87654321};
    cpu.write_mem(source, pattern, sizeof pattern);
    require(call(cpu, "_ZN3Osp4Base10ByteBuffer8SetArrayEPKhii", {buffer, source, 4, 8}) == 0, "SetArray failed");
    require(cpu.read_u32(data + 4) == pattern[0] && cpu.read_u32(data + 8) == pattern[1], "SetArray discarded PCM bytes");
    require(call(cpu, "_ZN3Osp4Base10ByteBuffer8SetArrayEPKhii", {buffer, source, 15, 8}) != 0, "SetArray accepted overflow");
    call(cpu, "_ZN3Osp4Base10BufferBase11SetPositionEi", {buffer, 8});
    call(cpu, "_ZN3Osp4Base10BufferBase4FlipENS0_10PositionToE", {buffer, 0});
    require(call(cpu, "_ZNK3Osp4Base10BufferBase8GetLimitEv", {buffer}) == 8, "Flip did not shorten the buffer");
    call(cpu, "_ZN3Osp4Base10BufferBase6RewindEv", {buffer});
    require(call(cpu, "_ZNK3Osp4Base10BufferBase8GetLimitEv", {buffer}) == 8, "Rewind changed the limit");
    call(cpu, "_ZN3Osp4Base10BufferBase5ClearEv", {buffer});
    require(call(cpu, "_ZNK3Osp4Base10BufferBase8GetLimitEv", {buffer}) == 16, "Clear did not restore capacity");
    call(cpu, "_ZN3Osp4Base10ByteBufferD1Ev", {buffer});
    uint32_t pointer = 0, length = 0;
    require(!hle::byte_buffer_view(buffer, pointer, length), "destroyed buffer still has a descriptor");
}

uint32_t queue(Cpu& cpu, uint32_t output, int frames, int channels, int16_t left, int16_t right) {
    std::vector<int16_t> samples(frames * channels);
    for (int frame = 0; frame < frames; ++frame) {
        samples[frame * channels] = left;
        if (channels == 2) samples[frame * channels + 1] = right;
    }
    const uint32_t buffer = cpu.alloc(36);
    call(cpu, "_ZN3Osp4Base10ByteBufferC1Ev", {buffer});
    call(cpu, "_ZN3Osp4Base10ByteBuffer9ConstructEi", {buffer, static_cast<uint32_t>(samples.size() * 2)});
    const uint32_t pointer = call(cpu, "_ZNK3Osp4Base10ByteBuffer10GetPointerEv", {buffer});
    cpu.write_mem(pointer, samples.data(), samples.size() * 2);
    require(call(cpu, "_ZN3Osp5Media8AudioOut11WriteBufferERKNS_4Base10ByteBufferE", {output, buffer}) == 0, "PCM queue failed");
    // Fruit Ninja destroys each buffer immediately; Sky Force overwrites it.
    std::fill(samples.begin(), samples.end(), 0);
    cpu.write_mem(pointer, samples.data(), samples.size() * 2);
    call(cpu, "_ZN3Osp4Base10ByteBufferD1Ev", {buffer});
    return buffer;
}

void await_completion(Cpu& cpu, uint32_t owner, uint32_t count) {
    const uint32_t deadline = SDL_GetTicks() + 1500;
    while (cpu.read_u32(owner + 4) < count && static_cast<int32_t>(deadline - SDL_GetTicks()) > 0) {
        hle::audio_pump(cpu);
        SDL_Delay(5);
    }
    require(cpu.read_u32(owner + 4) == count, "audio did not call the Thumb listener on the emulation thread");
}

void audio_test(Cpu& cpu, const std::filesystem::path& recording) {
    const uint32_t owner = cpu.alloc(32), listener = owner + 12;
    const uint32_t output = cpu.alloc(20);
    const uint32_t vtable = cpu.make_vtable(3);
    // A real Thumb multiple-inheritance thunk: adjust listener this to its
    // owner, count buffer completions, and record the AudioOut reference.
    const uint16_t thunk[] = {0x380c, 0x6842, 0x3201, 0x6042, 0x6081, 0x4770};
    cpu.write_mem(0x100100, thunk, sizeof thunk);
    cpu.write_u32(vtable + 2 * 4, 0x100101);
    cpu.write_u32(listener, vtable);
    call(cpu, "_ZN3Osp5Media8AudioOutC1Ev", {output});
    call(cpu, "_ZN3Osp5Media8AudioOut9ConstructERKNS0_22IAudioOutEventListenerE", {output, listener});
    require(call(cpu, "_ZNK3Osp5Media8AudioOut22GetOptimizedSampleTypeEv", {output}) == 5, "invalid optimized PCM type");
    require(call(cpu, "_ZNK3Osp5Media8AudioOut23GetOptimizedChannelTypeEv", {output}) == 1, "invalid optimized channel type");
    require(call(cpu, "_ZNK3Osp5Media8AudioOut22GetOptimizedSampleRateEv", {output}) == 44100, "invalid optimized sample rate");
    require(call(cpu, "_ZNK3Osp5Media8AudioOut16GetMinBufferSizeEv", {output}) > 0, "zero-sized Sky Force audio buffer");
    require(call(cpu, "_ZN3Osp5Media8AudioOut7PrepareENS0_15AudioSampleTypeENS0_16AudioChannelTypeEi",
                 {output, 0, 2, 8000}) != 0, "unsupported PCM type was accepted");
    require(call(cpu, "_ZN3Osp5Media8AudioOut7PrepareENS0_15AudioSampleTypeENS0_16AudioChannelTypeEi",
                 {output, 5, 2, 8000}) == 0, "Fruit Ninja PCM prepare failed");
    call(cpu, "_ZN3Osp5Media8AudioOut9SetVolumeEi", {output, 50});
    queue(cpu, output, 300, 2, 6000, -4000); // Fruit Ninja's 1200-byte / 37.5 ms block
    SDL_Delay(60);
    hle::audio_pump(cpu);
    require(cpu.read_u32(owner + 4) == 0, "buffer consumed before Start");
    hle::audio_pause(true);
    call(cpu, "_ZN3Osp5Media8AudioOut5StartEv", {output});
    SDL_Delay(60);
    hle::audio_pump(cpu);
    require(cpu.read_u32(owner + 4) == 0, "background audio consumed a paused buffer");
    hle::audio_pause(false);
    SDL_Delay(100);
    require(cpu.read_u32(owner + 4) == 0, "audio thread re-entered Unicorn");
    await_completion(cpu, owner, 1);
    require(cpu.read_u32(owner + 8) == output, "listener received the wrong AudioOut object");
    SDL_Delay(30);
    hle::audio_pump(cpu);
    require(cpu.read_u32(owner + 4) == 1, "underrun generated duplicate completions");

    call(cpu, "_ZN3Osp5Media8AudioOut4StopEv", {output});
    require(call(cpu, "_ZN3Osp5Media8AudioOut7PrepareENS0_15AudioSampleTypeENS0_16AudioChannelTypeEi",
                 {output, 5, 1, 44100}) == 0, "Sky Force PCM prepare failed");
    call(cpu, "_ZN3Osp5Media8AudioOut9SetVolumeEi", {output, 100});
    queue(cpu, output, 1024, 1, 2000, 0);
    call(cpu, "_ZN3Osp5Media8AudioOut5StartEv", {output});
    await_completion(cpu, owner, 2);
    hle::audio_pause(true);
    queue(cpu, output, 1024, 1, 10000, 0);
    call(cpu, "_ZN3Osp5Media8AudioOut4StopEv", {output});
    hle::audio_pause(false);
    SDL_Delay(40);
    hle::audio_pump(cpu);
    require(cpu.read_u32(owner + 4) == 2, "Stop left a queued buffer completion");
    call(cpu, "_ZN3Osp5Media8AudioOut9UnprepareEv", {output});
    require(call(cpu, "_ZN3Osp5Media8AudioOut5StartEv", {output}) != 0, "unprepared stream started");

    // SDL's Android backend permits only one playback device. Two AudioOut
    // objects must mix through it, with independent completion callbacks.
    const uint32_t owner2 = cpu.alloc(32), listener2 = owner2 + 12;
    const uint32_t output2 = cpu.alloc(20);
    cpu.write_u32(listener2, vtable);
    call(cpu, "_ZN3Osp5Media8AudioOutC1Ev", {output2});
    call(cpu, "_ZN3Osp5Media8AudioOut9ConstructERKNS0_22IAudioOutEventListenerE", {output2, listener2});
    hle::audio_pause(true);
    for (const uint32_t stream : {output, output2}) {
        require(call(cpu, "_ZN3Osp5Media8AudioOut7PrepareENS0_15AudioSampleTypeENS0_16AudioChannelTypeEi",
                     {stream, 5, 2, 48000}) == 0, "simultaneous PCM prepare failed");
        queue(cpu, stream, 2400, 2, 20000, -22000);
        require(call(cpu, "_ZN3Osp5Media8AudioOut5StartEv", {stream}) == 0, "simultaneous PCM start failed");
    }
    hle::audio_pause(false);
    await_completion(cpu, owner, 3);
    await_completion(cpu, owner2, 1);
    require(cpu.read_u32(owner + 8) == output && cpu.read_u32(owner2 + 8) == output2,
            "mixed streams sent a completion to the wrong listener");
    call(cpu, "_ZN3Osp5Media8AudioOutD1Ev", {output2});
    call(cpu, "_ZN3Osp5Media8AudioOutD1Ev", {output});
    hle::audio_shutdown();

    std::ifstream input(recording, std::ios::binary);
    std::vector<int16_t> recorded;
    int16_t value;
    while (input.read(reinterpret_cast<char*>(&value), sizeof value)) recorded.push_back(value);
    size_t stereo_frames = 0, mono_frames = 0, mixed_frames = 0;
    for (size_t frame = 0; frame + 1 < recorded.size(); frame += 2) {
        if (recorded[frame] == 3000 && recorded[frame + 1] == -2000) ++stereo_frames;
        else if (recorded[frame] == 2000 && recorded[frame + 1] == 2000) ++mono_frames;
        else if (recorded[frame] == 32767 && recorded[frame + 1] == -32768) ++mixed_frames;
        else require(recorded[frame] == 0 && recorded[frame + 1] == 0, "unexpected audio samples or channel order");
    }
    require(stereo_frames == 1800, "8 kHz stereo PCM was not resampled at the correct volume/duration");
    require(mono_frames == static_cast<size_t>(std::ceil(1024.0 * 48000 / 44100)),
            "44.1 kHz mono PCM was not duplicated/resampled correctly");
    require(mixed_frames == 2400, "simultaneous streams were not mixed/clipped on one playback device");
}
} // namespace

int main() {
    const auto recording = std::filesystem::temp_directory_path() /
        ("badanaut-audio-" + std::to_string(SDL_GetPerformanceCounter()) + ".pcm");
    try {
        SDL_SetMainReady();
        SDL_setenv("SDL_AUDIODRIVER", "disk", 1);
        SDL_setenv("SDL_DISKAUDIOFILE", recording.string().c_str(), 1);
        SDL_setenv("SDL_DISKAUDIODELAY", "10", 1);
        require(SDL_Init(SDL_INIT_TIMER) == 0, SDL_GetError());
        ElfImage image;
        const uint32_t instruction = 0xe12fff1e;
        image.segments.push_back({0x100000, 4, 4096, 5, reinterpret_cast<const uint8_t*>(&instruction)});
        Cpu cpu;
        std::string error;
        require(cpu.init(image, error), error.c_str());
        timer_test(cpu);
        buffer_test(cpu);
        audio_test(cpu, recording);
        uc_close(cpu.uc);
        SDL_Quit();
        std::filesystem::remove(recording);
        puts("Timer deadlines, PCM buffers, audio output and Thumb completion callbacks passed");
        return 0;
    } catch (const std::exception& error) {
        fprintf(stderr, "FAIL: %s\n", error.what());
        hle::audio_shutdown();
        SDL_Quit();
        std::filesystem::remove(recording);
        return 1;
    }
}
