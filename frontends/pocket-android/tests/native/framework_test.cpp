#define SDL_MAIN_HANDLED
#include <SDL2/SDL.h>
#include "cpu.h"
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <limits>
#include <stdexcept>
#include <vector>

namespace {
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
uint32_t call(Cpu& cpu, const char* name, std::initializer_list<uint32_t> args = {}) {
    unsigned index = 0;
    for (auto value : args) {
        if (index < 4) cpu.set_reg(UC_ARM_REG_R0 + index, value);
        else cpu.write_u32(cpu.reg(UC_ARM_REG_SP) + (index - 4) * 4, value);
        ++index;
    }
    hle::call(name, cpu);
    return cpu.reg(UC_ARM_REG_R0);
}
uint32_t text(Cpu& cpu, const char* value) {
    const uint32_t object = cpu.alloc(static_cast<uint32_t>(std::strlen(value) + 1));
    cpu.write_mem(object, value, static_cast<uint32_t>(std::strlen(value) + 1));
    return object;
}
void string_search_test(Cpu& cpu) {
    const uint32_t path = text(cpu, "/Res/font/font.dds"), empty = text(cpu, "");
    for (const auto name : {"strchr", "strrchr"}) {
        require(call(cpu, name, {path, 0}) == path + 18, "NUL search did not return the string terminator");
        require(call(cpu, name, {empty, 0}) == empty, "empty string has no terminator");
        require(call(cpu, name, {path, 'z'}) == 0, "missing character was found");
        require(call(cpu, name, {path, 256}) == path + 18, "character search did not narrow its int argument");
    }
    require(call(cpu, "strchr", {path, '/'}) == path &&
            call(cpu, "strrchr", {path, '/'}) == path + 9, "forward/reverse character search returned the wrong match");
    cpu.free(path);
    cpu.free(empty);
}

void switch_uqi(Cpu& cpu) { hle::call("__gnu_thumb1_case_uqi", cpu); }
void switch_uhi(Cpu& cpu) { hle::call("__gnu_thumb1_case_uhi", cpu); }
void switch_shi(Cpu& cpu) { hle::call("__gnu_thumb1_case_shi", cpu); }
void thumb_switch_test(Cpu& cpu) {
    // PUSH {r4,lr}; BLX r3; inline offsets; each case adds a distinct number
    // to the original selector and POPs {r4,pc}. LR retains the Thumb bit.
    const uint16_t bytes[] = {0xb510,0x4798,0x0402,0x0006,0x300a,0xbd10,0x3014,0xbd10,0x301e,0xbd10};
    const uint16_t halves[] = {0xb510,0x4798,4,6,8,0,0x300a,0xbd10,0x3014,0xbd10,0x301e,0xbd10};
    // The first signed-halfword case is before the inline table.
    const uint16_t signed_halves[] = {0x46c0,0x46c0,0x300a,0xbd10,0x46c0,0x46c0,0x46c0,0x46c0,
        0xb510,0x4798,0xfff8,4,6,0,0x3014,0xbd10,0x301e,0xbd10};
    cpu.write_mem(0x100200, bytes, sizeof bytes);
    cpu.write_mem(0x100240, halves, sizeof halves);
    cpu.write_mem(0x100280, signed_halves, sizeof signed_halves);
    const std::array<std::pair<uint32_t, hle::Handler>, 3> cases = {{{0x100201, switch_uqi},
        {0x100241, switch_uhi}, {0x100291, switch_shi}}};
    for (const auto& entry : cases) {
        const uint32_t helper = cpu.bind_trampoline(entry.second);
        for (uint32_t selector = 0; selector < 3; ++selector) {
            const uint32_t stack = cpu.reg(UC_ARM_REG_SP);
            cpu.set_reg(UC_ARM_REG_R4, 0x12345678);
            uint32_t result = 0;
            require(cpu.run_guest(entry.first, {selector, 0x89abcdef, 0x76543210, helper}, result),
                    "Thumb switch helper did not reach its case");
            require(result == selector + (selector + 1) * 10, "Thumb switch lost its selector or chose the wrong case");
            require(cpu.reg(UC_ARM_REG_R1) == 0x89abcdef && cpu.reg(UC_ARM_REG_R2) == 0x76543210 &&
                    cpu.reg(UC_ARM_REG_R4) == 0x12345678 && cpu.reg(UC_ARM_REG_SP) == stack,
                    "Thumb switch changed preserved registers or its stack");
        }
    }
}
void unsigned_float_test(Cpu& cpu) {
    const std::pair<float, uint32_t> cases[] = {{0.0f,0}, {-1.0f,0}, {1.75f,1},
        {4294967040.0f,0xffffff00u}, {4294967296.0f,UINT32_MAX},
        {std::numeric_limits<float>::infinity(),UINT32_MAX}, {std::numeric_limits<float>::quiet_NaN(),0}};
    for (bool hard : {false, true}) {
        cpu.hard_float = hard;
        for (const auto& entry : cases) {
            uint32_t bits = 0;
            std::memcpy(&bits, &entry.first, sizeof bits);
            require(call(cpu, "__aeabi_f2uiz", {bits}) == entry.second,
                    "unsigned float helper returned the wrong core-register result");
        }
    }
}

void datetime_test(Cpu& cpu) {
    const uint32_t object = cpu.alloc(40), span = cpu.alloc(24);
    cpu.write_u32(object + 32, 0xdeadbeef);
    call(cpu, "_ZN3Osp4Base8DateTimeC1Ev", {object});
    require(call(cpu, "_ZNK3Osp4Base8DateTime7GetTimeEv", {span, object}) == span &&
            call(cpu, "_ZNK3Osp4Base8TimeSpan15GetMillisecondsEv", {span}) == 0 &&
            cpu.reg(UC_ARM_REG_R1) == 0, "default DateTime did not return midnight by value");
    const uint32_t fields[] = {2024,2,29,12,34,(56u << 16) | 789u,0};
    cpu.write_mem(object + 4, fields, sizeof fields);
    const char* getters[] = {"_ZNK3Osp4Base8DateTime7GetYearEv", "_ZNK3Osp4Base8DateTime8GetMonthEv",
        "_ZNK3Osp4Base8DateTime6GetDayEv", "_ZNK3Osp4Base8DateTime7GetHourEv",
        "_ZNK3Osp4Base8DateTime9GetMinuteEv", "_ZNK3Osp4Base8DateTime9GetSecondEv"};
    for (unsigned index = 0; index < 6; ++index)
        require(call(cpu, getters[index], {object}) == (index == 5 ? 56u : fields[index]),
                "DateTime getter did not read the guest object's fields");
    call(cpu, "_ZNK3Osp4Base8DateTime7GetTimeEv", {span, object});
    require(call(cpu, "_ZNK3Osp4Base8TimeSpan15GetMillisecondsEv", {span}) == 45296789 &&
            cpu.reg(UC_ARM_REG_R1) == 0 && cpu.read_u32(object + 32) == 0xdeadbeef,
            "DateTime time-of-day lost its milliseconds or overwrote adjacent storage");
    const char* current = "_ZN3Osp6System10SystemTime14GetCurrentTimeENS0_8TimeModeERNS_4Base8DateTimeE";
    require(call(cpu, current, {0, object}) == 0 && call(cpu, getters[0], {object}) >= 2020 &&
            call(cpu, getters[1], {object}) >= 1 && call(cpu, getters[1], {object}) <= 12 &&
            call(cpu, getters[5], {object}) < 60, "SystemTime did not populate a current UTC DateTime");
    require(call(cpu, current, {3, object}) != 0 && call(cpu, current, {0, 0}) != 0,
            "SystemTime accepted an invalid mode or output object");

    const uint32_t tm_object = cpu.alloc(40);
    std::tm normalized{};
    normalized.tm_sec = 66; normalized.tm_min = 5; normalized.tm_hour = 4;
    normalized.tm_mday = 2; normalized.tm_mon = 0; normalized.tm_year = 103; normalized.tm_isdst = -1;
    const int32_t tm_fields[] = {66,5,4,2,0,103,0,0,-1};
    cpu.write_mem(tm_object, tm_fields, sizeof tm_fields);
    cpu.write_u32(tm_object + 36, 0xabcdef12);
    const time_t expected = std::mktime(&normalized);
    require(call(cpu, "mktime", {tm_object}) == static_cast<uint32_t>(expected) &&
            cpu.read_u32(tm_object) == static_cast<uint32_t>(normalized.tm_sec) &&
            cpu.read_u32(tm_object + 4) == static_cast<uint32_t>(normalized.tm_min) &&
            cpu.read_u32(tm_object + 24) == static_cast<uint32_t>(normalized.tm_wday) &&
            cpu.read_u32(tm_object + 36) == 0xabcdef12, "mktime did not normalize the guest's nine-field tm");
    const uint32_t format = text(cpu, "%Y-%m-%d %H:%M:%S"), output = cpu.alloc(32);
    cpu.write_u32(output + 24, 0x12345678);
    require(call(cpu, "strftime", {output, 24, format, tm_object}) == 19 &&
            cpu.read_cstr(output) == "2003-01-02 04:06:06" && cpu.read_u32(output + 24) == 0x12345678,
            "saved-world date formatting lost fields or overwrote its capacity");
    require(call(cpu, "strftime", {output, 5, format, tm_object}) == 0,
            "strftime accepted a buffer too small for the formatted date");
    cpu.write_u32(tm_object + 16, 12); // invalid month must not abort the Windows CRT
    require(call(cpu, "strftime", {output, 24, format, tm_object}) == 0,
            "strftime accepted a malformed guest tm");
    cpu.free(format); cpu.free(output);
    cpu.free(object); cpu.free(span); cpu.free(tm_object);
}

void add_control(Cpu& cpu) { hle::call("_ZN3Osp2Ui9Container10AddControlERKNS0_7ControlE", cpu); }
void form_initialization_test(Cpu& cpu) {
    const uint32_t handler = cpu.bind_trampoline(add_control);
    const uint32_t arm_caller[] = {0xe92d4008,0xe59f3008,0xe12fff33,0xe8bd8008,0xe1a00000,handler};
    const uint16_t thumb_caller[] = {0xb508,0x4b01,0x4798,0xbd08};
    cpu.write_mem(0x100400, arm_caller, sizeof arm_caller);
    cpu.write_mem(0x100440, thumb_caller, sizeof thumb_caller);
    cpu.write_u32(0x100448, handler);
    const uint32_t arm_callback[] = {0xe5902004,0xe2822001,0xe5802004,0xe3a04044,0xe3a00000,0xe12fff1e};
    const uint16_t thumb_callback[] = {0x6842,0x3201,0x6042,0x2444,0x2000,0x4770};
    const uint16_t failed_callback[] = {0x6842,0x3201,0x6042,0x2444,0x2007,0x4770};
    cpu.write_mem(0x100500, arm_callback, sizeof arm_callback);
    cpu.write_mem(0x100540, thumb_callback, sizeof thumb_callback);
    cpu.write_mem(0x100560, failed_callback, sizeof failed_callback);
    const uint32_t parent = cpu.make_stub_object(256), form = cpu.alloc(512);
    for (uint32_t callback : {0x100500u, 0x100541u}) for (uint32_t caller : {0x100400u, 0x100441u}) {
        call(cpu, "_ZN3Osp2Ui8Controls4FormC1Ev", {form});
        cpu.write_u32(form, cpu.make_vtable(5));
        cpu.write_u32(cpu.read_u32(form) + 16, callback);
        cpu.write_u32(form + 4, 0);
        cpu.set_reg(UC_ARM_REG_R4, 0x87654321);
        const uint32_t stack = cpu.reg(UC_ARM_REG_SP);
        uint32_t result = 0;
        require(cpu.run_guest(caller, {parent, form}, result) && result == 0 && cpu.read_u32(form + 4) == 1,
                "AddControl did not initialize the form and propagate its success");
        require(cpu.reg(UC_ARM_REG_R4) == 0x87654321 && cpu.reg(UC_ARM_REG_SP) == stack,
                "form initialization did not restore the caller context");
        require(cpu.run_guest(caller, {parent, form}, result) && result == 0 && cpu.read_u32(form + 4) == 1,
                "adding the same form initialized it twice");
    }
    call(cpu, "_ZN3Osp2Ui8Controls4FormC1Ev", {form});
    cpu.write_u32(form, cpu.make_vtable(5));
    cpu.write_u32(cpu.read_u32(form) + 16, 0x100561);
    cpu.write_u32(form + 4, 0);
    uint32_t result = 0;
    for (uint32_t count = 1; count <= 2; ++count)
        require(cpu.run_guest(0x100400, {parent, form}, result) && result == 7 && cpu.read_u32(form + 4) == count,
                "failed initialization attached the form or prevented a retry");

    // A form initializes a nested child before its own callback returns.
    const uint32_t nested = cpu.alloc(512);
    call(cpu, "_ZN3Osp2Ui8Controls4FormC1Ev", {nested});
    cpu.write_u32(nested, cpu.make_vtable(6));
    cpu.write_u32(cpu.read_u32(nested) + 16, 0x100500);
    cpu.write_u32(nested + 4, 0);
    const uint32_t nested_callback[] = {0xe92d4008,0xe5901008,0xe59f3004,0xe12fff33,0xe8bd8008,handler};
    cpu.write_mem(0x100580, nested_callback, sizeof nested_callback);
    cpu.write_u32(cpu.read_u32(form) + 16, 0x100580);
    cpu.write_u32(form + 8, nested);
    require(cpu.run_guest(0x100441, {parent, form}, result) && result == 0 && cpu.read_u32(nested + 4) == 1,
            "nested initialization did not resume both callers");
    require(cpu.run_guest(0x100400, {parent, form}, result) && result == 0 && cpu.read_u32(nested + 4) == 1,
            "nested attachment was lost after the callback returned");
    const uint32_t rectangle = cpu.alloc(24);
    cpu.write_u32(rectangle + 20, 0xdeadbeef);
    call(cpu, "_ZN3Osp2Ui8Controls4Form14SetOrientationENS0_11OrientationE", {form, 2});
    require(call(cpu, "_ZN3Osp2Ui8Controls4Form20GetOrientationStatusEv", {form}) == 2,
            "form orientation query lost the requested layout");
    require(call(cpu, "_ZNK3Osp2Ui7Control9GetBoundsEv", {rectangle, form}) == rectangle &&
            cpu.read_u32(rectangle + 12) == 800 && cpu.read_u32(rectangle + 16) == 480 &&
            cpu.read_u32(rectangle + 20) == 0xdeadbeef, "landscape bounds returned the wrong rectangle layout");
    cpu.free(rectangle); cpu.free(nested); cpu.free(form); cpu.free(parent);
}
} // namespace

int main() {
    try {
        SDL_SetMainReady();
        require(SDL_Init(SDL_INIT_TIMER) == 0, SDL_GetError());
        ElfImage image;
        const uint32_t instruction = 0xe12fff1e;
        image.segments.push_back({0x100000, 4, 4096, 7, reinterpret_cast<const uint8_t*>(&instruction)});
        Cpu cpu;
        std::string error;
        const bool initialized = cpu.init(image, error);
        require(initialized, error.c_str());
        cpu.set_reg(UC_ARM_REG_SP, cpu.reg(UC_ARM_REG_SP) - 256);
        string_search_test(cpu);
        thumb_switch_test(cpu);
        unsigned_float_test(cpu);
        datetime_test(cpu);
        form_initialization_test(cpu);
        SDL_Quit();
        puts("String terminators, Thumb switch tables, dates and ARM/Thumb form initialization passed");
        return 0;
    } catch (const std::exception& error) {
        fprintf(stderr, "FAIL: %s\n", error.what());
        SDL_Quit();
        return 1;
    }
}
