#include "cpu.h"
#include "hle/ui.h"
#include <cstdio>
#include <stdexcept>

namespace {
int changed = 0, cancelled = 0, action = 0;
uint32_t source = 0;
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
uint32_t call(Cpu& cpu, const char* name, std::initializer_list<uint32_t> args) {
    int index = 0;
    for (auto value : args) cpu.set_reg(UC_ARM_REG_R0 + index++, value);
    hle::call(name, cpu);
    return cpu.reg(UC_ARM_REG_R0);
}
void virtual_call(Cpu& cpu, uint32_t object, int slot, std::vector<uint32_t> args = {}) {
    args.insert(args.begin(), object);
    uint32_t result = 0;
    require(cpu.run_guest(cpu.read_u32(cpu.read_u32(object) + slot * 4), args, result), "virtual control call failed");
}
hle::EditField& field(uint32_t object) {
    for (auto* f : hle::ui_fields()) if (f->obj == object) return *f;
    throw std::runtime_error("control was not registered");
}
void controls(Cpu& cpu) {
    const uint32_t listener = cpu.alloc(16);
    cpu.write_u32(listener, cpu.make_vtable(4, {
        {2, [](Cpu& c) { ++changed; source = c.arg32(1); c.set_ret32(0); }},
        {3, [](Cpu& c) { ++cancelled; source = c.arg32(1); c.set_ret32(0); }}
    }));
    const uint32_t keypad = cpu.alloc(256), text = cpu.alloc(16);
    call(cpu, "_ZN3Osp2Ui8Controls6KeypadC1Ev", {keypad});
    require(call(cpu, "_ZN3Osp2Ui8Controls6Keypad9ConstructENS1_11KeypadStyleENS1_23KeypadInputModeCategoryEi", {keypad, 0, 0, 8}) == 0, "keypad construction failed");
    call(cpu, "_ZN3Osp2Ui8Controls6Keypad20AddTextEventListenerERKNS0_18ITextEventListenerE", {keypad, listener});
    virtual_call(cpu, keypad, 16, {1});
    virtual_call(cpu, keypad, 8);
    require(hle::ui_active(), "virtual Show did not open keypad");
    hle::ui_set_text(cpu, field(keypad), "Alex");
    require(changed == 0, "keypad finished on each keystroke");
    hle::ui_set_text(cpu, field(keypad), "TooLongName");
    call(cpu, "_ZNK3Osp2Ui8Controls6Keypad7GetTextEv", {text, keypad});
    require(hle::osp_string_get(cpu, text) == "Alex", "keypad lost text or exceeded its limit");
    require(call(cpu, "_ZNK3Osp4Base6String7IsEmptyEv", {text}) == 0, "entered name reported empty");
    const uint32_t encoded = call(cpu, "_ZN3Osp4Base7Utility10StringUtil13StringToUtf8NERKNS0_6StringE", {text});
    const uint32_t bytes = call(cpu, "_ZNK3Osp4Base10ByteBuffer10GetPointerEv", {encoded});
    char name[5]{};
    cpu.read_mem(bytes, name, sizeof(name));
    require(std::string(name) == "Alex", "Marmalade could not read the UTF-8 name buffer");
    virtual_call(cpu, encoded, 1);
    require(cpu.allocation_size(encoded) == 0 && cpu.allocation_size(bytes) == 0, "UTF-8 conversion leaked its owned result");
    // Native Strings are UTF-16; conversion must also encode non-ASCII units
    // and surrogate pairs, rather than truncating them into bytes.
    const uint16_t unicode[] = {0x00e9, 0xd83d, 0xde00, 0};
    const uint32_t units = cpu.alloc(sizeof(unicode));
    cpu.write_mem(units, unicode, sizeof(unicode));
    call(cpu, "_ZN3Osp4Base6StringC1EPKw", {text, units});
    const uint32_t unicode_buffer = call(cpu, "_ZN3Osp4Base7Utility10StringUtil13StringToUtf8NERKNS0_6StringE", {text});
    const uint32_t unicode_bytes = call(cpu, "_ZNK3Osp4Base10ByteBuffer10GetPointerEv", {unicode_buffer});
    char utf8[7]{};
    cpu.read_mem(unicode_bytes, utf8, sizeof(utf8));
    require(std::string(utf8) == "\xc3\xa9\xf0\x9f\x98\x80", "UTF-16 name was not converted to UTF-8");
    virtual_call(cpu, unicode_buffer, 1);
    call(cpu, "_ZN3Osp4Base6StringC1Ev", {text});
    require(call(cpu, "_ZNK3Osp4Base6String7IsEmptyEv", {text}) == 1, "empty name reported nonempty");
    hle::ui_commit(cpu, field(keypad), 0);
    require(changed == 1 && source == keypad && !hle::ui_active(), "Done did not report the completed name");
    const int generation = field(keypad).generation;
    virtual_call(cpu, keypad, 8);
    require(field(keypad).generation != generation, "new keypad session reused stale widget state");
    hle::ui_commit(cpu, field(keypad), 1);
    require(cancelled == 1 && changed == 1 && !hle::ui_active(), "Cancel accepted the name");
    virtual_call(cpu, keypad, 16, {1});
    virtual_call(cpu, keypad, 16, {0});
    require(!hle::ui_active(), "SetShowState(false) left input active");
    virtual_call(cpu, keypad, 1);
    require(cpu.allocation_size(keypad) == 0 && hle::ui_fields().empty(), "deleting destructor retained keypad state or memory");

    // EditField keeps its existing per-keystroke text notification and action
    // completion, which differs from the standalone Keypad session.
    const uint32_t edit = cpu.alloc(256), actions = cpu.alloc(16);
    call(cpu, "_ZN3Osp2Ui8Controls9EditFieldC1Ev", {edit});
    call(cpu, "_ZN3Osp2Ui8Controls9EditField20AddTextEventListenerERKNS0_18ITextEventListenerE", {edit, listener});
    cpu.write_u32(actions, cpu.make_vtable(3, {{2, [](Cpu& c) {
        source = c.arg32(1); action = c.arg32(2); c.set_ret32(0);
    }}}));
    field(edit).action_listener = actions;
    field(edit).action_id[0] = 42;
    call(cpu, "_ZN3Osp2Ui8Controls9EditField10ShowKeypadEv", {edit});
    hle::ui_set_text(cpu, field(edit), "Player");
    require(changed == 2 && source == edit, "EditField stopped sending text changes");
    hle::ui_commit(cpu, field(edit), 0);
    require(action == 42 && source == edit && !hle::ui_active(), "EditField lost its command action");
}
}
int main() {
    try {
        const uint32_t instruction = 0xe12fff1e;
        ElfImage image;
        image.segments.push_back({0x100000, 4, 4096, 5, reinterpret_cast<const uint8_t*>(&instruction)});
        Cpu cpu;
        std::string error;
        require(cpu.init(image, error), error.c_str());
        controls(cpu);
        puts("Keypad sessions, cancellation, text limits, destruction and EditField callbacks passed");
        return 0;
    } catch (const std::exception& error) {
        fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
