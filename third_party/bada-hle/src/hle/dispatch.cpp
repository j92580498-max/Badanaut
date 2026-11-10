#include "dispatch.h"
#include "cpu.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <chrono>
#include <cstdlib>
#if defined(__has_include)
#  if __has_include(<cxxabi.h>)
#    include <cxxabi.h>
#    define BADA_HAVE_CXXABI 1
#  endif
#endif
#include <deque>
#include <map>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace hle {

static std::unordered_map<std::string, Handler> g_handlers;
static std::map<std::string, uint64_t> g_calls; // name -> count
static std::unordered_map<std::string, bool> g_warned_unknown;

struct CallTiming {
    uint64_t calls = 0;
    uint64_t nanoseconds = 0;
};
static std::unordered_map<std::string, CallTiming> g_call_timings;

// Optional timings distinguish host API work from time executing ARM code.
// Ordinary runs do not read the clock or allocate timing records.
class CallTimer {
public:
    explicit CallTimer(const std::string& name) {
        static const bool enabled = getenv("BADA_PROFILE_HLE") != nullptr;
        if (enabled) {
            timing_ = &g_call_timings[name];
            started_ = std::chrono::steady_clock::now();
        }
    }
    ~CallTimer() {
        if (!timing_) return;
        ++timing_->calls;
        timing_->nanoseconds += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - started_).count());
    }
private:
    CallTiming* timing_ = nullptr;
    std::chrono::steady_clock::time_point started_;
};

// Runtime call trace state (set_trace / trace_count).
static bool g_rt_trace = false;
static std::string g_rt_filter;
static uint64_t g_rt_count = 0;

void register_handler(const char* name, Handler h) { g_handlers[name] = h; }

// ---- log timestamps ----
// A steady clock (not SDL_GetTicks) so this stays usable from the HLE modules.
// The epoch is a dynamically-initialised global, i.e. process start, so boot
// work and frame-loop events share one timeline.
static const auto g_log_t0 = std::chrono::steady_clock::now();

uint64_t log_ms() {
    auto dt = std::chrono::steady_clock::now() - g_log_t0;
    return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(dt).count();
}

const char* log_ts() {
    static char buf[24];
    snprintf(buf, sizeof buf, "%7llu", (unsigned long long)log_ms());
    return buf;
}

void set_trace(bool on, const std::string& filter) {
    g_rt_trace = on;
    g_rt_filter = filter;
    if (on) {
        g_rt_count = 0;
    }
}

uint64_t trace_count() { return g_rt_count; }

// Trace-log ring buffer (see dispatch.h). Guarded by a mutex: trace lines are
// pushed from the guest/main thread while `tracelog` may dump from the same
// thread today, but the lock keeps it correct if that ever changes.
static std::deque<std::string> g_trace_log;
static uint64_t g_trace_dropped = 0;
static std::mutex g_trace_log_mtx;
static const size_t kTraceLogMax = 20000; // ~lines kept; oldest dropped when full

void trace_log_push(const std::string& line) {
    std::lock_guard<std::mutex> lk(g_trace_log_mtx);
    g_trace_log.push_back(line);
    while (g_trace_log.size() > kTraceLogMax) {
        g_trace_log.pop_front();
        g_trace_dropped++;
    }
}

size_t trace_log_dump(const std::string& path, bool clear, uint64_t* dropped) {
    std::lock_guard<std::mutex> lk(g_trace_log_mtx);
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) {
        return (size_t)-1;
    }
    for (const auto& l : g_trace_log) {
        fwrite(l.data(), 1, l.size(), f);
        fputc('\n', f);
    }
    fclose(f);
    size_t n = g_trace_log.size();
    if (dropped) {
        *dropped = g_trace_dropped;
    }
    if (clear) {
        g_trace_log.clear();
        g_trace_dropped = 0;
    }
    return n;
}

namespace {
// Minimal Itanium C++ ABI demangler, used as the fallback on toolchains without
// <cxxabi.h> (MSVC -- its UnDecorateSymbolName can't read Itanium names). Handles
// the constructs that appear in bada guest symbols: nested names, ctors/dtors,
// builtin/pointer/ref/const types, std substitution shortcuts, the S_/S<n>_
// substitution table, and template args. Best-effort: on anything it doesn't
// understand it clears `ok`, and the caller falls back to the raw mangled name.
struct Demangler {
    const std::string& s;
    size_t i = 0;
    std::vector<std::string> subs; // substitution table (S_, S0_, ...)
    bool ok = true;

    explicit Demangler(const std::string& str) : s(str) {}

    char peek() const { return i < s.size() ? s[i] : 0; }
    char get() { return i < s.size() ? s[i++] : 0; }
    static std::string tail(const std::string& q) { // unqualified part after last ::
        auto p = q.rfind("::");
        return p == std::string::npos ? q : q.substr(p + 2);
    }

    int number() {
        int n = 0;
        bool any = false;
        while (std::isdigit((unsigned char)peek())) {
            n = n * 10 + (get() - '0');
            any = true;
        }
        if (!any) {
            ok = false;
        }
        return n;
    }

    std::string source_name() {
        int n = number();
        if (!ok || n < 0 || i + (size_t)n > s.size()) {
            ok = false;
            return "";
        }
        std::string r = s.substr(i, n);
        i += n;
        return r;
    }

    // <substitution> := S_ | S <seq-id> _ | S <std-abbrev>
    std::string substitution() {
        get(); // 'S'
        char c = peek();
        if (c == '_') {
            get();
            if (subs.empty()) {
                ok = false;
                return "";
            }
            return subs[0];
        }
        if (std::isdigit((unsigned char)c) || (c >= 'A' && c <= 'Z')) {
            int id = 0;
            for (;;) {
                char d = peek();
                int v;
                if (std::isdigit((unsigned char)d)) {
                    v = d - '0';
                } else if (d >= 'A' && d <= 'Z') {
                    v = d - 'A' + 10;
                } else {
                    break;
                }
                id = id * 36 + v;
                get();
            }
            if (get() != '_') {
                ok = false;
                return "";
            }
            size_t idx = (size_t)id + 1; // S_ = 0, S0_ = 1, ...
            if (idx >= subs.size()) {
                ok = false;
                return "";
            }
            return subs[idx];
        }
        get(); // consume the std-abbrev letter
        switch (c) {
        case 't': return "std";
        case 'a': return "std::allocator";
        case 'b': return "std::basic_string";
        case 's': return "std::string";
        case 'i': return "std::istream";
        case 'o': return "std::ostream";
        case 'd': return "std::iostream";
        default: ok = false; return "";
        }
    }

    // <template-args> := I <type>+ E  -> "<a, b, ...>"
    std::string template_args() {
        get(); // 'I'
        std::string r = "<";
        bool first = true;
        while (ok && peek() && peek() != 'E') {
            if (!first) {
                r += ", ";
            }
            first = false;
            r += type();
        }
        if (get() != 'E') {
            ok = false;
        }
        return r + ">";
    }

    // <nested-name> := N [CV] <component>+ E   (component = source-name | ctor |
    // dtor | substitution | template-args). Builds the qualified scope and grows
    // the substitution table with each prefix.
    std::string nested_name() {
        get(); // 'N'
        while (peek() == 'r' || peek() == 'V' || peek() == 'K') {
            get(); // CV / ref qualifiers (ignored in output)
        }
        std::string full, last_simple;
        bool firstc = true;
        while (ok && peek() && peek() != 'E') {
            char c = peek();
            if (c == 'I') {
                std::string ta = template_args();
                full += ta;
                last_simple += ta;
                subs.push_back(full);
                continue;
            }
            std::string comp;
            bool add = true;
            if (c == 'C') {
                get();
                get(); // C<n>
                comp = last_simple; // ctor: class simple name
                add = false;
            } else if (c == 'D') {
                get();
                get(); // D<n>
                comp = "~" + last_simple;
                add = false;
            } else if (c == 'S') {
                comp = substitution();
            } else if (std::isdigit((unsigned char)c)) {
                comp = source_name();
            } else {
                ok = false;
                break;
            }
            if (!ok) {
                break;
            }
            full = firstc ? comp : full + "::" + comp;
            last_simple = tail(comp);
            firstc = false;
            if (add) {
                subs.push_back(full);
            }
        }
        if (get() != 'E') {
            ok = false;
        }
        return full;
    }

    // <type> -- enough of the grammar for params/template-args.
    std::string type() {
        char c = peek();
        if (c == 'P') {
            get();
            return type() + "*";
        }
        if (c == 'R') {
            get();
            return type() + "&";
        }
        if (c == 'O') {
            get();
            return type() + "&&";
        }
        if (c == 'K') {
            get();
            return "const " + type();
        }
        if (c == 'N') {
            std::string r = nested_name();
            if (peek() == 'I') {
                r += template_args();
            }
            return r;
        }
        if (c == 'S') {
            std::string r = substitution();
            if (peek() == 'I') {
                r += template_args();
            }
            return r;
        }
        if (std::isdigit((unsigned char)c)) {
            std::string r = source_name();
            if (peek() == 'I') {
                r += template_args();
            }
            subs.push_back(r);
            return r;
        }
        get();
        switch (c) {
        case 'v': return "void";
        case 'b': return "bool";
        case 'c': return "char";
        case 'a': return "signed char";
        case 'h': return "unsigned char";
        case 'w': return "wchar_t";
        case 's': return "short";
        case 't': return "unsigned short";
        case 'i': return "int";
        case 'j': return "unsigned int";
        case 'l': return "long";
        case 'm': return "unsigned long";
        case 'x': return "long long";
        case 'y': return "unsigned long long";
        case 'f': return "float";
        case 'd': return "double";
        case 'e': return "long double";
        default: ok = false; return "";
        }
    }

    std::string run() {
        if (s.compare(0, 2, "_Z") != 0) {
            return ""; // not an Itanium-mangled name
        }
        i = 2;
        std::string name;
        if (peek() == 'N') {
            name = nested_name();
        } else if (std::isdigit((unsigned char)peek())) {
            name = source_name();
            if (peek() == 'I') {
                name += template_args();
            }
        } else {
            return "";
        }
        if (!ok) {
            return "";
        }
        // Parse the parameter types (best-effort: on failure show "(...)").
        std::vector<std::string> params;
        size_t save = i;
        bool params_ok = true;
        while (peek()) {
            std::string t = type();
            if (!ok) {
                params_ok = false;
                break;
            }
            params.push_back(t);
        }
        if (!params_ok) {
            ok = true;
            i = save;
            return name + "(...)";
        }
        if (params.size() == 1 && params[0] == "void") {
            params.clear();
        }
        std::string out = name + "(";
        for (size_t k = 0; k < params.size(); k++) {
            out += (k ? ", " : "") + params[k];
        }
        return out + ")";
    }
};

std::string itanium_demangle(const std::string& name) {
    Demangler d(name);
    std::string r = d.run();
    return d.ok ? r : std::string();
}
} // namespace

std::string demangle(const std::string& name) {
#ifdef BADA_HAVE_CXXABI
    // Guest symbols use the Itanium C++ ABI mangling (GCC/Clang). Prefer the real
    // demangler when available (Linux); MSVC lacks it, so fall back to our own.
    int status = 0;
    char* d = abi::__cxa_demangle(name.c_str(), nullptr, nullptr, &status);
    if (status == 0 && d) {
        std::string r(d);
        free(d);
        return r;
    }
#endif
    std::string r = itanium_demangle(name);
    return r.empty() ? name : r;
}

void init_dispatch() {
    register_libc();
    register_gl();
    register_osp();
    register_io();
    register_ui();
    register_audio();
    printf("[hle] %zu handlers registered\n", g_handlers.size());
}

// Heuristic: an Itanium constructor mangling has the C1/C2/C3 (complete/base/
// allocating ctor) marker closing the nested name, i.e. "C1E"/"C2E"/"C3E".
static bool is_ctor_name(const std::string& n) {
    return n.find("C1E") != std::string::npos || n.find("C2E") != std::string::npos ||
           n.find("C3E") != std::string::npos;
}

void call(const std::string& name, Cpu& cpu) {
    CallTimer timing(name);
    static const bool g_trace = getenv("BADA_TRACE") != nullptr;
    if (g_trace) {
        fprintf(stderr, "[call] %s\n", name.c_str());
    }
    if (g_rt_trace && (g_rt_filter.empty() || name.find(g_rt_filter) != std::string::npos)) {
        g_rt_count++;
        std::string line = std::string(log_ts()) + " [trace] " + demangle(name);
        printf("%s\n", line.c_str());
        trace_log_push(line); // also buffer for retrieval via `tracelog`
    }
    g_calls[name]++;
    auto it = g_handlers.find(name);
    if (it != g_handlers.end()) {
        it->second(cpu);
        return;
    }

    // Default behaviour: Osp::* and everything else -> log once, return 0.
    if (!g_warned_unknown[name]) {
        g_warned_unknown[name] = true;
        printf("[hle] STUB %s\n", demangle(name).c_str());
    }
    // Unhandled constructor: leave a valid no-op vtable on `this` (r0) and return
    // it, so a later virtual call on the object lands on a no-op instead of
    // jumping through an uninitialized vtable (e.g. Asphalt's Slider/OverlayPanel
    // built by BVideoPlayer -> wild PC). Mirrors stub_ctor for registered stubs.
    if (is_ctor_name(name)) {
        uint32_t self = cpu.reg(UC_ARM_REG_R0);
        if (self) {
            cpu.write_u32(self, cpu.noop_vtable());
        }
        cpu.set_ret32(self);
        return;
    }
    cpu.set_ret32(0);
}

void dump_stats() {
    printf("\n===== HLE call report (viability trace) =====\n");
    // Sort by count desc.
    std::vector<std::pair<std::string, uint64_t>> v(g_calls.begin(), g_calls.end());
    std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.second > b.second; });
    for (auto& [name, cnt] : v) {
        bool handled = g_handlers.count(name) > 0;
        printf("  %8llu  %-7s %s\n", (unsigned long long)cnt,
               handled ? "[impl]" : "[stub]", demangle(name).c_str());
    }
    printf("  total distinct symbols called: %zu\n", v.size());
    if (!g_call_timings.empty()) {
        std::vector<std::pair<std::string, CallTiming>> timings(g_call_timings.begin(), g_call_timings.end());
        std::sort(timings.begin(), timings.end(), [](const auto& a, const auto& b) {
            return a.second.nanoseconds > b.second.nanoseconds;
        });
        uint64_t total = 0;
        for (const auto& entry : timings) total += entry.second.nanoseconds;
        printf("\n[profile] HLE host time: %.3f seconds\n", total / 1e9);
        for (size_t i = 0; i < std::min(timings.size(), size_t{20}); ++i) {
            const auto& entry = timings[i];
            printf("[profile] %9.2f ms %9llu calls %s\n", entry.second.nanoseconds / 1e6,
                   static_cast<unsigned long long>(entry.second.calls), demangle(entry.first).c_str());
        }
    }
}

} // namespace hle
