#include "cpu.h"
#include "dispatch.h"
#include "gl_api.h"
#include "guest.h"
#include <SDL2/SDL.h>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <deque>
#include <map>
#include <string>
#include <vector>

// Osp framework HLE. Most of the 108 Osp::* methods still fall through to the
// default stub; the ones implemented here build *real* guest objects so callers
// see valid state instead of zeros.

namespace {

// Osp::Base::String layout (inherits Osp::Base::Object, which has a vtable):
//   +0  vptr        (Object vtable)
//   +4  __capacity  (int)
//   +8  __length    (int)
//   +12 __pValue    (mchar* = wchar_t*, 2-byte UTF-16, NUL-terminated)
// Size 16. mchar is 2 bytes (Tag_ABI_PCS_wchar_t: 2).
constexpr uint32_t STR_VPTR = 0, STR_CAP = 4, STR_LEN = 8, STR_PVAL = 12;
int g_canvas_width = 480, g_canvas_height = 640;
uint32_t g_orientation = 1;
bool g_frame_oriented = false;
std::map<uint32_t, uint32_t> g_orientation_listeners;
struct OrientationEvent { uint32_t source, listener, orientation; };
std::deque<OrientationEvent> g_orientation_events;
uint32_t g_canvas_buffer = 0;

void canvas_lock(Cpu& c) {
    if (!g_canvas_buffer) {
        g_canvas_buffer = c.alloc(g_canvas_width * g_canvas_height * 4, 4);
        if (g_canvas_buffer) {
            std::vector<uint8_t> clear((size_t)g_canvas_width * g_canvas_height * 4, 0);
            c.write_mem(g_canvas_buffer, clear.data(), (uint32_t)clear.size());
        }
    }
    uint32_t info = c.arg32(1);
    if (info && g_canvas_buffer) {
        c.write_u32(info + 4, g_canvas_width);
        c.write_u32(info + 8, g_canvas_height);
        c.write_u32(info + 12, g_canvas_width * 4);
        c.write_u32(info + 16, 32);
        c.write_u32(info + 20, 2); // PIXEL_FORMAT_ARGB8888
        c.write_u32(info + 24, g_canvas_buffer);
    }
    c.set_ret32(g_canvas_buffer ? 0 : 1);
}

void canvas_unlock(Cpu& c) { c.set_ret32(0); }

void canvas_show(Cpu& c) {
    if (g_canvas_buffer) {
        hle::gl_present_canvas(c, g_canvas_buffer, g_canvas_width, g_canvas_height);
    }
    c.set_ret32(0);
}

// Build a String object at `obj` from a UTF-16 array of `len` mchars (without
// the terminator) already gathered host-side.
void string_construct(Cpu& c, uint32_t obj, const std::vector<uint16_t>& u16) {
    uint32_t len = (uint32_t)u16.size();
    uint32_t cap = len < 16 ? 16 : len + 1;
    uint32_t buf = c.alloc(cap * 2, 2);
    if (len) {
        c.write_mem(buf, u16.data(), len * 2);
    }
    uint16_t nul = 0;
    c.write_mem(buf + len * 2, &nul, 2);
    c.write_u32(obj + STR_VPTR, 0); // TODO: synthesize Object vtable when a virtual call faults
    c.write_u32(obj + STR_CAP, cap);
    c.write_u32(obj + STR_LEN, len);
    c.write_u32(obj + STR_PVAL, buf);
}

// Osp::Base::String::String(wchar_t const*)  -- r0=this, r1=mchar* src
void str_ctor_wchar(Cpu& c) {
    uint32_t obj = c.arg32(0), src = c.arg32(1);
    std::vector<uint16_t> u16;
    if (src) {
        for (;;) {
            uint16_t ch;
            c.read_mem(src + (uint32_t)u16.size() * 2, &ch, 2);
            if (!ch) {
                break;
            }
            u16.push_back(ch);
        }
    }
    string_construct(c, obj, u16);
}

// Osp::Base::String::String(char const*)  -- r0=this, r1=char* src (widen to UTF-16)
void str_ctor_char(Cpu& c) {
    uint32_t obj = c.arg32(0);
    std::string s = c.read_cstr(c.arg32(1));
    std::vector<uint16_t> u16(s.begin(), s.end());
    string_construct(c, obj, u16);
}

// Osp::Graphics::Rectangle: vptr@0 (it has virtuals), x@4, y@8, width@12,
// height@16. Stubbed, its ctor left the object uninitialised and every control
// built from one got a garbage rect.
void rect_ctor4(Cpu& c) {
    uint32_t o = c.arg32(0);
    c.write_u32(o, c.noop_vtable());
    for (int i = 0; i < 4; i++) {
        c.write_u32(o + 4 + i * 4, c.arg32(1 + i));
    }
    c.set_ret32(o);
}
void rect_ctor0(Cpu& c) {
    uint32_t o = c.arg32(0);
    c.write_u32(o, c.noop_vtable());
    for (int i = 0; i < 4; i++) {
        c.write_u32(o + 4 + i * 4, 0);
    }
    c.set_ret32(o);
}

// Read a String object back into UTF-16.
std::vector<uint16_t> string_read(Cpu& c, uint32_t obj) {
    std::vector<uint16_t> u16;
    if (!obj) {
        return u16;
    }
    uint32_t pv = c.read_u32(obj + STR_PVAL), len = c.read_u32(obj + STR_LEN);
    for (uint32_t i = 0; i < len && i < 65536; i++) {
        uint16_t ch;
        c.read_mem(pv + i * 2, &ch, 2);
        u16.push_back(ch);
    }
    return u16;
}

// Osp::Base::String::String()  -- empty string. Leaving this a stub handed the
// caller an uninitialised object, and the garbage __pValue reached Io::File.
void str_ctor_empty(Cpu& c) {
    string_construct(c, c.arg32(0), {});
    c.set_ret32(c.arg32(0));
}
// Osp::Base::String::String(const String&)
void str_ctor_copy(Cpu& c) {
    string_construct(c, c.arg32(0), string_read(c, c.arg32(1)));
    c.set_ret32(c.arg32(0));
}
// Osp::Base::String::Append(const String&) / operator=(const String&)
void str_append(Cpu& c) {
    std::vector<uint16_t> u16 = string_read(c, c.arg32(0));
    std::vector<uint16_t> add = string_read(c, c.arg32(1));
    u16.insert(u16.end(), add.begin(), add.end());
    string_construct(c, c.arg32(0), u16);
    c.set_ret32(0);
}
void str_append_char(Cpu& c) {
    std::vector<uint16_t> u16 = string_read(c, c.arg32(0));
    u16.push_back(static_cast<unsigned char>(c.arg32(1)));
    string_construct(c, c.arg32(0), u16);
    c.set_ret32(0);
}
void str_assign(Cpu& c) {
    string_construct(c, c.arg32(0), string_read(c, c.arg32(1)));
    c.set_ret32(c.arg32(0));
}
void str_clear(Cpu& c) {
    string_construct(c, c.arg32(0), {});
    c.set_ret32(0);
}
void str_getlength(Cpu& c) { c.set_ret32(c.read_u32(c.arg32(0) + STR_LEN)); }
void str_ispointer(Cpu& c) { c.set_ret32(c.read_u32(c.arg32(0) + STR_PVAL)); } // GetPointer()
void str_index(Cpu& c) {
    const uint32_t object = c.arg32(0), index = c.arg32(1);
    c.set_ret32(index < c.read_u32(object + STR_LEN) ? c.read_u32(object + STR_PVAL) + index * 2 : 0);
}

// Osp::Base::String::~String()  -- bump allocator: nothing to free
void str_dtor(Cpu& c) { c.set_ret32(0); }

// --- Osp::Base::Collection::ArrayList (32 bytes) ---
// Built virtually-callable: the bootstrap dispatches Add() through [vtable+16].
// The launch-arg list is not read by OnAppInitializing, so the list is a no-op
// container -- it just needs a valid vtable so virtual calls don't crash.
uint32_t g_arraylist_vt = 0;
void arraylist_ctor(Cpu& c) {
    uint32_t obj = c.arg32(0);
    if (!g_arraylist_vt) {
        g_arraylist_vt = c.make_vtable(40); // all no-op (Add/cleanup safe)
    }
    for (int i = 0; i < 32; i += 4) {
        c.write_u32(obj + i, 0);
    }
    c.write_u32(obj, g_arraylist_vt); // vptr
    c.set_ret32(obj);
}
void arraylist_construct(Cpu& c) { c.set_ret32(0); } // E_SUCCESS; capacity ignored

// --- Osp::App::Application::Execute(factory, IList*, AppAttributeType) ---
// Never returns in real bada (runs the event loop). Here we stash the factory
// and stop emulation; the host loop then drives factory -> OnAppInitializing ->
// per-frame as separate top-level guest calls (no nested uc_emu_start).
void app_execute(Cpu& c) {
    c.exec_factory = c.arg32(0);
    c.exec_args = c.arg32(1);
    printf("[hle] Application::Execute(factory=0x%x, args=0x%x, attr=%u) -> host loop\n",
           c.exec_factory, c.exec_args, c.arg32(2));
    c.request_exit();
    c.set_ret32(0);
}

// --- Generic stubs for not-yet-implemented Osp objects ---
// Any class whose ctor we stub still needs a valid vtable so the guest's virtual
// dispatch (ldr vtable; ldr [vtable+N]; blx) lands on a no-op instead of reading
// a null vtable. stub_ctor installs the shared no-op vtable on `this`.
void stub_ctor(Cpu& c) {
    uint32_t o = c.arg32(0);
    if (o) {
        c.write_u32(o, c.noop_vtable());
    }
    c.set_ret32(o);
}
void ret_ok(Cpu& c) { c.set_ret32(0); }   // E_SUCCESS
void ret_true(Cpu& c) { c.set_ret32(1); } // bool true

// --- Osp::Media::Player (video): no real decode. Drive a tiny state machine so
// apps that gate progress on playback completion (Asphalt's intro trailer) see
// the clip play briefly and reach end-of-clip, rather than looping forever on a
// player that never finishes. Osp::Media::PlayerState: INITIALIZED=0, OPENING=1,
// OPENED=2, ENDOFCLIP=3, STOPPED=4, PAUSED=5, PLAYING=6, CLOSED=7, ERROR=8.
struct VidPlayer {
    int state = 0;                                // PlayerState
    std::chrono::steady_clock::time_point t_play; // when Play() was called
    bool playing = false;
    bool ended = false;    // OnPlayerEndOfClip already fired
    uint32_t listener = 0; // IPlayerEventListener `this` (from Construct)
    uint32_t end_cb = 0;   // listener's OnPlayerEndOfClip address
};
std::map<uint32_t, VidPlayer> g_players;
constexpr int kClipMs = 300; // pretend every clip is this long, then end

VidPlayer& player(Cpu& c) { return g_players[c.arg32(0)]; }
// Player::Construct(IPlayerEventListener& listener, BufferInfo*): capture the
// listener and resolve its OnPlayerEndOfClip so the pump can signal the end.
void player_construct(Cpu& c) {
    VidPlayer& p = player(c);
    p.listener = c.arg32(1);
    p.end_cb = c.img ? c.img->resolve_contains("17OnPlayerEndOfClipEv") : 0;
    c.set_ret32(0);
}
int player_elapsed_ms(const VidPlayer& p) {
    return (int)std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - p.t_play)
        .count();
}
void player_ctor(Cpu& c) {
    uint32_t o = c.arg32(0);
    if (o) {
        c.write_u32(o, c.noop_vtable());
    }
    g_players[o] = VidPlayer{};
    c.set_ret32(o);
}
void player_openfile(Cpu& c) {
    player(c).state = 2; // OPENED
    c.set_ret32(0);
}
void player_play(Cpu& c) {
    VidPlayer& p = player(c);
    p.state = 6; // PLAYING
    p.playing = true;
    p.ended = false;
    p.t_play = std::chrono::steady_clock::now();
    c.set_ret32(0);
}
void player_getstate(Cpu& c) {
    VidPlayer& p = player(c);
    if (p.playing && player_elapsed_ms(p) >= kClipMs) {
        p.state = 3; // ENDOFCLIP
    }
    c.set_ret32((uint32_t)p.state);
}
void player_getduration(Cpu& c) { c.set_ret32(kClipMs); }
void player_getposition(Cpu& c) {
    VidPlayer& p = player(c);
    int pos = p.playing ? player_elapsed_ms(p) : 0;
    c.set_ret32((uint32_t)(pos > kClipMs ? kClipMs : pos));
}
void player_stop(Cpu& c) {
    VidPlayer& p = player(c);
    p.state = 4; // STOPPED
    p.playing = false;
    c.set_ret32(0);
}
void player_close(Cpu& c) {
    VidPlayer& p = player(c);
    p.state = 7; // CLOSED -- BVideoPlayer::UpdateVideo gates the trailer exit on
                 // GetState()==CLOSED after it calls Close() in the ENDOFCLIP case
    p.playing = false;
    c.set_ret32(0);
}

// SystemTime::GetTicks(long long& out) -- monotonic ms since start (static, so
// r0 = &out). Apps drive animation/physics off this; a frozen 0 stalls them.
void systemtime_getticks(Cpu& c) {
    static const auto t0 = std::chrono::steady_clock::now();
    int64_t ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                     std::chrono::steady_clock::now() - t0)
                     .count();
    uint32_t out = c.arg32(0);
    if (out) {
        c.write_mem(out, &ms, 8);
    }
    c.set_ret32(0);
}

// --- Osp::Base::DateTime / TimeSpan ---
// TimeSpan has a virtual dtor, so: vptr@0, pad, __ticks@8 (ms). DateTime holds a
// TmDateTime; the games only build one and read GetTime() off it, so back it
// with the host clock and keep the guest side opaque.
constexpr uint32_t TS_TICKS = 8;
void datetime_ctor(Cpu& c) {
    uint32_t o = c.arg32(0);
    if (o) {
        c.write_u32(o, c.noop_vtable());
    }
    c.set_ret32(o);
}
// DateTime::GetTime() const -> TimeSpan by value: r0 = &ret, r1 = this.
void datetime_gettime(Cpu& c) {
    uint32_t ret = c.arg32(0);
    time_t now = time(nullptr);
    struct tm t {};
#ifdef _WIN32
    localtime_s(&t, &now);
#else
    localtime_r(&now, &t);
#endif
    int64_t ms = ((int64_t)t.tm_hour * 3600 + t.tm_min * 60 + t.tm_sec) * 1000;
    c.write_u32(ret, c.noop_vtable());
    c.write_u32(ret + TS_TICKS, (uint32_t)ms);
    c.write_u32(ret + TS_TICKS + 4, (uint32_t)(ms >> 32));
    c.set_ret32(ret);
}
void timespan_getms(Cpu& c) {
    uint32_t o = c.arg32(0);
    c.set_ret64((uint64_t)c.read_u32(o + TS_TICKS) |
                ((uint64_t)c.read_u32(o + TS_TICKS + 4) << 32));
}

// Application::GetAppFrame() -> IAppFrame whose GetFrame() (vtable slot 9) returns
// a Frame (Container). Frame::AddControl (slot 24) and AppFrame's other slots fall
// through to the no-op vtable, returning 0 == E_SUCCESS.
uint32_t g_appframe = 0, g_frame = 0;
void frame_getframe(Cpu& c) { c.set_ret32(g_frame); }
// Application::GetInstance() -- the app object the factory built. Returning 0
// here hands the caller a null `this`.
void app_getinstance(Cpu& c) { c.set_ret32(c.app_instance); }

// Control::SendUserEvent(long requestId, const IList* pArgs) -- bada posts the
// request to the UI event loop and the app's OnUserEventReceivedN picks it up on
// a later turn. Queue it here; user_event_pump delivers it from the frame loop.
// PvZ starts its whole resource load this way (TitleScreen::Update ->
// SendStartLoading -> SendUserEvent(0x22b)), so a no-op stub left it on a black
// screen forever.
// Control::AddTouchEventListener(const ITouchEventListener&) -- remember the
// listener and the control that owns it, so the host can deliver touches the
// way bada does: straight through the listener's vtable. This is the portable
// route; the GlesForm hook in main.cpp only knows Fruit Ninja's own class.
//
// ITouchEventListener derives from IEventListener, whose only virtual is its
// destructor, so the vtable is:
//   0 ~D1, 1 ~D0, 2 OnTouchPressed, 3 OnTouchLongPressed, 4 OnTouchReleased,
//   5 OnTouchMoved, 6 OnTouchDoublePressed, 7 OnTouchFocusIn, 8 OnTouchFocusOut
uint32_t g_touch_listener = 0, g_touch_source = 0;
std::map<uint32_t, bool> g_touch_multipoint;
std::map<uint32_t, uint32_t> g_touch_event_ids;
std::map<uint32_t, std::pair<int, int>> g_touch_event_positions;
void touch_setmultipoint(Cpu& c) {
    g_touch_multipoint[c.arg32(1)] = c.arg32(2) != 0;
    c.set_ret32(0);
}
void touch_ismultipoint(Cpu& c) {
    const auto found = g_touch_multipoint.find(c.arg32(1));
    c.set_ret32(found != g_touch_multipoint.end() && found->second);
}
void touch_getpointid(Cpu& c) {
    const auto found = g_touch_event_ids.find(c.arg32(0));
    c.set_ret32(found == g_touch_event_ids.end() ? 0 : found->second);
}
void touch_getposition(Cpu& c) {
    const uint32_t output = c.arg32(0);
    const auto position = g_touch_event_positions.find(c.arg32(1));
    c.write_u32(output, c.noop_vtable());
    c.write_u32(output + 4, position == g_touch_event_positions.end() ? 0 : position->second.first);
    c.write_u32(output + 8, position == g_touch_event_positions.end() ? 0 : position->second.second);
    c.set_ret32(output);
}
void control_addtouchlistener(Cpu& c) {
    if (!g_touch_listener) {
        g_touch_source = c.arg32(0);
        g_touch_listener = c.arg32(1);
        printf("[touch] listener @ 0x%x on control 0x%x\n", g_touch_listener, g_touch_source);
    }
    c.set_ret32(0);
}

struct UserEvent {
    uint32_t id;
    uint32_t args;
};
std::deque<UserEvent> g_user_events;
void control_senduserevent(Cpu& c) {
    g_user_events.push_back({c.arg32(1), c.arg32(2)});
    c.set_ret32(0);
}
void app_getappframe(Cpu& c) {
    if (!g_appframe) {
        g_frame = c.make_stub_object(256, 64);                  // the Frame/Container
        uint32_t vt = c.make_vtable(64, {{9, frame_getframe}}); // AppFrame: slot 9 = GetFrame
        g_appframe = c.alloc(64, 8);
        for (int i = 0; i < 64; i += 4) {
            c.write_u32(g_appframe + i, 0);
        }
        c.write_u32(g_appframe, vt);
    }
    c.set_ret32(g_appframe);
}

// --- System / Locales (language detection) ---
// Osp::Locales::Locale::GetLanguageCode()/GetCountryCode() return *enum values*
// (Osp::Locales::LanguageCode / CountryCode), NOT strings -- apps switch on the
// int (e.g. FruitNinja: GetLanguageCode()==0x88 -> French). The reported locale
// defaults to English/US; hle::set_locale overrides it (from --locale / env).
// `g_locale_*_str` is just for logging.
int g_locale_lang_enum = 0x7a;    // LANGUAGE_ENG
int g_locale_country_enum = 0xe1; // COUNTRY_US
std::string g_locale_lang = "eng";
std::string g_locale_country = "US";

// SettingInfo::GetValue(String key, bool& out) -> write false (e.g. SilentMode off)
void settinginfo_getvalue_bool(Cpu& c) {
    c.write_u32(c.arg32(1), 0);
    c.set_ret32(0);
}
void runtimeinfo_getvalue_int(Cpu& c) {
    const auto units = string_read(c, c.arg32(0));
    const std::string key(units.begin(), units.end());
    uint32_t value;
    if (key == "AllocatedMemory") value = c.allocated_memory();
    else if (key == "AvailableMemory") value = guest::HEAP_SIZE - c.allocated_memory();
    else { c.set_ret32(0xa0000000u + 1909); return; }
    c.write_u32(c.arg32(1), value);
    c.set_ret32(0);
}
void systeminfo_getvalue_string(Cpu& c) {
    const auto units = string_read(c, c.arg32(0));
    const std::string key(units.begin(), units.end());
    if (key != "ModelName") { c.set_ret32(0xa0000000u + 1909); return; }
    const std::string model = "GT-S8500";
    string_construct(c, c.arg32(1), std::vector<uint16_t>(model.begin(), model.end()));
    c.set_ret32(0);
}
// LocaleManager::GetSystemLocale() -> returns Locale by value (sret: r0=&ret)
void locale_get_system(Cpu& c) {
    uint32_t ret = c.arg32(0);
    c.write_u32(ret, c.noop_vtable());
    c.set_ret32(ret);
}
// Locale::GetLanguageCode()/GetCountryCode() const -> LanguageCode/CountryCode
// enum value in r0 (this scalar return, not the sret String form).
void locale_lang(Cpu& c) { c.set_ret32((uint32_t)g_locale_lang_enum); }
void locale_country(Cpu& c) { c.set_ret32((uint32_t)g_locale_country_enum); }

// --- Timer: host drives OnTimerExpired; Construct stashes the timer, Start arms it ---
void timer_construct(Cpu& c) {
    c.timer_obj = c.arg32(0);
    c.set_ret32(0);
}
// Timer::Start(msec) is one-shot: the app re-arms it at the end of every
// OnTimerExpired with the time left in its own period. Honour that interval --
// driving OnTimerExpired once per rendered frame instead runs the app's whole
// clock at the host's frame rate (PvZ asks for 100 ms and ran ~6x too fast).
void timer_start(Cpu& c) {
    c.timer_active = true;
    uint32_t iv = c.arg32(1);
    static bool said = false;
    if (!said) { // the app's target rate. It re-arms with the time left in its
        said = true; // period after that, so the value drifts every tick.
        printf("[osp] Timer::Start(%u ms) -> the app wants %.1f ticks/s\n", iv,
               iv ? 1000.0 / iv : 0.0);
    }
    c.timer_interval_ms = iv;
    c.timer_deadline_ms = SDL_GetTicks() + std::max(iv, 1u);
    c.set_ret32(0);
}
void timer_cancel(Cpu& c) {
    c.timer_active = false;
    c.set_ret32(0);
}

// Control::GetBounds(int& x, int& y, int& w, int& h) -- report the form's full
// (device) size so Draw()'s glViewport and aspect ratio are correct.
// Control::GetBounds returns the portrait device panel (480x800). The binary's
// InitGL swaps these into glViewport(0,0,800,480) -- it renders landscape from
// portrait bounds -- so this must stay 480x800 (confirmed by RE).
void control_getbounds(Cpu& c) {
    c.write_u32(c.arg32(1), 0);
    c.write_u32(c.arg32(2), 0); // x, y
    c.write_u32(c.arg32(3), g_frame_oriented ? g_canvas_width : 480);
    c.write_u32(c.arg32(4), g_frame_oriented ? g_canvas_height : 800); // width, height
    c.set_ret32(0);
}
void frame_setorientation(Cpu& c) {
    const uint32_t orientation = c.arg32(1);
    if (orientation < 1 || orientation > 4) { c.set_ret32(0xa0000000u + 1909); return; }
    if (!g_frame_oriented || g_orientation != orientation) {
        g_frame_oriented = true;
        g_orientation = orientation;
        g_canvas_width = orientation == 2 || orientation == 4 ? 800 : 480;
        g_canvas_height = orientation == 2 || orientation == 4 ? 480 : 800;
        c.free(g_canvas_buffer);
        g_canvas_buffer = 0;
        hle::gl_set_surface_size(g_canvas_width, g_canvas_height);
        for (const auto& entry : g_orientation_listeners)
            g_orientation_events.push_back({entry.first, entry.second, orientation});
    }
    c.set_ret32(0);
}

} // namespace

namespace hle {
void register_osp() {
    g_orientation = 1;
    g_frame_oriented = false;
    g_canvas_width = 480;
    g_canvas_height = 640;
    g_canvas_buffer = 0;
    g_orientation_events.clear();
    g_orientation_listeners.clear();
    g_touch_multipoint.clear();
    g_touch_event_ids.clear();
    g_touch_event_positions.clear();
    g_touch_listener = g_touch_source = 0;
    register_handler("_ZN3Osp4Base6StringC1EPKw", str_ctor_wchar); // String(wchar_t const*)
    register_handler("_ZN3Osp4Base6StringC2EPKw", str_ctor_wchar);
    register_handler("_ZN3Osp4Base6StringC1EPKc", str_ctor_char); // String(char const*)
    register_handler("_ZN3Osp4Base6StringC2EPKc", str_ctor_char);
    register_handler("_ZN3Osp4Base6StringC1Ev", str_ctor_empty); // String()
    register_handler("_ZN3Osp4Base6StringC2Ev", str_ctor_empty);
    register_handler("_ZNK3Osp4Base6StringixEi", str_index);
    register_handler("_ZNK3Osp4Base6String7IsEmptyEv", [](Cpu& c) { c.set_ret32(c.read_u32(c.arg32(0) + STR_LEN) == 0); });
    register_handler("_ZN3Osp4Base6StringC1ERKS1_", str_ctor_copy); // String(const String&)
    register_handler("_ZN3Osp4Base6StringC2ERKS1_", str_ctor_copy);
    register_handler("_ZN3Osp4Base6String6AppendERKS1_", str_append);
    register_handler("_ZN3Osp4Base6String6AppendEc", str_append_char);
    register_handler("_ZN3Osp4Base6StringaSERKS1_", str_assign);
    register_handler("_ZN3Osp4Base6String5ClearEv", str_clear);
    register_handler("_ZNK3Osp4Base6String9GetLengthEv", str_getlength);
    register_handler("_ZNK3Osp4Base6String10GetPointerEv", str_ispointer);
    register_handler("_ZN3Osp4Base6StringD1Ev", str_dtor); // ~String()
    register_handler("_ZN3Osp4Base6StringD2Ev", str_dtor);

    register_handler("_ZN3Osp4Base10Collection9ArrayListC1Ev", arraylist_ctor);
    register_handler("_ZN3Osp4Base10Collection9ArrayListC2Ev", arraylist_ctor);
    register_handler("_ZN3Osp4Base10Collection9ArrayList9ConstructEi", arraylist_construct);

    // Application::Execute(Application*(*)(), IList const*, AppAttributeType)
    register_handler("_ZN3Osp3App11Application7ExecuteEPFPS1_vEPKNS_4Base10Collection5IListENS0_16AppAttributeTypeE", app_execute);

    // App + Ui::Controls::Form -- stub objects with a valid (no-op) vtable.
    register_handler("_ZN3Osp3App11ApplicationC2Ev", stub_ctor); // Application()
    register_handler("_ZN3Osp3App11ApplicationC1Ev", stub_ctor);
    register_handler("_ZNK3Osp3App11Application11GetAppFrameEv", app_getappframe);
    register_handler("_ZN3Osp3App11Application11GetInstanceEv", app_getinstance);
    register_handler("_ZNK3Osp2Ui7Control13SendUserEventElPKNS_4Base10Collection5IListE",
                     control_senduserevent);
    register_handler("_ZN3Osp3App11Application13SendUserEventElPKNS_4Base10Collection5IListE",
                     control_senduserevent);
    register_handler("_ZN3Osp8Graphics9RectangleC1Eiiii", rect_ctor4);
    register_handler("_ZN3Osp8Graphics9RectangleC2Eiiii", rect_ctor4);
    register_handler("_ZN3Osp8Graphics9RectangleC1Ev", rect_ctor0);
    register_handler("_ZN3Osp8Graphics9RectangleC2Ev", rect_ctor0);
    register_handler("_ZN3Osp8Graphics9RectangleD1Ev", str_dtor);
    register_handler("_ZN3Osp8Graphics9RectangleD2Ev", str_dtor);
    register_handler("_ZN3Osp4Base8DateTimeC1Ev", datetime_ctor);
    register_handler("_ZN3Osp4Base8DateTimeC2Ev", datetime_ctor);
    register_handler("_ZNK3Osp4Base8DateTime7GetTimeEv", datetime_gettime);
    register_handler("_ZNK3Osp4Base8TimeSpan15GetMillisecondsEv", timespan_getms);
    register_handler("_ZN3Osp4Base8TimeSpanD1Ev", str_dtor);
    register_handler("_ZN3Osp4Base8TimeSpanD2Ev", str_dtor);
    register_handler("_ZN3Osp2Ui8Controls4FormC1Ev", stub_ctor); // Form()
    register_handler("_ZN3Osp2Ui8Controls4FormC2Ev", stub_ctor);
    register_handler("_ZN3Osp2Ui8Controls4Form9ConstructEm", ret_ok); // Form::Construct(ulong)

    // Container::AddControl(Control const&) -- also called directly (import).
    register_handler("_ZN3Osp2Ui9Container10AddControlERKNS_7ControlE", ret_ok);

    // Timer (Base::Runtime)
    register_handler("_ZN3Osp4Base7Runtime5TimerC1Ev", stub_ctor); // Timer()
    register_handler("_ZN3Osp4Base7Runtime5TimerC2Ev", stub_ctor);
    register_handler("_ZN3Osp4Base7Runtime5Timer9ConstructERKNS1_19ITimerEventListenerE", timer_construct);
    register_handler("_ZN3Osp4Base7Runtime5Timer5StartEi", timer_start);
    register_handler("_ZN3Osp4Base7Runtime5Timer6CancelEv", timer_cancel);

    // Control::GetBounds(int&,int&,int&,int&) const -- viewport size for Draw()
    register_handler("_ZNK3Osp2Ui7Control9GetBoundsERiS2_S2_S2_", control_getbounds);
    // Form::SetOrientation(Orientation) -- FruitNinja requests LANDSCAPE; the
    // engine does its own rotation, so this only affects input mapping: no-op.
    register_handler("_ZN3Osp2Ui8Controls4Form14SetOrientationENS0_11OrientationE", ret_ok);
    register_handler("_ZN3Osp2Ui8Controls5Frame14SetOrientationENS0_11OrientationE", frame_setorientation);
    register_handler("_ZN3Osp2Ui8Controls4Form20GetOrientationStatusEv", [](Cpu& c) { c.set_ret32(g_orientation); });
    register_handler("_ZN3Osp2Ui8Controls4Form27AddOrientationEventListenerERKNS0_25IOrientationEventListenerE", [](Cpu& c) {
        g_orientation_listeners[c.arg32(0)] = c.arg32(1);
        c.set_ret32(0);
    });

    // Osp::Ui::Touch + Control touch-listener APIs (Asphalt 5 registers a Touch
    // listener and requires multipoint; FruitNinja drives touch via the GlesForm
    // listener instead). Preserve the control's setting and return each
    // callback's contact ID instead of unconditional success/zero stubs.
    register_handler("_ZN3Osp2Ui5TouchC1Ev", stub_ctor); // Touch()
    register_handler("_ZN3Osp2Ui5TouchC2Ev", stub_ctor);
    register_handler("_ZN3Osp2Ui7Control21AddTouchEventListenerERKNS0_19ITouchEventListenerE",
                     control_addtouchlistener);
    register_handler("_ZN3Osp2Ui5Touch20SetMultipointEnabledERKNS0_7ControlEb", touch_setmultipoint);
    register_handler("_ZNK3Osp2Ui5Touch19IsMultipointEnabledERKNS0_7ControlE", touch_ismultipoint);
    register_handler("_ZNK3Osp2Ui14TouchEventInfo10GetPointIdEv", touch_getpointid);
    register_handler("_ZNK3Osp2Ui14TouchEventInfo18GetCurrentPositionEv", touch_getposition);

    // Workers park at Sleep/Yield/Join or a semaphore wait, allowing the host
    // to deliver UI events before restoring their ARM/VFP context.
    register_handler("_ZN3Osp6System10SystemTime8GetTicksERx", systemtime_getticks);
    register_handler("_ZN3Osp4Base7Runtime6ThreadC1Ev", stub_ctor);
    register_handler("_ZN3Osp4Base7Runtime6ThreadC2Ev", stub_ctor);
    register_handler("_ZN3Osp4Base7Runtime6Thread9ConstructERNS1_9IRunnableElNS1_14ThreadPriorityE", [](Cpu& c) {
        c.set_ret32(c.construct_thread(c.arg32(0), c.arg32(1), c.arg32(2)) ? 0 : 0xa0000000u + 1909);
    });
    register_handler("_ZN3Osp4Base7Runtime6Thread5StartEv", [](Cpu& c) {
        c.set_ret32(c.start_thread(c.arg32(0)) ? 0 : 0xa0000000u + 1926);
    });
    register_handler("_ZN3Osp4Base7Runtime6Thread4JoinEv", [](Cpu& c) { c.set_ret32(c.join_thread(c.arg32(0)) ? 0 : 0xa0000000u + 1926); });
    register_handler("_ZN3Osp4Base7Runtime6ThreadD1Ev", [](Cpu& c) { c.destroy_thread(c.arg32(0)); c.set_ret32(0); });
    register_handler("_ZN3Osp4Base7Runtime6ThreadD2Ev", [](Cpu& c) { c.destroy_thread(c.arg32(0)); c.set_ret32(0); });
    register_handler("_ZN3Osp4Base7Runtime6Thread16GetCurrentThreadEv", [](Cpu& c) { c.set_ret32(c.current_thread()); });
    register_handler("_ZN3Osp4Base7Runtime6Thread5SleepEl", [](Cpu& c) {
        const int32_t ms = c.arg32(0);
        c.set_ret32(0);
        c.sleep_thread(std::max(0, ms));
    });
    register_handler("_ZN3Osp4Base7Runtime6Thread5YieldEv", [](Cpu& c) { c.set_ret32(0); c.sleep_thread(0); });
    register_handler("_ZN3Osp4Base7Runtime6Thread4ExitEi", [](Cpu& c) { c.exit_thread(); });
    register_handler("_ZN3Osp4Base7Runtime9SemaphoreC1Ev", stub_ctor);
    register_handler("_ZN3Osp4Base7Runtime9SemaphoreC2Ev", stub_ctor);
    register_handler("_ZN3Osp4Base7Runtime9Semaphore6CreateEi", [](Cpu& c) {
        if (static_cast<int32_t>(c.arg32(1)) < 0) { c.set_ret32(0xa0000000u + 1909); return; }
        c.create_semaphore(c.arg32(0), c.arg32(1)); c.set_ret32(0);
    });
    register_handler("_ZNK3Osp4Base7Runtime9Semaphore7AcquireEl", [](Cpu& c) {
        c.set_ret32(c.acquire_semaphore(c.arg32(0), c.arg32(1)) ? 0 : 0xa0000000u + 3001);
    });
    register_handler("_ZNK3Osp4Base7Runtime9Semaphore7ReleaseEv", [](Cpu& c) { c.release_semaphore(c.arg32(0)); c.set_ret32(0); });

    // Osp::Media::Player (no-op video that reports a short clip then end-of-clip).
    register_handler("_ZN3Osp5Media6PlayerC1Ev", player_ctor);
    register_handler("_ZN3Osp5Media6PlayerC2Ev", player_ctor);
    register_handler("_ZN3Osp5Media6Player9ConstructERKNS0_20IPlayerEventListenerEPKNS_8Graphics10BufferInfoE",
                     player_construct);
    register_handler("_ZN3Osp5Media6Player8OpenFileERKNS_4Base6StringEb", player_openfile);
    register_handler("_ZN3Osp5Media6Player4PlayEv", player_play);
    register_handler("_ZN3Osp5Media6Player4StopEv", player_stop);
    register_handler("_ZN3Osp5Media6Player5PauseEv", ret_ok);
    register_handler("_ZN3Osp5Media6Player5CloseEv", player_close);
    register_handler("_ZN3Osp5Media6Player6SeekToEl", ret_ok);
    register_handler("_ZN3Osp5Media6Player7SetMuteEb", ret_ok);
    register_handler("_ZN3Osp5Media6Player9SetVolumeEi", ret_ok);
    register_handler("_ZNK3Osp5Media6Player8GetStateEv", player_getstate);
    register_handler("_ZNK3Osp5Media6Player11GetDurationEv", player_getduration);
    register_handler("_ZNK3Osp5Media6Player11GetPositionEv", player_getposition);

    // PowerManager::SetScreenEventListener(IScreenEventListener&)
    register_handler("_ZN3Osp6System12PowerManager22SetScreenEventListenerERNS_24IScreenEventListenerE", ret_ok);
    register_handler("_ZN3Osp6System12PowerManager17KeepScreenOnStateEbb", ret_ok);
    register_handler("_ZN3Osp6System11SettingInfo8GetValueERKNS_4Base6StringERb", settinginfo_getvalue_bool);
    register_handler("_ZN3Osp6System11RuntimeInfo8GetValueERKNS_4Base6StringERi", runtimeinfo_getvalue_int);
    register_handler("_ZN3Osp6System10SystemInfo8GetValueERKNS_4Base6StringERS3_", systeminfo_getvalue_string);

    // Locales (report English/US)
    register_handler("_ZN3Osp7Locales13LocaleManagerC1Ev", stub_ctor);
    register_handler("_ZN3Osp7Locales13LocaleManagerC2Ev", stub_ctor);
    register_handler("_ZN3Osp7Locales13LocaleManager9ConstructEv", ret_ok);
    register_handler("_ZN3Osp7Locales13LocaleManagerD1Ev", ret_ok);
    register_handler("_ZN3Osp7Locales13LocaleManager15GetSystemLocaleEv", locale_get_system);
    register_handler("_ZNK3Osp7Locales6Locale15GetLanguageCodeEv", locale_lang);
    register_handler("_ZNK3Osp7Locales6Locale14GetCountryCodeEv", locale_country);
    register_handler("_ZN3Osp7Locales6LocaleD1Ev", ret_ok);

    register_handler("_ZN3Osp8Graphics6Canvas4LockERNS0_10BufferInfoEl", canvas_lock);
    register_handler("_ZN3Osp8Graphics6Canvas6UnlockEv", canvas_unlock);
    register_handler("_ZN3Osp8Graphics6Canvas4ShowEv", canvas_show);
}

// Override the system locale reported to the guest (from --locale / BADA_LOCALE).
// `spec` is "lang" or "lang_COUNTRY" / "lang-COUNTRY". The language part accepts
// either bada's 3-letter ISO 639-2 code (e.g. "fre") or a 2-letter ISO 639-1 code
// (e.g. "fr"), which is mapped to 3-letter. Empty spec leaves the default.
void set_locale(const std::string& spec) {
    if (spec.empty()) {
        return;
    }
    std::string lang = spec, country;
    if (auto sep = spec.find_first_of("_-"); sep != std::string::npos) {
        lang = spec.substr(0, sep);
        country = spec.substr(sep + 1);
    }
    for (char& ch : lang) {
        ch = (char)tolower((unsigned char)ch);
    }
    static const std::map<std::string, std::string> iso2to3 = {
        {"en", "eng"}, {"fr", "fre"}, {"de", "ger"}, {"es", "spa"}, {"it", "ita"},
        {"pt", "por"}, {"nl", "dut"}, {"ru", "rus"}, {"ja", "jpn"}, {"ko", "kor"},
        {"zh", "chi"}, {"pl", "pol"}, {"sv", "swe"}, {"da", "dan"}, {"no", "nor"},
        {"fi", "fin"}, {"tr", "tur"}, {"ar", "ara"}, {"el", "gre"}, {"cs", "cze"},
        {"hu", "hun"}, {"th", "tha"}, {"id", "ind"}, {"vi", "vie"}, {"nb", "nor"}};
    if (lang.size() == 2) {
        if (auto it = iso2to3.find(lang); it != iso2to3.end()) {
            lang = it->second;
        }
    }
    g_locale_lang = lang;
    if (!country.empty()) {
        for (char& ch : country) {
            ch = (char)toupper((unsigned char)ch);
        }
        g_locale_country = country;
    }
    // Map to the bada Osp::Locales::LanguageCode / CountryCode enum values that
    // GetLanguageCode()/GetCountryCode() return (FLclLocale.h: LANGUAGE_* indexed
    // from LANGUAGE_AAR=0, COUNTRY_* from COUNTRY_AF=0). bada uses ISO 639-2/T
    // codes (fra/deu/zho, not fre/ger/chi); B-code aliases are accepted too.
    // Unmapped codes fall back to English/US.
    static const std::map<std::string, int> kLang = {
        {"ara", 20},  {"zho", 78},  {"chi", 78},  {"ces", 100}, {"cze", 100},
        {"dan", 102}, {"nld", 115}, {"dut", 115}, {"eng", 122}, {"fin", 133},
        {"fra", 136}, {"fre", 136}, {"deu", 149}, {"ger", 149}, {"ell", 163},
        {"gre", 163}, {"heb", 172}, {"hin", 176}, {"hun", 182}, {"ind", 195},
        {"ita", 201}, {"jpn", 204}, {"kor", 230}, {"msa", 273}, {"may", 273},
        {"nor", 315}, {"fas", 341}, {"per", 341}, {"pol", 345}, {"por", 347},
        {"ron", 359}, {"rum", 359}, {"rus", 362}, {"slk", 384}, {"slo", 384},
        {"spa", 400}, {"swe", 412}, {"tha", 425}, {"tur", 443}, {"ukr", 451},
        {"vie", 458}};
    static const std::map<std::string, int> kCountry = {
        {"US", 225}, {"GB", 224}, {"CA", 38},  {"AU", 13},  {"FR", 73},
        {"DE", 80},  {"IT", 105}, {"ES", 198}, {"BR", 30},  {"PT", 172},
        {"RU", 177}, {"CN", 44},  {"TW", 207}, {"HK", 96},  {"JP", 107},
        {"KR", 113}, {"NL", 150}, {"PL", 171}, {"SE", 204}, {"TR", 217},
        {"MX", 138}, {"IN", 99}};
    if (auto it = kLang.find(g_locale_lang); it != kLang.end()) {
        g_locale_lang_enum = it->second;
    }
    if (auto it = kCountry.find(g_locale_country); it != kCountry.end()) {
        g_locale_country_enum = it->second;
    }
    printf("[locale] language=%s(%d) country=%s(%d)\n", g_locale_lang.c_str(), g_locale_lang_enum,
           g_locale_country.c_str(), g_locale_country_enum);
}

// Per frame (main/GL thread): once a no-op clip has "played" for kClipMs, fire
// the player's OnPlayerEndOfClip listener so the app stops waiting and advances
// (Asphalt's intro trailer -> menu). Must run from the loop, not a handler (it
// re-enters the guest).
void player_pump(Cpu& cpu) {
    for (auto& [self, p] : g_players) {
        if (!p.playing || p.ended || !p.listener || !p.end_cb) {
            continue;
        }
        if (player_elapsed_ms(p) >= kClipMs) {
            p.ended = true;
            p.state = 3; // ENDOFCLIP
            uint32_t ret = 0;
            cpu.run_guest(p.end_cb, {p.listener}, ret);
        }
    }
}

void osp_string_set(Cpu& c, uint32_t obj, const std::string& utf8) {
    string_construct(c, obj, std::vector<uint16_t>(utf8.begin(), utf8.end()));
}
std::string osp_string_get(Cpu& c, uint32_t obj) {
    std::vector<uint16_t> u16 = string_read(c, obj);
    return std::string(u16.begin(), u16.end()); // ASCII is all these apps use
}

bool touch_listener(uint32_t& listener, uint32_t& source) {
    listener = g_touch_listener;
    source = g_touch_source;
    return g_touch_listener != 0;
}
bool touch_multipoint_enabled(uint32_t source) {
    const auto found = g_touch_multipoint.find(source);
    return found != g_touch_multipoint.end() && found->second;
}
void touch_event_set(uint32_t object, uint32_t point_id, int x, int y) {
    g_touch_event_ids[object] = point_id;
    g_touch_event_positions[object] = {x, y};
}

// Per frame (main/GL thread): hand queued user events to the app's
// OnUserEventReceivedN(requestId, pArgs) override.
void user_event_pump(Cpu& cpu) {
    while (!g_orientation_events.empty()) {
        const auto event = g_orientation_events.front();
        g_orientation_events.pop_front();
        const uint32_t callback = cpu.read_u32(cpu.read_u32(event.listener) + 8);
        uint32_t result = 0;
        if (callback) cpu.run_guest(callback, {event.listener, event.source, event.orientation}, result);
    }
    if (g_user_events.empty() || !cpu.app_instance) {
        return;
    }
    static uint32_t cb = cpu.img ? cpu.img->resolve_contains("20OnUserEventReceivedN") : 0;
    if (!cb) {
        g_user_events.clear();
        return;
    }
    while (!g_user_events.empty()) {
        UserEvent e = g_user_events.front();
        g_user_events.pop_front();
        uint32_t ret = 0;
        if (getenv("BADA_TRACE_EVENTS")) printf("[osp] user event %u -> OnUserEventReceivedN\n", e.id);
        cpu.run_guest(cb, {cpu.app_instance, e.id, e.args}, ret);
    }
}
} // namespace hle
