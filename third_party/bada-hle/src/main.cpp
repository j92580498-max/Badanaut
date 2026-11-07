#ifndef __ANDROID__
#define SDL_MAIN_HANDLED
#endif
#include <SDL2/SDL.h>
#ifndef __ANDROID__
#include <SDL2/SDL_image.h>
#include <SDL2/SDL_opengl.h>
#else
#include <GLES2/gl2.h>
#include "diagnostic_log.h"
#endif
#include <cctype>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <cxxopts.hpp>
#include <pugixml.hpp>
#include "control.h"
#include "cpu.h"
#include "elf_loader.h"
#include "guest.h"
#include "hle/dispatch.h"
#include "hle/gl_api.h"
#include "ui_overlay.h"
namespace fs = std::filesystem;

// ---- stdin command interface (for scripted control: screenshots, quit) ----
// A reader thread blocks on std::cin and queues whole lines; the frame loop
// drains them without blocking. Commands: "screenshot [path]" / "shot [path]",
// "quit". Lets a driver capture the live framebuffer deterministically.
namespace {
std::mutex g_cmd_mtx;
std::deque<std::string> g_cmds;

void stdin_reader() {
    std::string line;
    while (std::getline(std::cin, line)) {
        std::lock_guard<std::mutex> lk(g_cmd_mtx);
        g_cmds.push_back(line);
    }
}
bool pop_cmd(std::string& out) {
    std::lock_guard<std::mutex> lk(g_cmd_mtx);
    if (g_cmds.empty()) {
        return false;
    }
    out = std::move(g_cmds.front());
    g_cmds.pop_front();
    return true;
}

// ---- control-server request marshaling ----
// Commands from the TCP control server (control.cpp) must run on the main/GL
// thread (they touch GL and re-enter the guest). The server thread enqueues a
// request and blocks until the frame loop processes it, then gets the reply.
struct CtlReq {
    std::string cmd;
    std::string reply;
    bool done = false;
    std::mutex m;
    std::condition_variable cv;
};
std::deque<CtlReq*> g_ctl_q;
std::mutex g_ctl_mtx;

std::string control_exec(const std::string& cmd) { // runs on the server thread
    CtlReq req;
    req.cmd = cmd;
    {
        std::lock_guard<std::mutex> lk(g_ctl_mtx);
        g_ctl_q.push_back(&req);
    }
    std::unique_lock<std::mutex> lk(req.m);
    req.cv.wait(lk, [&] { return req.done; });
    return req.reply;
}

#ifndef __ANDROID__
// Write a surface to disk, picking the encoder from the path's extension:
//   .png -> IMG_SavePNG, .jpg/.jpeg -> IMG_SaveJPG (quality 90), else SDL_SaveBMP.
// Returns 0 on success (matching SDL's convention). PNG/JPG come from SDL2_image.
static int save_surface(SDL_Surface* s, const std::string& path) {
    std::string ext;
    if (auto dot = path.find_last_of('.'); dot != std::string::npos) {
        ext = path.substr(dot + 1);
        for (char& ch : ext) {
            ch = (char)tolower((unsigned char)ch);
        }
    }
    if (ext == "png") {
        return IMG_SavePNG(s, path.c_str());
    }
    if (ext == "jpg" || ext == "jpeg") {
        return IMG_SaveJPG(s, path.c_str(), 90);
    }
    return SDL_SaveBMP(s, path.c_str());
}

// Read the on-screen (front) framebuffer and save it. The output format follows
// the path extension (.png/.jpg/.bmp); see save_surface.
std::string save_screenshot(SDL_Window* win, const std::string& path) {
    int w = 0, h = 0;
    SDL_GL_GetDrawableSize(win, &w, &h);
    if (w <= 0 || h <= 0) {
        return "no drawable";
    }
    std::vector<uint8_t> px((size_t)w * h * 4);
    glReadBuffer(GL_FRONT); // what's actually displayed (post-swap)
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    glReadBuffer(GL_BACK);
    SDL_Surface* s = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ABGR8888);
    if (!s) {
        return std::string("surface alloc failed: ") + SDL_GetError();
    }
    for (int y = 0; y < h; y++) { // GL origin is bottom-left -> flip to top-left
        memcpy((uint8_t*)s->pixels + (size_t)y * s->pitch,
               px.data() + (size_t)(h - 1 - y) * w * 4, (size_t)w * 4);
    }
    bool ok = save_surface(s, path) == 0;
    SDL_FreeSurface(s);
    char buf[512];
    snprintf(buf, sizeof buf, "%s %dx%d -> %s", ok ? "saved" : "save FAILED", w, h, path.c_str());
    return buf;
}

// Read back a host texture object's level-0 image (as RGBA8) and save it as BMP.
// Lets us tell whether a texture *uploaded* correctly vs. fails to *draw*.
std::string dump_texture(const std::string& arg) {
    int id = atoi(arg.c_str());
    std::string path = "tex.bmp";
    if (auto sp = arg.find(' '); sp != std::string::npos) {
        path = arg.substr(sp + 1);
    }
    GLint prev = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev);
    glBindTexture(GL_TEXTURE_2D, (GLuint)id);
    GLint w = 0, h = 0;
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &w);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &h);
    char buf[512];
    if (w <= 0 || h <= 0) {
        glBindTexture(GL_TEXTURE_2D, (GLuint)prev);
        snprintf(buf, sizeof buf, "tex %d has no level-0 image", id);
        return buf;
    }
    std::vector<uint8_t> px((size_t)w * h * 4);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    glBindTexture(GL_TEXTURE_2D, (GLuint)prev);
    SDL_Surface* s = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ABGR8888);
    if (!s) {
        return "surface alloc failed";
    }
    memcpy(s->pixels, px.data(), px.size());
    bool ok = save_surface(s, path) == 0;
    SDL_FreeSurface(s);
    snprintf(buf, sizeof buf, "tex %d %dx%d -> %s%s", id, w, h, path.c_str(), ok ? "" : " (FAILED)");
    return buf;
}

#endif

// ---- touch injection via the Bada event path (FruitNinja) ----
// We deliver touches exactly as bada would: by calling the app form's
// ITouchEventListener overrides GlesForm::OnTouchPressed/Moved/Released(source,
// currentPosition, touchInfo). The form's TransformTouchPos scales the Point
// (device px -> logical) and forwards to Mortar::Touch; the only TouchEventInfo
// method it calls is GetPointId() (an Osp import whose default HLE stub returns
// 0 = a valid point id). Addresses are resolved by symbol name (see main()).
enum TouchKind { TK_PRESSED = 0, TK_MOVED = 1, TK_RELEASED = 2 };

uint32_t g_glesform = 0;                       // GlesForm `this` (from its ctor)
uint32_t g_on[3] = {0, 0, 0};                  // OnTouchPressed/Moved/Released addrs
uint32_t g_skyforce_touch[3] = {0, 0, 0};
uint32_t g_pt_obj = 0;                          // guest Osp::Graphics::Point (x@+4, y@+8)
uint32_t g_ti_obj = 0;                          // guest TouchEventInfo (dummy)
bool g_mouse_down = false;

// ---- app focus / lifecycle (FruitNinja) ----
// Bada delivers OnBackground()/OnForeground() to the Osp::App::Application when
// the app leaves/returns to the foreground. FruitNinja::OnBackground -> Game::Paused
// (opens the pause screen); OnForeground -> Game::UnPaused. We map SDL window
// focus-loss/gain onto these so the game pauses when our window loses focus.
uint32_t g_on_background = 0;                    // FruitNinja::OnBackground(this)
uint32_t g_on_foreground = 0;                    // FruitNinja::OnForeground(this)
bool g_app_focused = true;                       // debounce redundant focus events

// ---- audio pump: the Mortar software mixer's missing heartbeat (FruitNinja) ----
// MAMAudioThread is a *cooperative* mixer: Draw() ticks ThreadMainLoop() once per
// frame, which calls FillBuffer() -- the function that mixes each voice, advances
// its play cursor and, when a voice reaches its end, posts SendSoundStoppedCmd.
// But once the output stream is up (state@+0x2c == 1), ThreadMainLoop only re-enters
// FillBuffer if the previous block was refused (flag@+0x14). On hardware the real
// pump is the AudioOut event path: IAudioOutEventListener::OnAudioOutBufferEndReached
// -> MAMAudioThread::SubsystemBufferCopied(false) -> FillBuffer, once per consumed
// buffer. Our Osp::Media::AudioOut is a write-only stub that never posts events, so
// FillBuffer dies after the priming block: no voice ever ends, no stop is posted,
// MAMAudioController::Update has nothing to dispatch, DefaultSoundRemovedCallback ->
// GameSound::Release never runs, and GameSound's 32 slots leak until every later
// SFXPlay is refused by FindFree() == -1 (i.e. the game goes silent).
// So we generate the heartbeat here, at the true block rate: WriteSamples ships
// 1200 bytes = 300 stereo S16 frames = 37.5 ms at the 8 kHz the game prepares.
// SubsystemBufferCopied self-gates on the stream state, so early calls are no-ops.
uint32_t g_mam_thread = 0;             // MAMAudioThread* (captured at Controller::Init)
uint32_t g_subsystem_buffer_copied = 0; // MAMAudioThread::SubsystemBufferCopied(bool)
bool g_audio_pump = true;              // BADA_NO_AUDIO_PUMP=1 disables (A/B testing)

void audio_pump(Cpu& cpu) {
    if (!g_audio_pump || !g_mam_thread || !g_subsystem_buffer_copied) {
        return;
    }
    static uint32_t last = 0;
    static uint32_t acc = 0; // elapsed, in half-milliseconds (37.5 ms -> 75)
    uint32_t now = SDL_GetTicks();
    if (last) {
        acc += (now - last) * 2;
    }
    last = now;
    for (int blocks = 0; acc >= 75 && blocks < 4; acc -= 75, blocks++) {
        uint32_t r = 0;
        cpu.run_guest(g_subsystem_buffer_copied, {g_mam_thread, 0}, r);
    }
    if (acc > 300) {
        acc = 0; // a long stall (breakpoint, load spike) shouldn't burst-mix
    }
}

// Count calls to the guest audio functions we hook for diagnosis
// (BADA_TRACE_AUDIO=1). One handler for all of them: at an observer the PC *is*
// the hooked entry, so the symbol names itself.
std::map<std::string, uint64_t> g_aud_hits;
void aud_count(Cpu& c) {
    uint32_t off = 0;
    std::string n = c.symbolize(c.reg(UC_ARM_REG_PC) & ~1u, &off);
    uint64_t k = ++g_aud_hits[n];
    if (k <= 2) {
        printf("%s [aud] %s call #%llu\n", hle::log_ts(), hle::demangle(n).c_str(),
               (unsigned long long)k);
    }
}

// Free entries in the guest GameSound pool: 32 slots at GameSound+0x08, stride
// 0x3c, {MortarSound* sound, uint id, bool isFree}. FindFree() scans exactly this.
int gs_free_slots(Cpu& c, uint32_t gs) {
    int n = 0;
    for (int i = 0; i < 32; i++) {
        if (c.read_u32(gs + 0x10 + 0x3c * i) & 0xff) { // slot[i].isFree
            n++;
        }
    }
    return n;
}

struct GestureStep {
    int x, y, kind;
};
std::deque<GestureStep> g_gesture; // scripted multi-point gesture, one step/frame

// Deliver one touch event through the bada listener. wx,wy are window pixels.
void feed_touch(Cpu& cpu, SDL_Window* win, int kind, int wx, int wy) {
    // Two routes to the same event. Fruit Ninja's GlesForm overrides land at
    // fixed addresses we resolved by name; anything else goes through whatever
    // ITouchEventListener it registered, dispatched via its vtable.
    static const int kVtSlot[3] = {2, 5, 4}; // pressed, moved, released
    uint32_t listener = 0, source = 0;
    uint32_t fn = 0, self = 0;
    if (g_glesform && g_on[kind]) {
        fn = g_on[kind];
        self = g_glesform;
        source = g_glesform;
    } else if (cpu.app_instance && g_skyforce_touch[kind]) {
        fn = g_skyforce_touch[kind];
        self = cpu.app_instance;
        source = cpu.app_instance;
    } else if (hle::touch_listener(listener, source)) {
        uint32_t vt = cpu.read_u32(listener);
        fn = vt ? cpu.read_u32(vt + (uint32_t)kVtSlot[kind] * 4) : 0;
        self = listener;
    }
    if (!fn || !g_pt_obj) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            printf("[touch] ignored: no touch listener registered by the app\n");
        }
        return;
    }
    int ww = 800, wh = 480;
    SDL_GetWindowSize(win, &ww, &wh);
    // window px -> device px (800x480) -- TransformTouchPos applies the rest.
    float device_w = g_skyforce_touch[kind] ? 480.0f : 800.0f;
    float device_h = g_skyforce_touch[kind] ? 640.0f : 480.0f;
    int dx = (int)((float)wx * device_w / (float)(ww ? ww : (int)device_w));
    int dy = (int)((float)wy * device_h / (float)(wh ? wh : (int)device_h));
    cpu.write_u32(g_pt_obj, 0);
    cpu.write_u32(g_pt_obj + 4, (uint32_t)dx); // Osp::Graphics::Point.x (@+4)
    cpu.write_u32(g_pt_obj + 8, (uint32_t)dy); // Osp::Graphics::Point.y (@+8)
    cpu.write_u32(g_pt_obj + 12, 0);
    uint32_t ret = 0;
    // OnTouch*(this, source, &currentPosition, &touchInfo)
    cpu.run_guest(fn, {self, source, g_pt_obj, g_ti_obj}, ret);
}

// Deliver an app-lifecycle transition (background/foreground) to the guest app,
// as bada would: OnBackground()/OnForeground() take only `this` (app instance).
// No-op unless the hook was resolved (non-FruitNinja) and the app exists.
void feed_focus(Cpu& cpu, uint32_t hook, const char* what) {
    if (!hook || !cpu.app_instance) {
        return;
    }
    uint32_t ret = 0;
    bool ok = cpu.run_guest(hook, {cpu.app_instance}, ret);
    printf("[focus] %s%s\n", what, ok ? "" : " [FAULTED]");
}

// ---- function argument tracer (stdin: "args <func> <types> [count]") ----
// Arms a patch on a guest function so the next N calls print their arguments.
// Each char of <types> is one argument, in order:
//   i=int  u=uint  x/p=hex pointer  f=float  s=C-string  b=bool
// Per the AAPCS hard-float ABI, integer/pointer args (r0-r3, then stack) and
// float args (s0..) are counted independently -- e.g. a member fn that takes
// (this, uint, bool, float, float, float) is "pubfff".
struct ArgTrace {
    std::string types;
    int remaining;
};
std::map<uint32_t, ArgTrace> g_arg_traces;

bool arg_trace_fn(Cpu& c) {
    uint32_t pc = c.reg(UC_ARM_REG_PC) & ~1u;
    auto it = g_arg_traces.find(pc);
    if (it == g_arg_traces.end() || it->second.remaining <= 0) {
        return false; // not armed -> run the function normally
    }
    std::string out;
    char buf[256];
    int ii = 0, fi = 0; // independent integer / float arg counters
    for (size_t k = 0; k < it->second.types.size(); k++) {
        if (k) {
            out += ", ";
        }
        switch (it->second.types[k]) {
        case 'f': snprintf(buf, sizeof buf, "%g", c.argf32(fi++)); break;
        case 'i': snprintf(buf, sizeof buf, "%d", (int)c.arg32(ii++)); break;
        case 'u': snprintf(buf, sizeof buf, "%u", c.arg32(ii++)); break;
        case 'b': snprintf(buf, sizeof buf, "%s", c.arg32(ii++) ? "true" : "false"); break;
        case 's': {
            uint32_t p = c.arg32(ii++);
            snprintf(buf, sizeof buf, "0x%x \"%s\"", p, p ? c.read_cstr(p, 120).c_str() : "");
            break;
        }
        default: snprintf(buf, sizeof buf, "0x%x", c.arg32(ii++)); break; // x/p/unknown
        }
        out += buf;
    }
    char line[512];
    snprintf(line, sizeof line, "%s [args] 0x%x(%s)", hle::log_ts(), pc, out.c_str());
    printf("%s\n", line);
    hle::trace_log_push(line); // also buffer for retrieval via `tracelog`
    it->second.remaining--;
    return false; // let the real function run
}

// Resolve a target to a guest address. The base is a literal (0xHEX or decimal,
// anything starting with a digit) or a symbol -- exact match first (FUNC or data
// OBJECT), then first FUNC whose mangled name contains the substring. An optional
// "+off" or "-off" suffix (hex or decimal) is added, e.g. "OnTouchMoved+0x4" or
// "0x80001000+24". Returns 0 if the base can't be resolved.
uint32_t resolve_addr(Cpu& cpu, const std::string& tok) {
    std::string base = tok;
    long long off = 0;
    size_t op = tok.find_first_of("+-");
    if (op != std::string::npos && op > 0) {
        base = tok.substr(0, op);
        long long mag = strtoll(tok.c_str() + op + 1, nullptr, 0);
        off = (tok[op] == '-') ? -mag : mag;
    }
    uint32_t a = 0;
    if (!base.empty() && base[0] >= '0' && base[0] <= '9') {
        a = (uint32_t)strtoul(base.c_str(), nullptr, 0); // 0x.. (hex) or decimal
    } else if (cpu.img) {
        a = cpu.img->resolve(base);
        if (!a) {
            a = cpu.img->resolve_contains(base);
        }
    }
    return a ? (uint32_t)((long long)a + off) : 0;
}

// Run one command line and return a reply string (sent to the control client and
// echoed to the console for stdin). Sets quit=true if the command asked to exit.
std::string run_command(Cpu& cpu, SDL_Window* win, std::string c, bool& quit) {
    quit = false;
    if (c.compare(0, 3, "\xEF\xBB\xBF") == 0) {
        c.erase(0, 3); // strip a leading UTF-8 BOM (some stdin writers emit one)
    }
    size_t lead = c.find_first_not_of(" \t");
    if (lead != std::string::npos) {
        c.erase(0, lead);
    }
    while (!c.empty() && (c.back() == '\r' || c.back() == ' ' || c.back() == '\t')) {
        c.pop_back();
    }
    size_t sp = c.find(' ');
    std::string verb = c.substr(0, sp);
    std::string arg = (sp == std::string::npos) ? "" : c.substr(sp + 1);
    char buf[256];

    if (verb.empty()) {
        return "";
#ifdef __ANDROID__
    } else if (verb == "screenshot" || verb == "shot" || verb == "dumptex") {
        return "capture commands are unavailable in the Android game activity";
#else
    } else if (verb == "screenshot" || verb == "shot") {
        return save_screenshot(win, arg.empty() ? "screenshot.bmp" : arg);
    } else if (verb == "dumptex") {
        return dump_texture(arg);
#endif
    } else if (verb == "touch") {
        char sub[16] = {0};
        int x = 0, y = 0;
        if (sscanf(arg.c_str(), "%15s %d %d", sub, &x, &y) < 1) {
            return "usage: touch down|move|up x y";
        }
        int kind = strcmp(sub, "up") == 0     ? TK_RELEASED
                   : strcmp(sub, "move") == 0 ? TK_MOVED
                                              : TK_PRESSED;
        feed_touch(cpu, win, kind, x, y);
        return "ok";
    } else if (verb == "type") {
        // Type into a mocked UI control (see hle/ui.h). Real SDL events, so this
        // drives exactly the path a person at the keyboard does. A trailing
        // "\n" presses Return, which takes the control's confirming button.
        if (!overlay::active()) {
            return "no UI control is on screen";
        }
        std::string text = arg;
        bool enter = false;
        if (text.size() >= 2 && text.compare(text.size() - 2, 2, "\\n") == 0) {
            text.resize(text.size() - 2);
            enter = true;
        }
        for (char ch : text) {
            SDL_Event e{};
            e.type = SDL_TEXTINPUT;
            e.text.text[0] = ch;
            e.text.text[1] = 0;
            SDL_PushEvent(&e);
        }
        if (enter) {
            SDL_Event d{};
            d.type = SDL_KEYDOWN;
            d.key.keysym.sym = SDLK_RETURN;
            SDL_PushEvent(&d);
            SDL_Event u{};
            u.type = SDL_KEYUP;
            u.key.keysym.sym = SDLK_RETURN;
            SDL_PushEvent(&u);
        }
        return "ok";
    } else if (verb == "gesture") {
        std::vector<int> pts;
        std::istringstream is(arg);
        int v;
        while (is >> v) {
            pts.push_back(v);
        }
        size_t n = pts.size() / 2;
        g_gesture.clear();
        for (size_t i = 0; i < n; i++) {
            int kind = (i == 0) ? TK_PRESSED : (i == n - 1) ? TK_RELEASED : TK_MOVED;
            g_gesture.push_back({pts[2 * i], pts[2 * i + 1], kind});
        }
        if (n == 1) {
            g_gesture.push_back({pts[0], pts[1], TK_RELEASED}); // single point = tap
        }
        snprintf(buf, sizeof buf, "queued %zu point(s)", n);
        return buf;
    } else if (verb == "args") {
        // args <func|0xADDR> <types> [count]
        char fn[192] = {0}, types[64] = {0};
        int count = 1;
        if (sscanf(arg.c_str(), "%191s %63s %d", fn, types, &count) < 2) {
            return "usage: args <func|0xADDR> <types> [count]";
        }
        uint32_t addr = resolve_addr(cpu, fn);
        if (!addr) {
            return std::string("function not found: ") + fn;
        }
        bool fresh = g_arg_traces.find(addr) == g_arg_traces.end();
        g_arg_traces[addr] = {types, count};
        if (fresh) {
            cpu.patch(addr, arg_trace_fn);
        }
        snprintf(buf, sizeof buf, "armed 0x%x types=%s count=%d", addr, types, count);
        return buf;
    } else if (verb == "trace") {
        // trace on [filter] | trace off  -- live-log every dispatched guest call
        // whose name contains <filter> (empty = all). Global form of `args`;
        // `trace off` unregisters it. Output is "[trace] <name>" on the console.
        char sub[16] = {0}, filt[192] = {0};
        int n = sscanf(arg.c_str(), "%15s %191s", sub, filt);
        if (n >= 1 && strcmp(sub, "off") == 0) {
            hle::set_trace(false, "");
            snprintf(buf, sizeof buf, "trace off (%llu call(s) logged)",
                     (unsigned long long)hle::trace_count());
            return buf;
        }
        if (n >= 1 && strcmp(sub, "on") == 0) {
            hle::set_trace(true, n >= 2 ? filt : "");
            if (n >= 2) {
                snprintf(buf, sizeof buf, "trace on filter=%s", filt);
            } else {
                snprintf(buf, sizeof buf, "trace on (all calls)");
            }
            return buf;
        }
        return "usage: trace on [filter] | trace off";
    } else if (verb == "tracelog") {
        // tracelog [path]  -- drain the buffered [trace]/[args] lines to a file
        // (one per line) and clear the buffer, so the next poll returns only new
        // lines. The single-line reply reports the count; the lines go to `path`.
        std::string path = arg.empty() ? "tracelog.txt" : arg;
        uint64_t dropped = 0;
        size_t n = hle::trace_log_dump(path, true, &dropped);
        if (n == (size_t)-1) {
            return std::string("cannot write ") + path;
        }
        if (dropped) {
            snprintf(buf, sizeof buf, "wrote %zu line(s) to %s (%llu dropped, buffer was full)",
                     n, path.c_str(), (unsigned long long)dropped);
        } else {
            snprintf(buf, sizeof buf, "wrote %zu line(s) to %s", n, path.c_str());
        }
        return buf;
    } else if (verb == "addr" || verb == "resolve") {
        // addr <symbol|0xADDR>[+/-off]  -> resolved guest address (hex).
        // The inverse of peek: peek reads the value AT an address, this finds
        // the address of a symbol (so you can feed it to peek/poke/call/args).
        char tok[192] = {0};
        if (sscanf(arg.c_str(), "%191s", tok) != 1) {
            return "usage: addr <symbol|0xADDR>[+/-off]";
        }
        uint32_t addr = resolve_addr(cpu, tok);
        if (!addr) {
            return std::string("not found: ") + tok;
        }
        snprintf(buf, sizeof buf, "0x%x", addr);
        return buf;
    } else if (verb == "peek") {
        // peek <addr|symbol> <len>  -> hex
        char tok[192] = {0};
        uint32_t len = 0;
        if (sscanf(arg.c_str(), "%191s %u", tok, &len) != 2) {
            return "usage: peek <addr|symbol> <len>";
        }
        uint32_t addr = resolve_addr(cpu, tok);
        if (!addr) {
            return std::string("not found: ") + tok;
        }
        if (len > 4096) {
            len = 4096;
        }
        std::vector<uint8_t> b(len);
        cpu.read_mem(addr, b.data(), len);
        std::string out;
        for (uint32_t i = 0; i < len; i++) {
            snprintf(buf, sizeof buf, "%02x", b[i]);
            out += buf;
        }
        return out;
    } else if (verb == "poke") {
        // poke <addr|symbol> <hexbytes>
        char tok[192] = {0}, hx[8192] = {0};
        if (sscanf(arg.c_str(), "%191s %8191s", tok, hx) != 2) {
            return "usage: poke <addr|symbol> <hexbytes>";
        }
        uint32_t addr = resolve_addr(cpu, tok);
        if (!addr) {
            return std::string("not found: ") + tok;
        }
        std::vector<uint8_t> b;
        for (size_t i = 0; hx[i] && hx[i + 1]; i += 2) {
            unsigned byte = 0;
            sscanf(hx + i, "%2x", &byte);
            b.push_back((uint8_t)byte);
        }
        cpu.write_mem(addr, b.data(), (uint32_t)b.size());
        snprintf(buf, sizeof buf, "wrote %zu byte(s) @ 0x%x", b.size(), addr);
        return buf;
    } else if (verb == "call") {
        // call <func|0xADDR> [intarg ...]   (integer/pointer args only)
        std::istringstream is(arg);
        std::string fn;
        is >> fn;
        if (fn.empty()) {
            return "usage: call <func|0xADDR> [intarg ...]";
        }
        uint32_t addr = resolve_addr(cpu, fn);
        if (!addr) {
            return "function not found: " + fn;
        }
        std::vector<uint32_t> a;
        std::string t;
        while (is >> t) {
            a.push_back((uint32_t)strtoul(t.c_str(), nullptr, 0));
        }
        uint32_t r = 0;
        bool ok = cpu.run_guest(addr, a, r);
        snprintf(buf, sizeof buf, "%s r0=0x%x (%d)", ok ? "ok" : "FAULT", r, (int)r);
        return buf;
    } else if (verb == "quit" || verb == "q") {
        quit = true;
        return "bye";
    }
    return "unknown: " + verb;
}
} // namespace

// Find the app's executable (Bin/*.exe) inside a package directory.
static std::string find_exe(const std::string& binDir) {
    std::error_code ec;
    for (auto& e : fs::directory_iterator(binDir, ec)) {
        if (e.path().extension() == ".exe") {
            return e.path().string();
        }
    }
    return "";
}

// Read the AppId from the package manifest (Info/Appinfo.sys: AppId="...";
// Info/manifest.xml: <Id>...</Id>).
// Read the package AppId from the Info/ metadata via a real XML parser. Sources,
// in priority order: Appinfo.sys (<Application AppId="...">), manifest.xml
// (<Manifest><Id>...</Id>). Returns "" if none parse / lack an id.
static std::string read_appid(const std::string& root) {
    pugi::xml_document doc;
    if (doc.load_file((root + "/Info/Appinfo.sys").c_str())) {
        if (auto id = doc.child("Application").attribute("AppId")) {
            return id.value();
        }
    }
    if (doc.load_file((root + "/Info/manifest.xml").c_str())) {
        if (auto id = doc.child("Manifest").child("Id")) {
            return id.child_value();
        }
    }
    return "";
}

// Walk up from an exe path to the directory containing Signature.xml; if none is
// found (a bare exe with no package), fall back to the exe's own directory.
static std::string find_root(const std::string& exe) {
    auto s0 = exe.find_last_of("/\\");
    std::string exedir = (s0 == std::string::npos) ? "." : exe.substr(0, s0);
    std::string p = exe;
    for (int up = 0; up < 8; up++) {
        auto s = p.find_last_of("/\\");
        if (s == std::string::npos) {
            break;
        }
        p = p.substr(0, s);
        std::error_code ec;
        if (fs::exists(p + "/Signature.xml", ec)) {
            return p;
        }
    }
    return exedir;
}

// Locate the guest libstdc++.so the app was linked against, when no path was
// given. Looks beside bada-hle, in the package's Bin, then in a bada SDK named
// by $BADA_SDK. Picks the multilib that matches the app's float ABI -- the same
// test Cpu::init uses (soft-float binaries import the __aeabi_f* helpers).
// Returns "" when nothing turned up; main treats that as fatal.
static std::string find_libstdcxx(const ElfImage& img, const std::string& root,
                                  std::vector<std::string>* searched) {
    bool hard = true;
    for (auto& n : img.imports) {
        if (n == "__aeabi_fmul" || n == "__aeabi_fadd") {
            hard = false;
            break;
        }
    }
    std::vector<std::string> dirs;
    if (char* base = SDL_GetBasePath()) {
        dirs.push_back(base);
        SDL_free(base);
    }
    dirs.push_back(root + "/Bin");
    if (const char* sdk = getenv("BADA_SDK")) {
        std::string lib = std::string(sdk) + "/Tools/Toolchains/ARM/arm-bada-eabi/lib";
        dirs.push_back(hard ? lib + "/hard/armv6/vfp" : lib);
    }
    // The SDK ships one libstdc++ per multilib under the same name, so a copy
    // dropped next to bada-hle carries its ABI in the name instead.
    std::string tagged = hard ? "libstdc++-hard.so" : "libstdc++-soft.so";
    std::error_code ec;
    for (std::string& d : dirs) {
        while (d.size() > 1 && (d.back() == '/' || d.back() == '\\')) {
            d.pop_back(); // SDL_GetBasePath keeps a trailing separator
        }
        for (const std::string& name :
             {tagged, std::string("libstdc++.so.6.0.12"), std::string("libstdc++.so.6")}) {
            std::string p = d + "/" + name;
            if (searched) {
                searched->push_back(p);
            }
            if (fs::exists(p, ec)) {
                printf("[boot] guest libstdc++ (%s-float): %s\n", hard ? "hard" : "soft", p.c_str());
                return p;
            }
        }
    }
    return "";
}

// Bada device screen (Samsung Wave-class): 480x800 portrait. FruitNinja renders
// 480x320 landscape, rotated 90 deg on-device. We open the real device size.
static const int DEV_W = 480;
static const int DEV_H = 800;

int main(int argc, char** argv) {
    setbuf(stdout, nullptr); // unbuffered so piped logs appear immediately
    // CLI: bada-hle <app-dir-or-exe> [AppId] [maxFrames] [--locale xx[_CC]]
    //   <app-dir>  : a bada package directory (has Signature.xml) -> resolves the
    //                exe (Bin/*.exe) and AppId (Info/Appinfo.sys) automatically.
    //   <exe>      : a bare .exe (e.g. the GlesCube sample, no package).
    // AppId/maxFrames stay positional for back-compat; --app-id/--max-frames also
    // work. --locale (or env BADA_LOCALE) overrides the reported system language.
    cxxopts::Options opts("bada-hle", "Samsung bada high-level emulator");
    opts.add_options()                                                                    //
        ("input", "bada package dir or bare .exe", cxxopts::value<std::string>())          //
        ("a,app-id", "AppId override", cxxopts::value<std::string>()->default_value(""))   //
        ("f,max-frames", "stop after N frames (0 = run until window close)",
         cxxopts::value<int>()->default_value("0"))                                        //
        ("l,locale", "system locale override, e.g. fre or fre_FR (or env BADA_LOCALE)",
         cxxopts::value<std::string>()->default_value(""))   //
        ("libstdcxx", "guest libstdc++.so to map (or env BADA_LIBSTDCXX); found automatically if omitted",
         cxxopts::value<std::string>()->default_value(""))   //
        ("h,help", "show this help");
#ifdef __ANDROID__
    opts.add_options()("log-file", "capture native diagnostics", cxxopts::value<std::string>()->default_value(""));
    auto diagnostics = std::make_unique<DiagnosticLog>();
#endif
    opts.parse_positional({"input", "app-id", "max-frames"});
    opts.positional_help("<app-dir-or-exe> [AppId] [maxFrames]");

    std::string input, appid, locale, libstdcxx;
    int max_frames = 0;
    try {
        auto res = opts.parse(argc, argv);
#ifdef __ANDROID__
        const auto log_path = res["log-file"].as<std::string>();
        if (!log_path.empty()) diagnostics->start(log_path);
#endif
        if (res.count("help") || !res.count("input")) {
            printf("%s\n", opts.help().c_str());
            return res.count("help") ? 0 : 2;
        }
        input = res["input"].as<std::string>();
        appid = res["app-id"].as<std::string>();
        max_frames = res["max-frames"].as<int>();
        locale = res["locale"].as<std::string>();
        libstdcxx = res["libstdcxx"].as<std::string>();
    } catch (const std::exception& e) {
        printf("arg error: %s\n%s\n", e.what(), opts.help().c_str());
        return 2;
    }
    if (locale.empty()) {
        if (const char* e = getenv("BADA_LOCALE")) {
            locale = e;
        }
    }
    if (libstdcxx.empty()) {
        if (const char* e = getenv("BADA_LIBSTDCXX")) {
            libstdcxx = e;
        }
    }
    hle::set_locale(locale); // no-op when empty -> keeps the eng/US default

    std::string exe, root;
    std::error_code dec;
    if (fs::is_directory(input, dec)) {
        root = input;
        while (root.size() > 1 && (root.back() == '/' || root.back() == '\\')) {
            root.pop_back();
        }
        exe = find_exe(root + "/Bin");
        if (exe.empty()) {
            printf("no .exe found in %s/Bin\n", root.c_str());
            return 1;
        }
        if (appid.empty()) {
            appid = read_appid(root);
        }
    } else {
        exe = input;
        root = find_root(exe);
    }
    if (appid.empty()) {
        appid = "52469ud106";
    }
    printf("[boot] exe=%s\n[boot] package=%s AppId=%s frames=%s\n", exe.c_str(), root.c_str(),
           appid.c_str(), max_frames ? std::to_string(max_frames).c_str() : "infinite");

    // Read imports before creating the host context: fixed-function games need
    // ES 1 on Android, while shaders and the Canvas presenter need ES 2.
    ElfImage img;
    std::string err;
    if (!img.load(exe, err)) {
        printf("load failed: %s\n", err.c_str());
        return 1;
    }

    // ---- SDL window + GL context (compatibility profile for fixed-function) ----
    SDL_SetMainReady(); // we used SDL_MAIN_HANDLED; tell SDL the entry point ran
#ifdef __ANDROID__
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
#endif
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        printf("SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
#ifdef __ANDROID__
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    const int gles_version = hle::gles_context_version(img.imports);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, gles_version);
    // EGL selects the ES 1.x API with client version 1.
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
#else
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_COMPATIBILITY);
#endif
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 16);
    std::string exe_name = exe.substr(exe.find_last_of("/\\") + 1);
    std::string base_title = "bada-hle [" + exe_name + "]";
    Uint32 window_flags = SDL_WINDOW_OPENGL;
#ifdef __ANDROID__
    window_flags |= SDL_WINDOW_FULLSCREEN_DESKTOP | SDL_WINDOW_ALLOW_HIGHDPI;
#endif
    SDL_Window* win = SDL_CreateWindow(base_title.c_str(), SDL_WINDOWPOS_CENTERED,
                                       SDL_WINDOWPOS_CENTERED, DEV_W, DEV_H, window_flags);
    if (!win) {
        printf("SDL_CreateWindow: %s\n", SDL_GetError());
        return 1;
    }
    SDL_GLContext glctx = SDL_GL_CreateContext(win);
    if (!glctx) {
        printf("SDL_GL_CreateContext: %s\n", SDL_GetError());
        return 1;
    }
    printf("[gl] vendor=%s renderer=%s\n", glGetString(GL_VENDOR), glGetString(GL_RENDERER));
    printf("[gl] version=%s\n", glGetString(GL_VERSION));
#ifdef __ANDROID__
    if (gles_version >= 2)
#endif
    printf("[gl] glsl=%s\n", glGetString(GL_SHADING_LANGUAGE_VERSION));
    {
        int pf = 0;
        SDL_GL_GetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, &pf);
        printf("[gl] profile=%s\n", pf == SDL_GL_CONTEXT_PROFILE_COMPATIBILITY ? "compat" : pf == SDL_GL_CONTEXT_PROFILE_CORE ? "core"
                                                                                                                              : "es/other");
    }
    hle::gl_set_window(win); // sglSwapBuffers -> SDL_GL_SwapWindow
    overlay::init(win);
    // The app owns the swap, so the overlay draws from inside sglSwapBuffers.
    hle::gl_set_preswap([win](Cpu&) { overlay::draw(win); });

    // Clear once so the window is visibly alive before we start the guest.
    glClearColor(0.1f, 0.1f, 0.12f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    SDL_GL_SwapWindow(win);

    // ---- The guest libstdc++, required (--libstdcxx / BADA_LIBSTDCXX, else found) ----
    // The bada SDK ships the ARM libstdc++.so.6.0.12 these apps were linked
    // against. Mapping it runs the app's std::* imports on the real code, so the
    // layouts match by construction -- the guest inlines half of std::string and
    // reads its _Rep header directly. There is no HLE stand-in to fall back to,
    // so a missing library is fatal.
    if (libstdcxx.empty()) {
        std::vector<std::string> searched;
        libstdcxx = find_libstdcxx(img, root, &searched);
        if (libstdcxx.empty()) {
            printf("\nERROR: no guest libstdc++.so found.\n"
                   "The app's std::* calls need the ARM libstdc++ it was linked against"
                   " (the bada SDK ships it under Tools/Toolchains/ARM/arm-bada-eabi/lib).\n"
                   "Give one with --libstdcxx <path>, set BADA_LIBSTDCXX, set BADA_SDK, or"
                   " copy it next to bada-hle as libstdc++-hard.so / libstdc++-soft.so.\n"
                   "Looked in:\n");
            for (const std::string& p : searched) {
                printf("  %s\n", p.c_str());
            }
            return 1;
        }
    }
    ElfImage libcxx;
    if (!libcxx.load(libstdcxx, err, guest::LIB_BASE)) {
        printf("libstdc++ load failed: %s\n", err.c_str());
        return 1;
    }

    Cpu cpu;
    if (!cpu.init(img, err, &libcxx)) {
        printf("cpu init failed: %s\n", err.c_str());
        return 1;
    }

    hle::set_package_root(root);

    // Touch event arguments, shared by both delivery routes (see feed_touch).
    // Every app needs these, not just the one with a named GlesForm.
    g_pt_obj = cpu.alloc(16);                    // Osp::Graphics::Point (x@+4, y@+8)
    g_ti_obj = cpu.make_stub_object(32);         // TouchEventInfo (GetPointId() only)
    if (exe.find("SkyForce") != std::string::npos) {
        g_skyforce_touch[TK_PRESSED] = img.resolve("_ZN8SkyForce14OnTouchPressedERKN3Osp2Ui7ControlERKNS0_8Graphics5PointERKNS1_14TouchEventInfoE");
        g_skyforce_touch[TK_MOVED] = img.resolve("_ZN8SkyForce12OnTouchMovedERKN3Osp2Ui7ControlERKNS0_8Graphics5PointERKNS1_14TouchEventInfoE");
        g_skyforce_touch[TK_RELEASED] = img.resolve("_ZN8SkyForce15OnTouchReleasedERKN3Osp2Ui7ControlERKNS0_8Graphics5PointERKNS1_14TouchEventInfoE");
        printf("[touch] SkyForce OnTouch P/M/R=0x%x/0x%x/0x%x\\n", g_skyforce_touch[0], g_skyforce_touch[1], g_skyforce_touch[2]);
    }

    if (exe.find("FruitNinja") != std::string::npos) {
        // The TTF glyph-atlas page's texture ([page+0x40]) comes out garbage
        // (atlas/texture subsystem not fully wired). Skip *garbage* atlas
        // textures so the draw continues (text without glyph texture) rather
        // than deref a bad pointer; valid (heap) textures run unchanged.
        cpu.patch(0x259290, [](Cpu& c) -> bool { // TextureAtlasPage::GetTexture
            uint32_t tex = c.read_u32(c.reg(UC_ARM_REG_R1) + 0x40);
            if (tex >= 0x80000000u && tex < 0x88000000u) {
                return false;
            }
            uint32_t out = c.reg(UC_ARM_REG_R0);
            c.write_u32(out, 0); // null SmartPtr -> caller skips texture
            c.set_ret32(out);
            return true;
        });
        // Touch: resolve the bada listener entries by symbol name, capture the
        // GlesForm instance from its ctor, and allocate the Point + TouchEventInfo
        // arguments. Host mouse / scripted gestures then call OnTouch* (feed_touch).
        g_on[TK_PRESSED] = img.resolve(
            "_ZN8GlesForm14OnTouchPressedERKN3Osp2Ui7ControlERKNS0_8Graphics5PointERKNS1_14TouchEventInfoE");
        g_on[TK_MOVED] = img.resolve(
            "_ZN8GlesForm12OnTouchMovedERKN3Osp2Ui7ControlERKNS0_8Graphics5PointERKNS1_14TouchEventInfoE");
        g_on[TK_RELEASED] = img.resolve(
            "_ZN8GlesForm15OnTouchReleasedERKN3Osp2Ui7ControlERKNS0_8Graphics5PointERKNS1_14TouchEventInfoE");
        uint32_t gctor = img.resolve("_ZN8GlesFormC1EP10FruitNinja");
        if (gctor) {
            cpu.patch(gctor, [](Cpu& c) -> bool {
                if (!g_glesform) {
                    g_glesform = c.reg(UC_ARM_REG_R0);
                    printf("[touch] GlesForm @ 0x%x\n", g_glesform);
                }
                return false; // run the real ctor
            });
        }
        printf("[touch] OnTouch P/M/R=0x%x/0x%x/0x%x ctor=0x%x\n", g_on[0], g_on[1], g_on[2],
               gctor);

        // Focus: FruitNinja's app-lifecycle virtuals. Window focus-loss/gain map
        // to these (OnBackground -> pause, OnForeground -> unpause).
        g_on_background = img.resolve("_ZN10FruitNinja12OnBackgroundEv");
        g_on_foreground = img.resolve("_ZN10FruitNinja12OnForegroundEv");
        printf("[focus] OnBackground/OnForeground=0x%x/0x%x\n", g_on_background, g_on_foreground);

        // ---- Sound tracing: hook the whole funnel, not just its tail ----
        // Game SFX go
        //   GameSound::SFXPlay(name, vol, vol2, finishCb, pitch)
        //     -> SoundManager::SFXPlay -> SFXPlayInternal -> MortarSound::Load/Play
        // GameSound owns 32 slots and only frees one in GameSound::Update() once
        // MortarSound::IsPlaying() and IsPaused() are both false. With every slot
        // busy, FindFree() returns -1 and SFXPlay bails *before* SFXPlayInternal:
        // a "[sfx] GameSound::SFXPlay" line with no "[sfx] SFXPlayInternal" behind
        // it is a dropped play (slot leak), not a missing hook. Music is a wholly
        // separate path (Song*) that never reaches SFXPlayInternal.
        auto trace = [&](const char* sym, hle::Handler fn) {
            uint32_t a = img.resolve(sym) & ~1u; // & ~1: Thumb bit isn't part of the addr
            if (a) {
                cpu.add_observer(a, fn);
            }
            printf("[sfx] hook 0x%-8x %s\n", a, sym);
        };
        // r0=this, r1=name, r2=&finishCb, s0/s1=volume, s2=pitch (hard-float ABI).
        // The slot walk makes the silent refusal visible: with no free slot the
        // guest returns here, before SoundManager::SFXPlay, and nothing is played.
        trace("_ZN9GameSound7SFXPlayEPKcffN6Mortar9Delegate1IbPNS2_11MortarSoundEEEf", [](Cpu& c) {
            std::string name = c.read_cstr(c.reg(UC_ARM_REG_R1));
            int freen = gs_free_slots(c, c.reg(UC_ARM_REG_R0));
            if (freen == 0) {
                static std::map<std::string, uint64_t> drops;
                uint64_t n = ++drops[name];
                if (n == 1 || n % 100 == 0) {
                    printf("%s [sfx] GameSound::SFXPlay(\"%s\") NO FREE SLOT -- refused (x%llu)\n",
                           hle::log_ts(), name.c_str(), (unsigned long long)n);
                }
                return;
            }
            printf("%s [sfx] GameSound::SFXPlay(\"%s\", %.2f, %.2f, pitch=%.2f) free=%d/32\n",
                   hle::log_ts(), name.c_str(), c.argf32(0), c.argf32(1), c.argf32(2), freen);
        });
        // SFXPlayInternal(const char* name, unsigned long, MortarSound*, unsigned char, long)
        trace("_ZN6Mortar12SoundManager15SFXPlayInternalEPKcmPNS_11MortarSoundEhl", [](Cpu& c) {
            printf("%s [sfx]   SFXPlayInternal(\"%s\", snd=0x%x)\n", hle::log_ts(),
                   c.read_cstr(c.reg(UC_ARM_REG_R1)).c_str(), c.reg(UC_ARM_REG_R3));
        });
        trace("_ZN9GameSound7KillAllEv",
              [](Cpu&) { printf("%s [sfx] GameSound::KillAll()\n", hle::log_ts()); });
        // Every volume change funnels through MortarSound::SetVolume (the only
        // caller of MAMAudioController::SetSoundVolume). MortarSound is
        // {vptr, char* m_Name, m_Handle, m_State} -- so the sound is nameable here,
        // unlike downstream where only the opaque handle survives. GameSound::Update
        // re-sends the volume every frame, so only log actual changes of the byte
        // the mixer sees; that byte truncates without clamping, so >1.004 wraps.
        trace("_ZN6Mortar11MortarSound9SetVolumeEf", [](Cpu& c) {
            static std::map<uint32_t, int> last;
            uint32_t self = c.reg(UC_ARM_REG_R0);
            float vol = c.argf32(0);
            int byte = (vol * 255.0f > 0.0f) ? (int)(uint8_t)(int)(vol * 255.0f) : 0;
            auto it = last.find(self);
            if (it != last.end() && it->second == byte) {
                return;
            }
            last[self] = byte;
            uint32_t handle = c.read_u32(self + 8);
            uint32_t name = c.read_u32(self + 4);
            printf("%s [sfx] SetVolume(\"%s\", %.3f -> %d/255%s)\n", hle::log_ts(),
                   name ? c.read_cstr(name).c_str() : "?", vol, byte,
                   handle ? "" : ", handle=0: dropped");
        });
        trace("_ZN6Mortar12SoundManager8SongPlayEPKc", [](Cpu& c) {
            printf("%s [sfx] SongPlay(\"%s\")\n", hle::log_ts(),
                   c.read_cstr(c.reg(UC_ARM_REG_R1)).c_str());
        });
        trace("_ZN6Mortar12SoundManager14SongSwitchPlayEPKc", [](Cpu& c) {
            printf("%s [sfx] SongSwitchPlay(\"%s\")\n", hle::log_ts(),
                   c.read_cstr(c.reg(UC_ARM_REG_R1)).c_str());
        });
        trace("_ZN6Mortar12SoundManager8SongStopEv",
              [](Cpu&) { printf("%s [sfx] SongStop()\n", hle::log_ts()); });
        trace("_ZN6Mortar12SoundManager12PreLoadSoundEPKc", [](Cpu& c) {
            printf("%s [sfx] PreLoadSound(\"%s\")\n", hle::log_ts(),
                   c.read_cstr(c.reg(UC_ARM_REG_R1)).c_str());
        });
        trace("_ZN6Mortar12SoundManager14PreLoadSoundExEPKcb", [](Cpu& c) {
            printf("%s [sfx] PreLoadSoundEx(\"%s\", %u)\n", hle::log_ts(),
                   c.read_cstr(c.reg(UC_ARM_REG_R1)).c_str(), c.reg(UC_ARM_REG_R2));
        });

        // ---- host heartbeat for the guest mixer (see audio_pump) ----
        if (getenv("BADA_NO_AUDIO_PUMP")) {
            g_audio_pump = false;
        }
        g_subsystem_buffer_copied = img.resolve("_ZN14MAMAudioThread21SubsystemBufferCopiedEb") & ~1u;
        trace("_ZN18MAMAudioController4InitEP14MAMAudioThread", [](Cpu& c) {
            g_mam_thread = c.reg(UC_ARM_REG_R1); // Init(this, MAMAudioThread*)
            printf("[sfx] MAMAudioThread @ 0x%x -- host audio pump %s\n", g_mam_thread,
                   g_audio_pump ? "armed" : "DISABLED (BADA_NO_AUDIO_PUMP)");
        });
        // Diagnosis: call counts along the mixer -> voice-end -> slot-release chain.
        if (getenv("BADA_TRACE_AUDIO")) {
            for (const char* s : {"_ZN14MAMAudioThread14ThreadMainLoopEv",
                                  "_ZN14MAMAudioThread10FillBufferEv",
                                  "_ZN14MAMAudioThread20ProcessSoundCommandsEv",
                                  "_ZN14MAMAudioThread12PlayNewSoundERK16MAMSoundInputCmd",
                                  "_ZN14MAMAudioThread19SendSoundStoppedCmdEj",
                                  "_ZN14MAMAudioThread21SubsystemInitCompleteEb",
                                  "_ZN14MAMAudioThread21SubsystemBufferCopiedEb",
                                  "_ZN14MAMAudioThread17FireUpAudioStreamEv",
                                  "_ZN18MAMAudioController6UpdateEv",
                                  "_ZN18MAMAudioController9PlaySoundEP8MAMSoundPjb",
                                  "_Z27DefaultSoundRemovedCallbackPN6Mortar11MortarSoundE",
                                  "_ZN9GameSound7ReleaseEPN6Mortar11MortarSoundEPKc",
                                  "_ZN20MortarAudioMixerBada12WriteSamplesEPvj"}) {
                trace(s, aud_count);
            }
        }
    }

    // ---- Run static constructors ----
    printf("\n[boot] running %zu init_array constructors...\n", img.init_array.size());
    cpu.run_init_array();

    // ---- Build argv and call OspMain (the real bada entry; e_entry is bogus) ----
    // OspMain elf_hash()es argv[0][44..54) against the AppId and bails on mismatch.
    std::string path(44, '/');
    path[1] = 'm'; // != 'O' so OspMain takes the offset-44 branch
    path += appid; // 10-char AppId at offset 44 -> elf_hash must match
    path += "/Bin/app";
    uint32_t path_len = (uint32_t)(path.size() + 1); // small, fixed-form path
    uint32_t str_addr = cpu.alloc(path_len);
    cpu.write_mem(str_addr, path.c_str(), path_len);
    uint32_t argv_addr = cpu.alloc(8);
    cpu.write_u32(argv_addr, str_addr); // argv[0]
    cpu.write_u32(argv_addr + 4, 0);    // argv[1] = NULL

    uint32_t ospmain = img.resolve("OspMain");
    printf("[boot] calling OspMain 0x%x (argv[0]=\"%s\") ...\n", ospmain, path.c_str());
    uint32_t ret = 0;
    bool ok = cpu.run_guest(ospmain, {1, argv_addr}, ret);
    printf("[boot] OspMain returned ok=%d r0=0x%x\n", ok, ret);

    // ---- Application lifecycle (host-driven; Execute handed control back) ----
    if (cpu.exec_requested) {
        printf("\n[boot] factory (app CreateInstance) ...\n");
        uint32_t app = 0;
        cpu.run_guest(cpu.exec_factory, {}, app);
        cpu.app_instance = app;
        printf("[boot] app instance = 0x%x\n", app);

        // OnAppInitializing(AppRegistry&) -- the big init (UI/GL/resources).
        // Match by signature substring so any app subclass works (not just FruitNinja).
        uint32_t reg = cpu.make_stub_object(128); // dummy AppRegistry w/ no-op vtable
        uint32_t oai = img.resolve_contains("17OnAppInitializingERN3Osp3App11AppRegistryE");
        printf("[boot] OnAppInitializing 0x%x(app=0x%x) ...\n", oai, app);
        uint32_t r = 0;
        bool ok2 = cpu.run_guest(oai, {app, reg}, r);
        printf("[boot] OnAppInitializing ok=%d -> %u\n", ok2, r);
        if (!ok2) {
            printf("[boot] init faulted; not entering frame loop\n");
            hle::dump_stats();
            return 0;
        }

        // ---- Timer-driven frame loop: each frame = one OnTimerExpired -> Draw ----
        uint32_t ote = img.resolve_contains("14OnTimerExpiredE");
        printf("[boot] OnTimerExpired @ 0x%x -- entering frame loop\n", ote);
        std::thread(stdin_reader).detach(); // scriptable: "screenshot <path>" / "quit"
        if (const char* cp = getenv("BADA_CONTROL_PORT")) { // localhost TCP control server
            start_control_server(atoi(cp), control_exec);
        }
        const uint32_t kFrameMs = 16;        // host loop granularity
        uint32_t next_tick = SDL_GetTicks(); // when the app's timer is next due
        int frames = 0;
        uint32_t fps_t0 = SDL_GetTicks(); // window-title FPS counter
        int fps_frames = 0;
        uint32_t busy_ms = 0; // accumulated per-frame work time (diag)
        for (bool running = true; running;) {
            uint32_t frame_start = SDL_GetTicks();
            SDL_Event ev;
            while (SDL_PollEvent(&ev)) {
                if (ev.type == SDL_QUIT) {
                    running = false;
                    continue;
                }
#ifdef __ANDROID__
                if (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_AC_BACK) {
                    running = false;
                    continue;
                }
#endif

                if (overlay::handle_event(ev)) {
                    continue; // a mocked control has focus; the guest sees nothing
                }
#ifdef __ANDROID__
                if (ev.type == SDL_FINGERDOWN && !g_mouse_down) {
                    int w = 0, h = 0;
                    SDL_GetWindowSize(win, &w, &h);
                    g_mouse_down = true;
                    feed_touch(cpu, win, TK_PRESSED, (int)(ev.tfinger.x * w), (int)(ev.tfinger.y * h));
                } else if (ev.type == SDL_FINGERMOTION && g_mouse_down) {
                    int w = 0, h = 0;
                    SDL_GetWindowSize(win, &w, &h);
                    feed_touch(cpu, win, TK_MOVED, (int)(ev.tfinger.x * w), (int)(ev.tfinger.y * h));
                } else if (ev.type == SDL_FINGERUP && g_mouse_down) {
                    int w = 0, h = 0;
                    SDL_GetWindowSize(win, &w, &h);
                    g_mouse_down = false;
                    feed_touch(cpu, win, TK_RELEASED, (int)(ev.tfinger.x * w), (int)(ev.tfinger.y * h));
                }
#endif

                if (ev.type == SDL_MOUSEBUTTONDOWN && ev.button.button == SDL_BUTTON_LEFT) {
                    g_mouse_down = true;
                    feed_touch(cpu, win, TK_PRESSED, ev.button.x, ev.button.y);
                } else if (ev.type == SDL_MOUSEMOTION && g_mouse_down) {
                    feed_touch(cpu, win, TK_MOVED, ev.motion.x, ev.motion.y);
                } else if (ev.type == SDL_MOUSEBUTTONUP && ev.button.button == SDL_BUTTON_LEFT) {
                    g_mouse_down = false;
                    feed_touch(cpu, win, TK_RELEASED, ev.button.x, ev.button.y);
                } else if (ev.type == SDL_WINDOWEVENT) {
                    // Window focus drives the app lifecycle: losing focus pauses
                    // the game (OnBackground), regaining it resumes (OnForeground).
                    bool lost = ev.window.event == SDL_WINDOWEVENT_FOCUS_LOST ||
                                ev.window.event == SDL_WINDOWEVENT_MINIMIZED ||
                                ev.window.event == SDL_WINDOWEVENT_HIDDEN;
                    bool gained = ev.window.event == SDL_WINDOWEVENT_FOCUS_GAINED ||
                                  ev.window.event == SDL_WINDOWEVENT_RESTORED;
                    if (lost && g_app_focused) {
                        g_app_focused = false;
                        if (g_mouse_down) { // don't leave a blade stroke held
                            g_mouse_down = false;
                            int mx = 0, my = 0;
                            SDL_GetMouseState(&mx, &my);
                            feed_touch(cpu, win, TK_RELEASED, mx, my);
                        }
                        feed_focus(cpu, g_on_background, "OnBackground (focus lost -> pause)");
                    } else if (gained && !g_app_focused) {
                        g_app_focused = true;
                        feed_focus(cpu, g_on_foreground, "OnForeground (focus gained -> resume)");
                    }
                }
            }
            std::string cmd;
            while (pop_cmd(cmd)) { // stdin commands
                bool q = false;
                std::string r = run_command(cpu, win, cmd, q);
                if (!r.empty()) {
                    printf("%s\n", r.c_str());
                }
                running = running && !q;
            }
            for (;;) { // control-server commands (run here, reply to the client)
                CtlReq* req = nullptr;
                {
                    std::lock_guard<std::mutex> lk(g_ctl_mtx);
                    if (!g_ctl_q.empty()) {
                        req = g_ctl_q.front();
                        g_ctl_q.pop_front();
                    }
                }
                if (!req) {
                    break;
                }
                bool q = false;
                req->reply = run_command(cpu, win, req->cmd, q);
                {
                    std::lock_guard<std::mutex> lk(req->m);
                    req->done = true;
                }
                req->cv.notify_one();
                running = running && !q;
            }
            if (!g_gesture.empty()) { // play one scripted gesture step per frame
                GestureStep st = g_gesture.front();
                g_gesture.pop_front();
                feed_touch(cpu, win, st.kind, st.x, st.y);
            }
            // The app drives its own rate: OnTimerExpired ends by re-arming the
            // one-shot timer with the time left in its period (PvZ asks for
            // 100 ms). Firing it once per host frame instead runs the app's whole
            // clock at our frame rate -- the game plays several times too fast.
            bool ticked = false;
            if (ote) {
                if ((int32_t)(SDL_GetTicks() - next_tick) >= 0) {
                    uint32_t rr = 0;
                    if (!cpu.run_guest(ote, {cpu.app_instance, cpu.timer_obj}, rr)) {
                        break;
                    }
                    uint32_t iv = cpu.timer_interval_ms ? cpu.timer_interval_ms : kFrameMs;
                    next_tick = SDL_GetTicks() + iv;
                    ticked = true;
                }
            } else {
                glClear(GL_COLOR_BUFFER_BIT);
                SDL_GL_SwapWindow(win);
                ticked = true;
            }
            overlay::pump(cpu);        // replay mocked-UI input into the app
            hle::user_event_pump(cpu); // deliver Control::SendUserEvent posts
            hle::player_pump(cpu);     // signal no-op video clips as ended (intro skip)
            audio_pump(cpu);       // AudioOut buffer-end heartbeat for the guest mixer
            if (ticked && ++frames == 1) {
                printf("[boot] first frame drawn\n");
            }
            // Sleep until the app's next tick, but wake at least every kFrameMs
            // so window events still get answered between slow ticks.
            uint32_t now = SDL_GetTicks();
            busy_ms += now - frame_start;
            uint32_t wake = next_tick;
            if ((int32_t)(wake - now) > (int32_t)kFrameMs) {
                wake = now + kFrameMs;
            }
            if ((int32_t)(wake - now) > 0) {
                SDL_Delay(wake - now);
            }
            // Count what the app actually drew, not how often the host loop ran.
            if (uint32_t dt = SDL_GetTicks() - fps_t0; fps_frames += ticked, dt >= 500) {
                char title[256];
                int n = fps_frames ? fps_frames : 1; // a single slow tick can span the window
                snprintf(title, sizeof title, "%s %dfps (%.1fms cpu)", base_title.c_str(),
                         (int)(fps_frames * 1000.0 / dt + 0.5), (double)busy_ms / n);
                SDL_SetWindowTitle(win, title);
                fps_t0 += dt;
                fps_frames = 0;
                busy_ms = 0;
            }
            if (max_frames && frames >= max_frames) {
                running = false;
            }
        }
        printf("[boot] rendered %d frames\n", frames);
        for (auto& [n, k] : g_aud_hits) { // BADA_TRACE_AUDIO call counts
            printf("[aud] %8llu  %s\n", (unsigned long long)k, hle::demangle(n).c_str());
        }
    }

    hle::dump_stats();
    SDL_GL_DeleteContext(glctx);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
