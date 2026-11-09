#pragma once
#include <cstdint>
#include <string>
#include <vector>

class Cpu;

// Mocked Osp::Ui controls.
//
// bada draws these itself (native widgets over the app's GL surface), so there
// is nothing in the app binary to emulate -- an EditField is a black box the app
// creates, fills and reads back. We keep the state host-side and draw a stand-in
// with Nuklear, then feed what the user types back through the app's listeners
// exactly as bada would.
namespace hle {

struct EditField {
    uint32_t obj = 0; // guest Control*, also the identity used by the app
    int x = 0, y = 0, w = 0, h = 0;
    std::string text;
    int limit = 0;             // 0 = unlimited
    uint32_t text_listener = 0;   // ITextEventListener*
    uint32_t action_listener = 0; // IActionEventListener*
    // The overlay keypad carries up to two command buttons, one per position
    // (0 = left, 1 = right). PvZ uses both: Cancel and Done.
    std::string command[2];
    int action_id[2] = {0, 0};
    bool has_command[2] = {false, false};
    bool visible = false; // ShowKeypad()/Show() opened it
    bool keypad = false; // Keypad reports text only when the session is accepted
    // Bumped whenever the app reopens the control or replaces its text. The
    // overlay keys its widget on this, so the widget starts with fresh state
    // instead of a text selection that no longer fits the new string.
    int generation = 0;
};

void register_ui();

// Fields the overlay should draw, in creation order.
std::vector<EditField*> ui_fields();
// True while any field is on screen -- the overlay grabs keyboard input then.
bool ui_active();
// The user edited the text: store it and call the app's OnTextValueChanged.
void ui_set_text(Cpu& cpu, EditField& f, const std::string& text);
// The user pressed command button `slot`: call OnActionPerformed(control, id)
// and close the field, as bada does when the overlay keypad is dismissed.
void ui_commit(Cpu& cpu, EditField& f, int slot);
// The user dismissed it: OnTextValueChangeCanceled, then close.
void ui_cancel(Cpu& cpu, EditField& f);

} // namespace hle
