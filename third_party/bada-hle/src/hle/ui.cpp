#include "ui.h"
#include "cpu.h"
#include "dispatch.h"
#include <cstdio>
#include <map>

// Mocked Osp::Ui controls. See ui.h for why these are host-side.
//
// Both listener interfaces derive from IEventListener, whose only virtual is its
// destructor, so their vtables start with the two destructor slots:
//   ITextEventListener:   2 OnTextValueChanged, 3 OnTextValueChangeCanceled
//   IActionEventListener: 2 OnActionPerformed(source, actionId)

namespace {

std::map<uint32_t, hle::EditField> g_fields;
std::vector<uint32_t> g_order; // creation order, for a stable overlay layout

hle::EditField* field(uint32_t obj) {
    auto it = g_fields.find(obj);
    return it == g_fields.end() ? nullptr : &it->second;
}

// Call slot `slot` of the object's vtable with the given arguments.
void call_virtual(Cpu& c, uint32_t self, int slot, const std::vector<uint32_t>& args) {
    if (!self) {
        return;
    }
    uint32_t vt = c.read_u32(self);
    uint32_t fn = vt ? c.read_u32(vt + (uint32_t)slot * 4) : 0;
    if (!fn) {
        return;
    }
    std::vector<uint32_t> a{self};
    a.insert(a.end(), args.begin(), args.end());
    uint32_t ret = 0;
    c.run_guest(fn, a, ret);
}

void ef_ctor(Cpu& c) {
    uint32_t o = c.arg32(0);
    c.write_u32(o, c.noop_vtable());
    if (!g_fields.count(o)) {
        g_order.push_back(o);
    }
    hle::EditField f;
    f.obj = o;
    g_fields[o] = f;
    c.set_ret32(o);
}
// Construct(const Rectangle&, EditFieldStyle, InputStyle, bool, int limit, GroupStyle)
void ef_construct(Cpu& c) {
    hle::EditField* f = field(c.arg32(0));
    if (!f) {
        c.set_ret32(0);
        return;
    }
    uint32_t r = c.arg32(1);
    f->x = (int)c.read_u32(r + 4);
    f->y = (int)c.read_u32(r + 8);
    f->w = (int)c.read_u32(r + 12);
    f->h = (int)c.read_u32(r + 16);
    f->limit = (int)c.arg32(5);
    printf("[ui] EditField 0x%x at %d,%d %dx%d limit=%d\n", f->obj, f->x, f->y, f->w, f->h,
           f->limit);
    c.set_ret32(0);
}
void ef_settext(Cpu& c) {
    if (hle::EditField* f = field(c.arg32(0))) {
        f->text = hle::osp_string_get(c, c.arg32(1));
        f->generation++; // the overlay's widget state no longer fits
    }
    c.set_ret32(0);
}
// GetText() const returns Osp::Base::String by value: r0 = &ret, r1 = this.
void ef_gettext(Cpu& c) {
    uint32_t ret = c.arg32(0);
    hle::EditField* f = field(c.arg32(1));
    hle::osp_string_set(c, ret, f ? f->text : std::string());
    c.set_ret32(ret);
}
void ef_gettextlength(Cpu& c) {
    hle::EditField* f = field(c.arg32(0));
    c.set_ret32(f ? (uint32_t)f->text.size() : 0);
}
void ef_showkeypad(Cpu& c) {
    if (hle::EditField* f = field(c.arg32(0))) {
        if (!f->visible) {
            f->generation++; // a new session: the overlay starts fresh
        }
        f->visible = true;
        printf("[ui] EditField 0x%x keypad shown\n", f->obj);
    }
    c.set_ret32(0);
}
// ScrollPanel::CloseOverlayWindow() dismisses the overlay keypad. It is called
// on the panel that hosts it, not on the field, and only one keypad is ever up.
void panel_close_overlay(Cpu& c) {
    for (auto& [obj, f] : g_fields) {
        f.visible = false;
    }
    c.set_ret32(0);
}
void ef_add_text_listener(Cpu& c) {
    if (hle::EditField* f = field(c.arg32(0))) {
        f->text_listener = c.arg32(1);
    }
    c.set_ret32(0);
}
void ef_add_action_listener(Cpu& c) {
    if (hle::EditField* f = field(c.arg32(0))) {
        f->action_listener = c.arg32(1);
    }
    c.set_ret32(0);
}
// SetOverlayKeypadCommandButton(CommandButtonPosition, const String& text, int actionId)
void ef_set_command_button(Cpu& c) {
    hle::EditField* f = field(c.arg32(0));
    int slot = (int)c.arg32(1) ? 1 : 0; // LEFT / RIGHT
    if (f) {
        f->command[slot] = hle::osp_string_get(c, c.arg32(2));
        f->action_id[slot] = (int)c.arg32(3);
        f->has_command[slot] = true;
        printf("[ui] EditField 0x%x button %d = \"%s\" action %d\n", f->obj, slot,
               f->command[slot].c_str(), f->action_id[slot]);
    }
    c.set_ret32(0);
}
// Control::SetShowState(bool) / Show(): a hidden field leaves the overlay.
void ctl_setshowstate(Cpu& c) {
    if (hle::EditField* f = field(c.arg32(0))) {
        f->visible = c.arg32(1) != 0;
    }
    c.set_ret32(0);
}
void ctl_show(Cpu& c) {
    if (hle::EditField* f = field(c.arg32(0))) {
        f->visible = true;
    }
    c.set_ret32(0);
}
void ret_ok(Cpu& c) { c.set_ret32(0); }

} // namespace

namespace hle {

std::vector<EditField*> ui_fields() {
    std::vector<EditField*> v;
    for (uint32_t o : g_order) {
        if (EditField* f = field(o)) {
            v.push_back(f);
        }
    }
    return v;
}

bool ui_active() {
    for (auto& [o, f] : g_fields) {
        if (f.visible) {
            return true;
        }
    }
    return false;
}

void ui_set_text(Cpu& cpu, EditField& f, const std::string& text) {
    if (f.limit > 0 && (int)text.size() > f.limit) {
        return;
    }
    if (f.text == text) {
        return;
    }
    f.text = text;
    call_virtual(cpu, f.text_listener, 2, {f.obj}); // OnTextValueChanged(source)
}

void ui_commit(Cpu& cpu, EditField& f, int slot) {
    f.visible = false;
    printf("[ui] EditField 0x%x commit \"%s\" (action %d)\n", f.obj, f.text.c_str(),
           f.action_id[slot]);
    call_virtual(cpu, f.action_listener, 2, {f.obj, (uint32_t)f.action_id[slot]});
}

void ui_cancel(Cpu& cpu, EditField& f) {
    f.visible = false;
    call_virtual(cpu, f.text_listener, 3, {f.obj}); // OnTextValueChangeCanceled
}

void register_ui() {
    register_handler("_ZN3Osp2Ui8Controls9EditFieldC1Ev", ef_ctor);
    register_handler("_ZN3Osp2Ui8Controls9EditFieldC2Ev", ef_ctor);
    register_handler(
        "_ZN3Osp2Ui8Controls9EditField9ConstructERKNS_8Graphics9RectangleENS1_14EditFieldStyleENS1_"
        "10InputStyleEbiNS1_10GroupStyleE",
        ef_construct);
    register_handler("_ZN3Osp2Ui8Controls9EditField7SetTextERKNS_4Base6StringE", ef_settext);
    register_handler("_ZNK3Osp2Ui8Controls9EditField7GetTextEv", ef_gettext);
    register_handler("_ZNK3Osp2Ui8Controls9EditField13GetTextLengthEv", ef_gettextlength);
    register_handler("_ZN3Osp2Ui8Controls9EditField10ShowKeypadEv", ef_showkeypad);
    register_handler("_ZN3Osp2Ui8Controls9EditField20AddTextEventListenerERKNS0_18ITextEventListenerE",
                     ef_add_text_listener);
    register_handler(
        "_ZN3Osp2Ui8Controls9EditField22AddActionEventListenerERKNS0_20IActionEventListenerE",
        ef_add_action_listener);
    register_handler("_ZN3Osp2Ui8Controls9EditField29SetOverlayKeypadCommandButtonENS1_"
                     "21CommandButtonPositionERKNS_4Base6StringEi",
                     ef_set_command_button);
    register_handler("_ZN3Osp2Ui8Controls11ScrollPanel18CloseOverlayWindowEv", panel_close_overlay);
    register_handler("_ZN3Osp2Ui7Control12SetShowStateEb", ctl_setshowstate);
    register_handler("_ZN3Osp2Ui7Control4ShowEv", ctl_show);
    register_handler("_ZN3Osp2Ui7Control4DrawEv", ret_ok); // host-drawn: nothing to do
    register_handler("_ZN3Osp2Ui7Control8SetFocusEv", ret_ok);
    register_handler("_ZN3Osp2Ui7Control10SetEnabledEb", ret_ok);
}

} // namespace hle
