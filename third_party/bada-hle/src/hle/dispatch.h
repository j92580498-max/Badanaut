#pragma once
#include <functional>
#include <cstdint>
#include <string>

class Cpu;

namespace hle {

using Handler = void (*)(Cpu&);

void register_handler(const char* name, Handler h);
void init_dispatch(); // wires up libc/gl/osp tables
void call(const std::string& name, Cpu& cpu);
void dump_stats(); // viability report: call counts

// Runtime call trace: when on, every dispatched guest symbol whose (mangled)
// name contains `filter` is logged live to the console (empty filter = all).
// The global, toggleable counterpart to the per-function `args` tracer.
void set_trace(bool on, const std::string& filter);
uint64_t trace_count(); // # of calls logged since the last set_trace(true, ...)

// Trace-log buffer. `[trace]` (dispatch) and `[args]` (main.cpp) lines are also
// captured into an in-memory ring buffer, since the control protocol replies are
// one line and can't carry the multi-line trace stream. A driver (MCP's
// `trace_log`) retrieves them via the `tracelog` command.
//   push  -- append a line (ring-capped; oldest dropped when full)
//   dump  -- write the buffer to `path` (one line each); if `clear`, empty it
//            afterwards (drain semantics -> repeat polls return only new lines).
//            Returns the line count written, or (size_t)-1 if `path` can't be
//            opened; *dropped (if given) gets the count lost to the ring cap.
void trace_log_push(const std::string& line);
size_t trace_log_dump(const std::string& path, bool clear, uint64_t* dropped = nullptr);

// per-module registration
void register_libc();
void register_gl();
void register_osp();
void register_io();
void register_ui();
void register_audio();

// Osp::Base::String <-> host string, on a guest String object (osp.cpp). The UI
// mock builds and reads them for SetText/GetText.
void osp_string_set(Cpu& c, uint32_t obj, const std::string& utf8);
std::string osp_string_get(Cpu& c, uint32_t obj);
void set_locale(const std::string& spec); // override reported system locale (--locale / BADA_LOCALE)

// Per-frame pump (osp.cpp): fires the video player's OnPlayerEndOfClip listener
// once a no-op clip has elapsed, so apps advance past intro movies. Needs a Cpu&
// to re-enter the guest, so it's called from the frame loop (not a handler).
void player_pump(Cpu& cpu);
void audio_pump(Cpu& cpu); // completed PCM buffers -> guest AudioOut listener
void audio_pause(bool pause);
void audio_shutdown();
bool byte_buffer_view(uint32_t object, uint32_t& data, uint32_t& length);
// Per-frame pump (osp.cpp): delivers whatever Control::SendUserEvent queued to
// the app's OnUserEventReceivedN. bada posts these to the UI event loop, so the
// callback lands on a later frame, not inside the SendUserEvent call.
void user_event_pump(Cpu& cpu);
// The ITouchEventListener the app registered with Control::AddTouchEventListener,
// and the control it was registered on. False when the app registered none.
bool touch_listener(uint32_t& listener, uint32_t& source);
bool touch_multipoint_enabled(uint32_t source);
// Associate the emulator-owned TouchEventInfo with this callback's contact.
void touch_event_set(uint32_t object, uint32_t point_id, int x = 0, int y = 0);
void set_package_root(const std::string& root); // .../FruitNinjaBada

std::string demangle(const std::string& name);

// printf-style formatting over guest args (libc.cpp). `first_arg` is the index
// of the first variadic argument in the integer-register sequence. Shared so the
// stdio handlers in io.cpp can format without duplicating the conversion code.
std::string format_args(Cpu& c, const std::string& fmt, int first_arg);

// Log timestamps. Trace lines ([sfx]/[aud]/[trace]/[args]) are prefixed with the
// milliseconds since process start, so a burst can be read as a timeline (e.g.
// one SFX per frame vs one per second) and correlated across subsystems.
uint64_t log_ms();  // ms since the first call (process start)
const char* log_ts(); // log_ms(), right-aligned for a line prefix ("   1234")

void gl_set_window(void* sdl_window); // for sglSwapBuffers -> SDL_GL_SwapWindow
// Runs just before each buffer swap. The app calls sglSwapBuffers itself, so
// this is the only place a host overlay can land inside the guest's frame.
void gl_set_preswap(std::function<void(Cpu&)> fn);
void gl_present_canvas(Cpu& cpu, uint32_t pixels, int width, int height);

} // namespace hle
