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

void boolean_reference_test(Cpu& cpu) {
    const uint32_t key = cpu.alloc(32);
    hle::osp_string_set(cpu, key, "SilentMode");
    const uint32_t value = cpu.alloc(8);
    cpu.write_u32(value, 0x123456ff);
    require(call(cpu, "_ZN3Osp6System11SettingInfo8GetValueERKNS_4Base6StringERb", {key, value}) == 0 &&
            cpu.read_u32(value) == 0x12345600,
            "boolean output overwrote adjacent guest data");
    const uint32_t handler = cpu.bind_trampoline([](Cpu& c) {
        hle::call("_ZN3Osp6System11SettingInfo8GetValueERKNS_4Base6StringERb", c);
    });
    // Match the ABI boundary in the game's IsSilent: its one-byte output is
    // just before the saved r4 word. A word store zeroed r4's low 24 bits.
    const uint32_t code[] = {0xe92d4010, 0xe24dd018, 0xe28d1017, 0xe59f300c,
                             0xe12fff33, 0xe28dd018, 0xe8bd8010, 0xe1a00000, handler};
    cpu.write_mem(0x100200, code, sizeof code);
    cpu.set_reg(UC_ARM_REG_R4, 0x89abcdef);
    uint32_t returned = 0;
    require(cpu.run_guest(0x100200, {key}, returned) && cpu.reg(UC_ARM_REG_R4) == 0x89abcdef,
            "boolean output damaged the caller's saved register");
    cpu.free(value);
    puts("One-byte boolean output and adjacent saved-register preservation passed");
}

void player_test(Cpu& cpu) {
    const uint32_t first = cpu.alloc(32), second = cpu.alloc(32);
    const uint32_t owner = cpu.alloc(32), other_owner = cpu.alloc(32);
    const uint32_t vtable = cpu.make_vtable(8, {{3, [](Cpu& c) {
        const uint32_t listener = c.arg32(0);
        c.write_u32(listener - 8, c.read_u32(listener - 8) + 1);
        c.write_u32(listener - 4, listener);
    }}});
    for (const uint32_t object : {owner, other_owner}) cpu.write_u32(object + 12, vtable);
    constexpr const char* construct = "_ZN3Osp5Media6Player9ConstructERKNS0_20IPlayerEventListenerEPKNS_8Graphics10BufferInfoE";
    const uint8_t video[] = {'R','I','F','F',4,0,0,0,'A','V','I',' '};
    const uint32_t buffer = cpu.alloc(36);
    call(cpu, "_ZN3Osp4Base10ByteBufferC1Ev", {buffer});
    call(cpu, "_ZN3Osp4Base10ByteBuffer9ConstructEi", {buffer, sizeof video});
    cpu.write_mem(call(cpu, "_ZNK3Osp4Base10ByteBuffer10GetPointerEv", {buffer}), video, sizeof video);
    for (const uint32_t object : {first, second}) {
        call(cpu, "_ZN3Osp5Media6PlayerC1Ev", {object});
        require(call(cpu, "_ZN3Osp5Media6Player4PlayEv", {object}) != 0, "unopened Player started");
        require(call(cpu, "_ZN3Osp5Media6Player10OpenBufferERKNS_4Base10ByteBufferEb", {object, buffer, 0}) == 0,
                "intro-video placeholder did not open");
    }
    call(cpu, "_ZN3Osp4Base10ByteBufferD1Ev", {buffer});
    call(cpu, construct, {first, owner + 12, 0});
    call(cpu, construct, {second, other_owner + 12, 0});
    call(cpu, "_ZN3Osp5Media6Player10SetLoopingEb", {second, 1});
    require(call(cpu, "_ZN3Osp5Media6Player9IsLoopingEv", {second}) == 1,
            "Player did not retain its looping setting");
    call(cpu, "_ZN3Osp5Media6Player4PlayEv", {first});
    call(cpu, "_ZN3Osp5Media6Player4PlayEv", {second});
    SDL_Delay(350);
    hle::player_pump(cpu);
    require(cpu.read_u32(owner + 4) == 1 && cpu.read_u32(owner + 8) == owner + 12,
            "video completion did not reach the registered listener subobject");
    require(cpu.read_u32(other_owner + 4) == 0 &&
            call(cpu, "_ZNK3Osp5Media6Player8GetStateEv", {second}) == 6,
            "looping media received an end-of-clip callback or stopped playing");
    hle::player_pump(cpu);
    require(cpu.read_u32(owner + 4) == 1, "video completion fired more than once");
    call(cpu, "_ZN3Osp5Media6Player4PlayEv", {first});
    call(cpu, "_ZN3Osp5Media6Player5PauseEv", {first});
    SDL_Delay(350);
    hle::player_pump(cpu);
    require(cpu.read_u32(owner + 4) == 1 &&
            call(cpu, "_ZNK3Osp5Media6Player8GetStateEv", {first}) == 5,
            "paused media completed behind the app's back");
    for (const uint32_t object : {first, second}) {
        call(cpu, "_ZN3Osp5Media6Player5CloseEv", {object});
        call(cpu, "_ZN3Osp5Media6PlayerD1Ev", {object});
    }
}

void watchdog_test(Cpu& cpu) {
    const uint32_t loop = 0xeafffffe; // ARM b .
    cpu.write_u32(0x100180, loop);
    uint32_t result = 0;
    require(!cpu.run_guest(0x100180, {}, result, 1000), "runaway guest bypassed the deadline");
    require(cpu.run_guest(0x100000, {}, result), "expired watchdog stopped the following callback");
    for (unsigned i = 0; i < 20; ++i)
        require(cpu.run_guest(0x100000, {}, result, 10000), "cancelled deadline stopped a later callback");
    puts("Guest watchdog expiry, recovery and cancellation passed");
}

void heap_test(Cpu& cpu) {
    // More than the entire mapped heap over the lifetime of this loop. A bump
    // allocator with a no-op free exhausts it; a reusable pool stays bounded.
    for (unsigned i = 0; i < 600; ++i) {
        const uint32_t block = call(cpu, i & 1 ? "malloc" : "_Z11Osp_allocExjPKci", {1024 * 1024});
        require(block != 0, "freed guest allocations were not reclaimed");
        cpu.write_u32(block, i);
        require(cpu.read_u32(block) == i, "host pool and Unicorn memory disagree");
        call(cpu, i & 1 ? "free" : "_Z8Osp_freePv", {block});
    }
    const uint32_t dirty = call(cpu, "malloc", {64});
    call(cpu, "memset", {dirty, 0x5a, 64});
    call(cpu, "free", {dirty});
    // Fruit Ninja's optional HUD state relied on the original zero-backed
    // allocation behavior. A recycled ordinary allocation must retain it.
    const uint32_t recycled = call(cpu, "malloc", {64});
    require(recycled != 0, "ordinary allocation failed after recycling");
    for (uint32_t offset = 0; offset < 64; offset += 4)
        require(cpu.read_u32(recycled + offset) == 0, "recycled allocation retained stale HUD state");
    call(cpu, "memset", {recycled, 0x5a, 64});
    call(cpu, "free", {recycled});
    const uint32_t zero = call(cpu, "calloc", {16, 4});
    require(zero != 0, "calloc failed after recycling");
    for (uint32_t offset = 0; offset < 64; offset += 4)
        require(cpu.read_u32(zero + offset) == 0, "calloc retained recycled bytes");
    call(cpu, "free", {zero});
    require(call(cpu, "calloc", {0x80000000u, 2}) == 0, "calloc size multiplication overflowed");
    const uint32_t aligned = cpu.alloc(64, 16);
    require(aligned && (aligned & 15) == 0, "guest aligned allocation lost its alignment");
    call(cpu, "free", {aligned});
    uint32_t data = call(cpu, "malloc", {16});
    cpu.write_u32(data, 0x12345678);
    data = call(cpu, "realloc", {data, 128});
    require(data && cpu.read_u32(data) == 0x12345678, "realloc did not preserve the old prefix");
    require(call(cpu, "realloc", {data, 0xffffffffu}) == 0, "oversized realloc succeeded");
    require(cpu.read_u32(data) == 0x12345678, "failed realloc released the original allocation");
    data = call(cpu, "realloc", {data, 4});
    require(data && cpu.read_u32(data) == 0x12345678, "shrinking realloc corrupted its prefix");
    call(cpu, "realloc", {data, 0});
    call(cpu, "free", {0});
    puts("Guest heap recycling, alignment, calloc and realloc passed");
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
    require(call(cpu, "_ZNK3Osp4Base10BufferBase11GetCapacityEv", {buffer}) == 16, "Flip changed storage capacity");
    call(cpu, "_ZN3Osp4Base10BufferBase6RewindEv", {buffer});
    require(call(cpu, "_ZNK3Osp4Base10BufferBase8GetLimitEv", {buffer}) == 8, "Rewind changed the limit");
    call(cpu, "_ZN3Osp4Base10BufferBase5ClearEv", {buffer});
    require(call(cpu, "_ZNK3Osp4Base10BufferBase8GetLimitEv", {buffer}) == 16, "Clear did not restore capacity");
    call(cpu, "_ZN3Osp4Base10ByteBufferD1Ev", {buffer});
    uint32_t pointer = 0, length = 0;
    require(!hle::byte_buffer_view(buffer, pointer, length), "destroyed buffer still has a descriptor");
}

void memory_transfer_test(Cpu& cpu) {
    const uint32_t source = cpu.alloc(96), destination = cpu.alloc(96);
    std::vector<uint8_t> pattern(96), result(96);
    for (size_t i = 0; i < pattern.size(); ++i) pattern[i] = static_cast<uint8_t>(i * 37);
    cpu.write_mem(source, pattern.data(), pattern.size());
    for (uint32_t count : {0u, 1u, 3u, 4u, 31u, 64u}) {
        call(cpu, "memset", {destination, 0xa5, 96});
        require(call(cpu, "memcpy", {destination + 1, source + 3, count}) == destination + 1,
                "memcpy did not return its destination");
        cpu.read_mem(destination, result.data(), result.size());
        for (size_t i = 0; i < result.size(); ++i)
            require(result[i] == (i > 0 && i <= count ? pattern[i + 2] : 0xa5),
                    "mapped memcpy changed bytes outside its unaligned range");
    }
    for (bool forward : {false, true}) {
        cpu.write_mem(source, pattern.data(), pattern.size());
        const uint32_t from = source + (forward ? 0 : 7), to = source + (forward ? 7 : 0);
        require(call(cpu, "memmove", {to, from, 64}) == to, "memmove return changed");
        cpu.read_mem(source, result.data(), result.size());
        auto expected = pattern;
        std::memmove(expected.data() + (to - source), expected.data() + (from - source), 64);
        require(result == expected, "memmove corrupted an overlapping guest buffer");
    }
    // Transfers larger than the staging chunk must keep overlap direction and
    // boundary bytes intact without allocating a second full guest buffer.
    const uint32_t large = cpu.alloc(131104);
    std::vector<uint8_t> data(131104), actual(data.size());
    for (size_t i = 0; i < data.size(); ++i) data[i] = static_cast<uint8_t>(i * 19);
    for (bool backwards : {false, true}) {
        cpu.write_mem(large, data.data(), data.size());
        auto expected = data;
        const uint32_t from = backwards ? 0 : 17, to = backwards ? 17 : 0;
        std::memmove(expected.data() + to, expected.data() + from, 131073);
        call(cpu, "memmove", {large + to, large + from, 131073});
        cpu.read_mem(large, actual.data(), actual.size());
        require(actual == expected, "large overlapping memmove lost chunk boundary bytes");
    }
    const auto before_fill = actual;
    call(cpu, "memset", {large + 1, 0x1234, 131073});
    cpu.read_mem(large, actual.data(), actual.size());
    for (size_t i = 1; i <= 131073; ++i)
        require(actual[i] == 0x34, "large memset skipped or overran a chunk");
    require(actual[0] == before_fill[0] && actual[131074] == before_fill[131074] &&
            actual.back() == before_fill.back(), "large memset changed bytes outside its range");
    require(!cpu.memory_mapped(0xfffffff0, 64) && !cpu.memory_mapped(0x100ffc, 8),
            "guest range validation accepted overflow or an unmapped page");
    const hle::Handler invalid_transfers[] = {
        [](Cpu& c) { hle::call("memset", c); },
        [](Cpu& c) { hle::call("memcpy", c); },
        [](Cpu& c) { hle::call("memcmp", c); }
    };
    for (const hle::Handler callback : invalid_transfers) {
        const uint32_t handler = cpu.bind_trampoline(callback);
        uint32_t returned = 0;
        require(!cpu.run_guest(handler, {large, large, 0xfffffff0}, returned),
                "invalid bulk transfer did not fail its guest callback");
        require(cpu.run_guest(0x100000, {}, returned), "handler failure poisoned a later guest call");
    }
    cpu.free(large);
    // Executable-image memory is owned by Unicorn, outside the shared heap.
    const uint32_t value = 0x87654321;
    cpu.write_u32(0x1001c0, value);
    call(cpu, "memcpy", {destination, 0x1001c0, 4});
    require(cpu.read_u32(destination) == value, "memcpy lost its non-heap fallback");
    call(cpu, "free", {source});
    call(cpu, "free", {destination});
    puts("Mapped memory transfers, unaligned ranges and overlapping memmove passed");
}

void wide_string_test(Cpu& cpu) {
    // Nova's XML type tags are UTF-16; its ELF explicitly selects 2-byte wchar_t.
    const char16_t vector[] = u"vector3d", quaternion[] = u"quaternion";
    const char16_t high[] = u"\u0200", low[] = u"\u0100";
    const uint32_t a = cpu.alloc(sizeof vector), b = cpu.alloc(sizeof quaternion);
    cpu.write_mem(a, vector, sizeof vector);
    cpu.write_mem(b, quaternion, sizeof quaternion);
    require(call(cpu, "wcscmp", {a, a}) == 0, "equal XML type tags compare differently");
    require(static_cast<int32_t>(call(cpu, "wcscmp", {a, b})) > 0,
            "different XML type tags were accepted as equal");
    require(static_cast<int32_t>(call(cpu, "wcscmp", {b, a})) < 0,
            "wide string ordering is reversed");
    cpu.write_mem(a, high, sizeof high);
    cpu.write_mem(b, low, sizeof low);
    require(static_cast<int32_t>(call(cpu, "wcscmp", {a, b})) > 0,
            "wide comparison treated a zero low byte as the terminator");

    const char filename[] = "/Res/anim";
    const uint32_t narrow = cpu.alloc(32), wide = cpu.alloc(32);
    cpu.write_mem(narrow, filename, sizeof filename);
    cpu.write_u32(wide + 20, 0x12345678);
    require(call(cpu, "mbstowcs", {wide, narrow, 16}) == 9 &&
            call(cpu, "mbstowcs", {0, narrow, 0}) == 9,
            "package filename conversion returned the wrong wide-character count");
    char16_t converted[10];
    cpu.read_mem(wide, converted, sizeof converted);
    require(std::u16string(converted) == u"/Res/anim" && cpu.read_u32(wide + 20) == 0x12345678,
            "package filename conversion used host wchar_t width or overwrote its output");
    cpu.write_u32(wide + 8, 0x12345678);
    require(call(cpu, "mbstowcs", {wide, narrow, 4}) == 4 &&
            cpu.read_u32(wide + 8) == 0x12345678,
            "truncated wide conversion wrote a terminator past the requested capacity");
    const char unicode[] = u8"/Res/\u00e4";
    cpu.write_mem(narrow, unicode, sizeof unicode);
    require(call(cpu, "mbstowcs", {wide, narrow, 16}) == 6,
            "UTF-8 filename conversion counted bytes as wide characters");
    cpu.read_mem(wide, converted, 14);
    require(std::u16string(converted) == u"/Res/\u00e4", "UTF-8 filename conversion lost the code point");
}

void guest_string_test(Cpu& cpu) {
    std::string long_name(600, 'x');
    long_name[257] = 'y';
    cpu.write_mem(0x100600, long_name.c_str(), long_name.size() + 1);
    require(cpu.read_cstr(0x100600) == long_name, "executable string lost bytes across read chunks");
    const char edge[] = "shader";
    cpu.write_mem(0x100ff9, edge, sizeof edge);
    require(cpu.read_cstr(0x100ff9) == edge, "page-end string required an extra mapped page");
    cpu.write_mem(0x100ffa, edge, sizeof edge - 1);
    require(cpu.read_cstr(0x100ffa) == edge && cpu.read_cstr(0x100ffa, 3) == "sha",
            "unterminated page-end string overran its mapping or requested length");
    require(cpu.read_cstr(0xffffffff, 64).empty(), "invalid string address wrapped into another guest region");
    require(uc_mem_map(cpu.uc, 0x150000, 8192, UC_PROT_READ | UC_PROT_WRITE) == UC_ERR_OK,
            "could not map cross-page string test memory");
    cpu.write_mem(0x150ff3, long_name.c_str(), long_name.size() + 1);
    require(cpu.read_cstr(0x150ff3) == long_name, "string read stopped at a mapped page boundary");
    uc_mem_unmap(cpu.uc, 0x150000, 8192);
    puts("Chunked guest string reads, bounded lengths and page boundaries passed");
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
    cpu.write_u32(owner + 4, 0);
    cpu.write_u32(owner + 8, 0);
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
    cpu.write_u32(owner2 + 4, 0);
    cpu.write_u32(owner2 + 8, 0);
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
        heap_test(cpu);
        watchdog_test(cpu);
        timer_test(cpu);
        boolean_reference_test(cpu);
        player_test(cpu);
        buffer_test(cpu);
        memory_transfer_test(cpu);
        guest_string_test(cpu);
        wide_string_test(cpu);
        audio_test(cpu, recording);
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
