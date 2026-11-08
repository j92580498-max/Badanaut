#include "cpu.h"
#include "dispatch.h"
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <functional>
#include <string>
#include <vector>

// Forward libc/libm/__aeabi_*/heap imports to the host runtime, operating on
// guest memory. Enough breadth to get the binary through static init + boot.

namespace {

// ---- heap ----
void h_alloc(Cpu& c) { c.set_ret32(c.alloc(c.arg32(0))); } // malloc/Osp_allocEx(size,...)
void h_calloc(Cpu& c) {                                    // Osp_callocEx(n,size,...)
    uint32_t n = c.arg32(0), sz = c.arg32(1), total = n * sz;
    uint32_t p = c.alloc(total);
    std::vector<uint8_t> z(total, 0);
    if (total) {
        c.write_mem(p, z.data(), total);
    }
    c.set_ret32(p);
}
void h_realloc(Cpu& c) { // Osp_reallocEx(ptr,size,...)
    uint32_t old = c.arg32(0), sz = c.arg32(1);
    uint32_t p = c.alloc(sz);
    if (old && sz) {
        std::vector<uint8_t> b(sz);
        c.read_mem(old, b.data(), sz);
        c.write_mem(p, b.data(), sz);
    }
    c.set_ret32(p);
}
void h_new(Cpu& c) { c.set_ret32(c.alloc(c.arg32(0))); }
void h_free(Cpu& c) { /* bump allocator: no-op */
}

// ---- mem* ----
void h_memcpy(Cpu& c) {
    uint32_t d = c.arg32(0), s = c.arg32(1), n = c.arg32(2);
    if (n) {
        std::vector<uint8_t> b(n);
        c.read_mem(s, b.data(), n);
        c.write_mem(d, b.data(), n);
    }
    c.set_ret32(d);
}
void h_memmove(Cpu& c) { h_memcpy(c); }
void h_memset(Cpu& c) {
    uint32_t d = c.arg32(0), v = c.arg32(1), n = c.arg32(2);
    if (n) {
        std::vector<uint8_t> b(n, (uint8_t)v);
        c.write_mem(d, b.data(), n);
    }
    c.set_ret32(d);
}
void h_memcmp(Cpu& c) {
    uint32_t a = c.arg32(0), b = c.arg32(1), n = c.arg32(2);
    std::vector<uint8_t> x(n), y(n);
    if (n) {
        c.read_mem(a, x.data(), n);
        c.read_mem(b, y.data(), n);
    }
    c.set_ret32((uint32_t)memcmp(x.data(), y.data(), n));
}

// ---- str* ----
void h_strlen(Cpu& c) { c.set_ret32(c.read_cstr(c.arg32(0)).size()); }
void h_strcmp(Cpu& c) { c.set_ret32((uint32_t)strcmp(c.read_cstr(c.arg32(0)).c_str(), c.read_cstr(c.arg32(1)).c_str())); }
void h_strcpy(Cpu& c) {
    std::string s = c.read_cstr(c.arg32(1));
    c.write_mem(c.arg32(0), s.c_str(), s.size() + 1);
    c.set_ret32(c.arg32(0));
}
void h_strncpy(Cpu& c) {
    uint32_t d = c.arg32(0);
    std::string s = c.read_cstr(c.arg32(1));
    uint32_t n = c.arg32(2);
    std::vector<char> buf(n, 0);
    for (uint32_t i = 0; i < n; i++) {
        buf[i] = i < s.size() ? s[i] : 0;
    }
    c.write_mem(d, buf.data(), n);
    c.set_ret32(d);
}
void h_strcat(Cpu& c) {
    uint32_t d = c.arg32(0);
    std::string a = c.read_cstr(d), b = c.read_cstr(c.arg32(1));
    std::string r = a + b;
    c.write_mem(d, r.c_str(), r.size() + 1);
    c.set_ret32(d);
}
void h_strncat(Cpu& c) {
    uint32_t d = c.arg32(0);
    std::string a = c.read_cstr(d), b = c.read_cstr(c.arg32(1));
    uint32_t n = c.arg32(2);
    std::string r = a + b.substr(0, n);
    c.write_mem(d, r.c_str(), r.size() + 1);
    c.set_ret32(d);
}
void h_strncmp(Cpu& c) {
    std::string a = c.read_cstr(c.arg32(0)), b = c.read_cstr(c.arg32(1));
    uint32_t n = c.arg32(2);
    c.set_ret32((uint32_t)strncmp(a.c_str(), b.c_str(), n));
}
// PvZ keys its resource map on std::string with a StringLessNoCase comparator,
// which calls this. Unregistered, it returned 0 -- every key compared equal, the
// red-black tree collapsed and every image lookup came back null.
void h_strcasecmp(Cpu& c) {
    std::string a = c.read_cstr(c.arg32(0)), b = c.read_cstr(c.arg32(1));
    size_t n = std::max(a.size(), b.size()) + 1;
    for (size_t i = 0; i < n; i++) {
        int ca = tolower((unsigned char)(i < a.size() ? a[i] : 0));
        int cb = tolower((unsigned char)(i < b.size() ? b[i] : 0));
        if (ca != cb) {
            c.set_ret32((uint32_t)(ca - cb));
            return;
        }
        if (!ca) {
            break;
        }
    }
    c.set_ret32(0);
}
void h_strncasecmp(Cpu& c) {
    std::string a = c.read_cstr(c.arg32(0)), b = c.read_cstr(c.arg32(1));
    uint32_t n = c.arg32(2);
    int r = 0;
    for (uint32_t i = 0; i < n; i++) {
        int ca = tolower((unsigned char)(i < a.size() ? a[i] : 0));
        int cb = tolower((unsigned char)(i < b.size() ? b[i] : 0));
        if (ca != cb) {
            r = ca - cb;
            break;
        }
        if (!ca) {
            break;
        }
    }
    c.set_ret32((uint32_t)r);
}
void h_strchr(Cpu& c) {
    uint32_t s = c.arg32(0);
    int ch = c.arg32(1);
    std::string str = c.read_cstr(s);
    auto p = str.find((char)ch);
    c.set_ret32(p == std::string::npos ? 0 : s + p);
}
void h_strrchr(Cpu& c) {
    uint32_t s = c.arg32(0);
    int ch = c.arg32(1);
    std::string str = c.read_cstr(s);
    auto p = str.rfind((char)ch);
    c.set_ret32(p == std::string::npos ? 0 : s + p);
}
void h_strstr(Cpu& c) {
    uint32_t s = c.arg32(0);
    std::string h = c.read_cstr(s), n = c.read_cstr(c.arg32(1));
    auto p = h.find(n);
    c.set_ret32(p == std::string::npos ? 0 : s + p);
}
void h_memchr(Cpu& c) {
    uint32_t s = c.arg32(0);
    int ch = c.arg32(1);
    uint32_t n = c.arg32(2);
    std::vector<uint8_t> b(n);
    if (n) {
        c.read_mem(s, b.data(), n);
    }
    for (uint32_t i = 0; i < n; i++) {
        if (b[i] == (uint8_t)ch) {
            c.set_ret32(s + i);
            return;
        }
    }
    c.set_ret32(0);
}
void h_strspn(Cpu& c) {
    c.set_ret32((uint32_t)c.read_cstr(c.arg32(0)).find_first_not_of(c.read_cstr(c.arg32(1))));
}
void h_strcspn(Cpu& c) {
    std::string s = c.read_cstr(c.arg32(0));
    size_t p = s.find_first_of(c.read_cstr(c.arg32(1)));
    c.set_ret32((uint32_t)(p == std::string::npos ? s.size() : p));
}
// strtok keeps its cursor between calls, as guest addresses.
void h_strtok(Cpu& c) {
    static uint32_t save = 0;
    uint32_t s = c.arg32(0) ? c.arg32(0) : save;
    if (!s) {
        c.set_ret32(0);
        return;
    }
    std::string delim = c.read_cstr(c.arg32(1));
    std::string str = c.read_cstr(s);
    size_t b = str.find_first_not_of(delim);
    if (b == std::string::npos) {
        save = 0;
        c.set_ret32(0);
        return;
    }
    size_t e = str.find_first_of(delim, b);
    if (e == std::string::npos) {
        save = 0;
    } else {
        uint8_t z = 0;
        c.write_mem(s + (uint32_t)e, &z, 1); // terminate this token in place
        save = s + (uint32_t)e + 1;
    }
    c.set_ret32(s + (uint32_t)b);
}
void h_atol(Cpu& c) { c.set_ret32((uint32_t)atol(c.read_cstr(c.arg32(0)).c_str())); }

// ---- time ----
void h_time(Cpu& c) {
    uint32_t t = c.arg32(0);
    uint32_t now = (uint32_t)time(nullptr);
    if (t) {
        c.write_u32(t, now);
    }
    c.set_ret32(now);
}
void h_localtime(Cpu& c) {
    static uint32_t buf = 0;
    uint32_t tp = c.arg32(0);
    time_t t = tp ? (time_t)c.read_u32(tp) : time(nullptr);
    struct tm* lt = localtime(&t);
    if (!buf) {
        buf = c.alloc(64, 4);
    }
    int v[9] = {lt->tm_sec, lt->tm_min, lt->tm_hour, lt->tm_mday, lt->tm_mon, lt->tm_year, lt->tm_wday, lt->tm_yday, lt->tm_isdst};
    for (int i = 0; i < 9; i++) {
        c.write_u32(buf + i * 4, v[i]);
    }
    c.set_ret32(buf);
}
void h_setlocale(Cpu& c) {
    static uint32_t s = 0;
    if (!s) {
        s = c.alloc(4);
        c.write_mem(s, "C", 2);
    }
    c.set_ret32(s);
}

// printf-style core. `next` fetches the next 32-bit arg; `nextd` the next double.
std::string fmt_core(Cpu& c, const std::string& fmt, std::function<uint32_t()> next, std::function<double()> nextd) {
    std::string out;
    char tmp[256];
    for (size_t i = 0; i < fmt.size(); i++) {
        if (fmt[i] != '%') {
            out += fmt[i];
            continue;
        }
        std::string spec = "%";
        i++;
        while (i < fmt.size() && strchr("-+ #0", fmt[i])) {
            spec += fmt[i++]; // flags
        }
        while (i < fmt.size() && (isdigit((unsigned char)fmt[i]) || fmt[i] == '*')) {
            if (fmt[i] == '*') {
                spec += std::to_string((int)next());
            } else {
                spec += fmt[i];
            }
            i++;
        }
        if (i < fmt.size() && fmt[i] == '.') {
            spec += '.';
            i++;
            while (i < fmt.size() && (isdigit((unsigned char)fmt[i]) || fmt[i] == '*')) {
                if (fmt[i] == '*') {
                    spec += std::to_string((int)next());
                } else {
                    spec += fmt[i];
                }
                i++;
            }
        }
        while (i < fmt.size() && strchr("lhLqjzt", fmt[i])) {
            i++; // skip length mods (treat as 32-bit)
        }
        if (i >= fmt.size()) {
            break;
        }
        char conv = fmt[i];
        switch (conv) {
        case 'd':
        case 'i':
        case 'u':
        case 'x':
        case 'X':
        case 'o':
        case 'c':
        case 'p':
            spec += (conv == 'p' ? std::string("x") : std::string(1, conv));
            snprintf(tmp, sizeof tmp, spec.c_str(), next());
            out += tmp;
            break;
        case 'e':
        case 'E':
        case 'f':
        case 'F':
        case 'g':
        case 'G':
            spec += conv;
            snprintf(tmp, sizeof tmp, spec.c_str(), nextd());
            out += tmp;
            break;
        case 's': {
            spec += 's';
            std::string s = c.read_cstr(next());
            snprintf(tmp, sizeof tmp, spec.c_str(), s.c_str());
            out += tmp;
            break;
        }
        case '%': out += '%'; break;
        default:
            out += '%';
            out += conv;
            break;
        }
    }
    return out;
}
void write_out(Cpu& c, uint32_t dst, uint32_t size, const std::string& s) {
    uint32_t n = (size && s.size() + 1 > size) ? size - 1 : s.size();
    if (n) {
        c.write_mem(dst, s.data(), n);
    }
    uint8_t z = 0;
    if (size) {
        c.write_mem(dst + n, &z, 1);
    }
}
void h_vsnprintf(Cpu& c) {
    uint32_t out = c.arg32(0), size = c.arg32(1);
    std::string fmt = c.read_cstr(c.arg32(2));
    uint32_t va = c.arg32(3);
    uint32_t p = va;
    auto next = [&]() { uint32_t v = c.read_u32(p); p += 4; return v; };
    auto nextd = [&]() { p = (p + 7) & ~7u; uint64_t b = (uint64_t)c.read_u32(p) | ((uint64_t)c.read_u32(p + 4) << 32); p += 8; double d; memcpy(&d, &b, 8); return d; };
    std::string r = fmt_core(c, fmt, next, nextd);
    write_out(c, out, size, r);
    c.set_ret32(r.size());
}
void h_snprintf(Cpu& c) {
    uint32_t out = c.arg32(0), size = c.arg32(1);
    std::string fmt = c.read_cstr(c.arg32(2));
    int idx = 3;
    auto next = [&]() { return c.arg32(idx++); };
    auto nextd = [&]() { if (idx & 1){ idx++;
} uint64_t b = (uint64_t)c.arg32(idx) | ((uint64_t)c.arg32(idx + 1) << 32); idx += 2; double d; memcpy(&d, &b, 8); return d; }; // variadic doubles: core regs, 8-aligned
    std::string r = fmt_core(c, fmt, next, nextd);
    write_out(c, out, size, r);
    c.set_ret32(r.size());
}
void h_sprintf(Cpu& c) {
    uint32_t out = c.arg32(0);
    std::string fmt = c.read_cstr(c.arg32(1));
    int idx = 2;
    auto next = [&]() { return c.arg32(idx++); };
    auto nextd = [&]() { if (idx & 1) { idx++; } uint64_t b = (uint64_t)c.arg32(idx) | ((uint64_t)c.arg32(idx + 1) << 32); idx += 2; double d; memcpy(&d, &b, 8); return d; };
    std::string r = fmt_core(c, fmt, next, nextd);
    write_out(c, out, 0, r);
    uint8_t z = 0;
    c.write_mem(out + r.size(), &z, 1);
    c.set_ret32((uint32_t)r.size());
}
void h_vsprintf(Cpu& c) {
    uint32_t out = c.arg32(0);
    std::string fmt = c.read_cstr(c.arg32(1));
    uint32_t p = c.arg32(2);
    auto next = [&]() { uint32_t v = c.read_u32(p); p += 4; return v; };
    auto nextd = [&]() { p = (p + 7) & ~7u; uint64_t b = (uint64_t)c.read_u32(p) | ((uint64_t)c.read_u32(p + 4) << 32); p += 8; double d; memcpy(&d, &b, 8); return d; };
    std::string r = fmt_core(c, fmt, next, nextd);
    write_out(c, out, 0, r);
    uint8_t z = 0;
    c.write_mem(out + r.size(), &z, 1);
    c.set_ret32((uint32_t)r.size());
}
// Number parsing. Stubbed, these returned 0 -- every numeric field the resource
// files carry read back as zero.
void h_atoi(Cpu& c) { c.set_ret32((uint32_t)atoi(c.read_cstr(c.arg32(0)).c_str())); }
void h_atof(Cpu& c) { c.set_retf64(atof(c.read_cstr(c.arg32(0)).c_str())); }
void h_strtol(Cpu& c) {
    std::string s = c.read_cstr(c.arg32(0));
    char* end = nullptr;
    long v = strtol(s.c_str(), &end, (int)c.arg32(2));
    if (uint32_t pend = c.arg32(1)) {
        c.write_u32(pend, c.arg32(0) + (uint32_t)(end - s.c_str()));
    }
    c.set_ret32((uint32_t)v);
}
void h_strtoul(Cpu& c) {
    std::string s = c.read_cstr(c.arg32(0));
    char* end = nullptr;
    unsigned long v = strtoul(s.c_str(), &end, (int)c.arg32(2));
    if (uint32_t pend = c.arg32(1)) {
        c.write_u32(pend, c.arg32(0) + (uint32_t)(end - s.c_str()));
    }
    c.set_ret32((uint32_t)v);
}
void h_strtod(Cpu& c) {
    std::string s = c.read_cstr(c.arg32(0));
    char* end = nullptr;
    double v = strtod(s.c_str(), &end);
    if (uint32_t pend = c.arg32(1)) {
        c.write_u32(pend, c.arg32(0) + (uint32_t)(end - s.c_str()));
    }
    c.set_retf64(v);
}
void h_tolower(Cpu& c) { c.set_ret32(tolower((int)c.arg32(0))); }
void h_toupper(Cpu& c) { c.set_ret32(toupper((int)c.arg32(0))); }
// ctype -- XML/text parsers depend on these to tokenize
void h_isalpha(Cpu& c) { c.set_ret32(isalpha((int)c.arg32(0)) ? 1 : 0); }
void h_isalnum(Cpu& c) { c.set_ret32(isalnum((int)c.arg32(0)) ? 1 : 0); }
void h_isspace(Cpu& c) { c.set_ret32(isspace((int)c.arg32(0)) ? 1 : 0); }
void h_isdigit(Cpu& c) { c.set_ret32(isdigit((int)c.arg32(0)) ? 1 : 0); }
void h_isupper(Cpu& c) { c.set_ret32(isupper((int)c.arg32(0)) ? 1 : 0); }
void h_islower(Cpu& c) { c.set_ret32(islower((int)c.arg32(0)) ? 1 : 0); }
void h_isxdigit(Cpu& c) { c.set_ret32(isxdigit((int)c.arg32(0)) ? 1 : 0); }
void h_ispunct(Cpu& c) { c.set_ret32(ispunct((int)c.arg32(0)) ? 1 : 0); }
void h_isprint(Cpu& c) { c.set_ret32(isprint((int)c.arg32(0)) ? 1 : 0); }
void h_iscntrl(Cpu& c) { c.set_ret32(iscntrl((int)c.arg32(0)) ? 1 : 0); }
void h_clock(Cpu& c) { c.set_ret32((uint32_t)clock()); }

// ---- math (VFP) ----
void h_sinf(Cpu& c) { c.set_retf32(sinf(c.argf32(0))); }
void h_cosf(Cpu& c) { c.set_retf32(cosf(c.argf32(0))); }
void h_sqrtf(Cpu& c) { c.set_retf32(sqrtf(c.argf32(0))); }
void h_sqrt(Cpu& c) { c.set_retf64(sqrt(c.argf64(0))); }
void h_floorf(Cpu& c) { c.set_retf32(floorf(c.argf32(0))); }
void h_ceilf(Cpu& c) { c.set_retf32(ceilf(c.argf32(0))); }
void h_fmodf(Cpu& c) { c.set_retf32(fmodf(c.argf32(0), c.argf32(1))); }
void h_powf(Cpu& c) { c.set_retf32(powf(c.argf32(0), c.argf32(1))); }
void h_atan2f(Cpu& c) { c.set_retf32(atan2f(c.argf32(0), c.argf32(1))); }
void h_asinf(Cpu& c) { c.set_retf32(asinf(c.argf32(0))); }
void h_acosf(Cpu& c) { c.set_retf32(acosf(c.argf32(0))); }
void h_atanf(Cpu& c) { c.set_retf32(atanf(c.argf32(0))); }
void h_expf(Cpu& c) { c.set_retf32(expf(c.argf32(0))); }
void h_logf(Cpu& c) { c.set_retf32(logf(c.argf32(0))); }
void h_fabsf(Cpu& c) { c.set_retf32(fabsf(c.argf32(0))); }
void h_floor(Cpu& c) { c.set_retf64(floor(c.argf64(0))); }
void h_ceil(Cpu& c) { c.set_retf64(ceil(c.argf64(0))); }
void h_pow(Cpu& c) { c.set_retf64(pow(c.argf64(0), c.argf64(1))); }
void h_fmod(Cpu& c) { c.set_retf64(fmod(c.argf64(0), c.argf64(1))); }
void h_sin(Cpu& c) { c.set_retf64(sin(c.argf64(0))); }
void h_cos(Cpu& c) { c.set_retf64(cos(c.argf64(0))); }
void h_asin(Cpu& c) { c.set_retf64(asin(c.argf64(0))); }
void h_acos(Cpu& c) { c.set_retf64(acos(c.argf64(0))); }
void h_atan(Cpu& c) { c.set_retf64(atan(c.argf64(0))); }
void h_atan2(Cpu& c) { c.set_retf64(atan2(c.argf64(0), c.argf64(1))); }
void h_exp(Cpu& c) { c.set_retf64(exp(c.argf64(0))); }
void h_log(Cpu& c) { c.set_retf64(log(c.argf64(0))); }
void h_log10(Cpu& c) { c.set_retf64(log10(c.argf64(0))); }

// ---- __aeabi integer divide ----
void h_idiv(Cpu& c) {
    int32_t a = c.arg32(0), b = c.arg32(1);
    c.set_ret32(b ? static_cast<int64_t>(a) / b : 0); // INT_MIN/-1 wraps without C++ overflow
}
void h_uidiv(Cpu& c) {
    uint32_t a = c.arg32(0), b = c.arg32(1);
    c.set_ret32(b ? a / b : 0);
}
void h_idivmod(Cpu& c) {
    int32_t a = c.arg32(0), b = c.arg32(1);
    c.set_reg(UC_ARM_REG_R0, b ? static_cast<int64_t>(a) / b : 0);
    c.set_reg(UC_ARM_REG_R1, b ? static_cast<int64_t>(a) % b : 0);
}
void h_uidivmod(Cpu& c) {
    uint32_t a = c.arg32(0), b = c.arg32(1);
    c.set_reg(UC_ARM_REG_R0, b ? a / b : 0);
    c.set_reg(UC_ARM_REG_R1, b ? a % b : 0);
}

// ---- __aeabi soft-float runtime (single + double precision) ----
// Soft-float binaries pass float/double args as integer bit patterns in core
// registers and call these helpers for every arithmetic op.
static float f_of(uint32_t b) {
    float f;
    memcpy(&f, &b, 4);
    return f;
}
static uint32_t b_of(float f) {
    uint32_t b;
    memcpy(&b, &f, 4);
    return b;
}
static double d_of(uint64_t b) {
    double d;
    memcpy(&d, &b, 8);
    return d;
}
static uint64_t b_of(double d) {
    uint64_t b;
    memcpy(&b, &d, 8);
    return b;
}
static double darg(Cpu& c, int i) { return d_of((uint64_t)c.arg32(2 * i) | ((uint64_t)c.arg32(2 * i + 1) << 32)); }
static void dret(Cpu& c, double v) { c.set_ret64(b_of(v)); }

void h_fadd(Cpu& c) { c.set_ret32(b_of(f_of(c.arg32(0)) + f_of(c.arg32(1)))); }
void h_fsub(Cpu& c) { c.set_ret32(b_of(f_of(c.arg32(0)) - f_of(c.arg32(1)))); }
void h_fmul(Cpu& c) { c.set_ret32(b_of(f_of(c.arg32(0)) * f_of(c.arg32(1)))); }
void h_fdiv(Cpu& c) {
    float b = f_of(c.arg32(1));
    c.set_ret32(b_of(b ? f_of(c.arg32(0)) / b : 0.f));
}
void h_dadd(Cpu& c) { dret(c, darg(c, 0) + darg(c, 1)); }
void h_dsub(Cpu& c) { dret(c, darg(c, 0) - darg(c, 1)); }
void h_dmul(Cpu& c) { dret(c, darg(c, 0) * darg(c, 1)); }
void h_ddiv(Cpu& c) {
    double b = darg(c, 1);
    dret(c, b ? darg(c, 0) / b : 0.0);
}
void h_i2f(Cpu& c) { c.set_ret32(b_of((float)(int32_t)c.arg32(0))); }
void h_ui2f(Cpu& c) { c.set_ret32(b_of((float)c.arg32(0))); }
void h_f2iz(Cpu& c) { c.set_ret32((uint32_t)(int32_t)f_of(c.arg32(0))); }
void h_i2d(Cpu& c) { dret(c, (double)(int32_t)c.arg32(0)); }
void h_d2iz(Cpu& c) { c.set_ret32((uint32_t)(int32_t)darg(c, 0)); }
void h_f2d(Cpu& c) { dret(c, (double)f_of(c.arg32(0))); }
// 64-bit int conversions. The __aeabi_ helpers use the base (soft-float) ABI
// even in a hard-float binary: the long long rides in r0:r1, the double comes
// back in r0:r1.
static int64_t larg(Cpu& c, int i) {
    return (int64_t)((uint64_t)c.arg32(2 * i) | ((uint64_t)c.arg32(2 * i + 1) << 32));
}
void h_l2d(Cpu& c) { dret(c, (double)larg(c, 0)); }
void h_ul2d(Cpu& c) { dret(c, (double)(uint64_t)larg(c, 0)); }
void h_l2f(Cpu& c) { c.set_ret32(b_of((float)larg(c, 0))); }
void h_ul2f(Cpu& c) { c.set_ret32(b_of((float)(uint64_t)larg(c, 0))); }
void h_d2lz(Cpu& c) { c.set_ret64((uint64_t)(int64_t)darg(c, 0)); }
void h_f2lz(Cpu& c) { c.set_ret64((uint64_t)(int64_t)f_of(c.arg32(0))); }
void h_d2f(Cpu& c) { c.set_ret32(b_of((float)darg(c, 0))); }
// comparisons: __aeabi_fcmp* return 1 when the predicate holds, else 0
void h_fcmplt(Cpu& c) { c.set_ret32(f_of(c.arg32(0)) < f_of(c.arg32(1))); }
void h_fcmple(Cpu& c) { c.set_ret32(f_of(c.arg32(0)) <= f_of(c.arg32(1))); }
void h_fcmpge(Cpu& c) { c.set_ret32(f_of(c.arg32(0)) >= f_of(c.arg32(1))); }
void h_fcmpgt(Cpu& c) { c.set_ret32(f_of(c.arg32(0)) > f_of(c.arg32(1))); }
void h_fcmpeq(Cpu& c) { c.set_ret32(f_of(c.arg32(0)) == f_of(c.arg32(1))); }
void h_dcmplt(Cpu& c) { c.set_ret32(darg(c, 0) < darg(c, 1)); }
void h_dcmple(Cpu& c) { c.set_ret32(darg(c, 0) <= darg(c, 1)); }
void h_dcmpge(Cpu& c) { c.set_ret32(darg(c, 0) >= darg(c, 1)); }
void h_dcmpgt(Cpu& c) { c.set_ret32(darg(c, 0) > darg(c, 1)); }
void h_dcmpeq(Cpu& c) { c.set_ret32(darg(c, 0) == darg(c, 1)); }
void h_tanf(Cpu& c) { c.set_retf32(tanf(c.argf32(0))); }
void h_tan(Cpu& c) { c.set_retf64(tan(c.argf64(0))); }

// ---- C++ ABI / guards ----
void h_guard_acquire(Cpu& c) { // return 1 if caller must run the init
    uint32_t g = c.arg32(0);
    uint8_t b;
    c.read_mem(g, &b, 1);
    c.set_ret32((b & 1) ? 0 : 1);
}
void h_guard_release(Cpu& c) {
    uint32_t g = c.arg32(0);
    uint8_t b = 1;
    c.write_mem(g, &b, 1);
}
void h_noop(Cpu& c) { c.set_ret32(0); }
void h_rand(Cpu& c) { c.set_ret32(rand()); }
void h_srand(Cpu& c) {
    srand(c.arg32(0));
    c.set_ret32(0);
}
// wide ctype: the guest's wchar_t is 4 bytes, the host's is 2 on Windows, so
// classify the code point directly instead of calling the host isw* on it.
void h_iswspace(Cpu& c) {
    uint32_t w = c.arg32(0);
    c.set_ret32(w < 128 && isspace((int)w) ? 1 : 0);
}
void h_towlower(Cpu& c) {
    uint32_t w = c.arg32(0);
    c.set_ret32(w < 128 ? (uint32_t)tolower((int)w) : w);
}

} // namespace

namespace hle {
std::string format_args(Cpu& c, const std::string& fmt, int first_arg) {
    int idx = first_arg;
    auto next = [&]() { return c.arg32(idx++); };
    auto nextd = [&]() { if (idx & 1) { idx++; } uint64_t b = (uint64_t)c.arg32(idx) | ((uint64_t)c.arg32(idx + 1) << 32); idx += 2; double d; memcpy(&d, &b, 8); return d; };
    return fmt_core(c, fmt, next, nextd);
}

void register_libc() {
    register_handler("malloc", h_alloc);
    register_handler("_Z11Osp_allocExjPKci", h_alloc);       // Osp_allocEx(size,file,line)
    register_handler("_Z9Osp_allocj", h_alloc);                // Osp_alloc(size)
    register_handler("_Z12Osp_callocExjjPKci", h_calloc);    // Osp_callocEx(n,size,file,line)
    register_handler("_Z13Osp_reallocExPvjPKci", h_realloc); // Osp_reallocEx(ptr,size,file,line)
    register_handler("_Z8Osp_freePv", h_free);               // Osp_free(ptr)
    register_handler("free", h_free);
    register_handler("_Znwj", h_new);   // operator new(unsigned int)
    register_handler("_Znaj", h_new);   // operator new[](unsigned int)
    register_handler("_ZdlPv", h_free); // operator delete(void*)
    register_handler("_ZdaPv", h_free); // operator delete[](void*)

    register_handler("memcpy", h_memcpy);
    register_handler("memmove", h_memmove);
    register_handler("memset", h_memset);
    register_handler("memcmp", h_memcmp);
    register_handler("strlen", h_strlen);
    register_handler("strcmp", h_strcmp);
    register_handler("strcpy", h_strcpy);
    register_handler("strncpy", h_strncpy);
    register_handler("strcat", h_strcat);
    register_handler("strncat", h_strncat);
    register_handler("strncmp", h_strncmp);
    register_handler("strcasecmp", h_strcasecmp);
    register_handler("strncasecmp", h_strncasecmp);
    register_handler("strchr", h_strchr);
    register_handler("strrchr", h_strrchr);
    register_handler("strstr", h_strstr);
    register_handler("memchr", h_memchr);
    register_handler("atol", h_atol);
    register_handler("time", h_time);
    register_handler("localtime", h_localtime);
    register_handler("gmtime", h_localtime);
    register_handler("setlocale", h_setlocale);
    register_handler("vsnprintf", h_vsnprintf);
    register_handler("snprintf", h_snprintf);
    register_handler("sprintf", h_sprintf);
    register_handler("vsprintf", h_vsprintf);
    register_handler("atoi", h_atoi);
    register_handler("atof", h_atof);
    register_handler("strtol", h_strtol);
    register_handler("strtoul", h_strtoul);
    register_handler("strtod", h_strtod);
    register_handler("tolower", h_tolower);
    register_handler("toupper", h_toupper);
    register_handler("clock", h_clock);
    register_handler("isalpha", h_isalpha);
    register_handler("isalnum", h_isalnum);
    register_handler("isspace", h_isspace);
    register_handler("isdigit", h_isdigit);
    register_handler("isupper", h_isupper);
    register_handler("islower", h_islower);
    register_handler("isxdigit", h_isxdigit);
    register_handler("ispunct", h_ispunct);
    register_handler("isprint", h_isprint);
    register_handler("iscntrl", h_iscntrl);

    register_handler("sinf", h_sinf);
    register_handler("cosf", h_cosf);
    register_handler("sqrtf", h_sqrtf);
    register_handler("sqrt", h_sqrt);
    register_handler("floorf", h_floorf);
    register_handler("ceilf", h_ceilf);
    register_handler("fmodf", h_fmodf);
    register_handler("powf", h_powf);
    register_handler("atan2f", h_atan2f);
    register_handler("asinf", h_asinf);
    register_handler("acosf", h_acosf);
    register_handler("atanf", h_atanf);
    register_handler("expf", h_expf);
    register_handler("logf", h_logf);
    register_handler("fabsf", h_fabsf);
    register_handler("floor", h_floor);
    register_handler("ceil", h_ceil);
    register_handler("pow", h_pow);
    register_handler("fmod", h_fmod);
    register_handler("sin", h_sin);
    register_handler("cos", h_cos);
    register_handler("asin", h_asin);
    register_handler("acos", h_acos);
    register_handler("atan", h_atan);
    register_handler("atan2", h_atan2);
    register_handler("exp", h_exp);
    register_handler("log", h_log);
    register_handler("log10", h_log10);

    register_handler("__aeabi_idiv", h_idiv);
    register_handler("__aeabi_uidiv", h_uidiv);
    register_handler("__aeabi_idivmod", h_idivmod);
    register_handler("__aeabi_uidivmod", h_uidivmod);

    register_handler("__aeabi_fadd", h_fadd);
    register_handler("__aeabi_fsub", h_fsub);
    register_handler("__aeabi_fmul", h_fmul);
    register_handler("__aeabi_fdiv", h_fdiv);
    register_handler("__aeabi_dadd", h_dadd);
    register_handler("__aeabi_dsub", h_dsub);
    register_handler("__aeabi_dmul", h_dmul);
    register_handler("__aeabi_ddiv", h_ddiv);
    register_handler("__aeabi_i2f", h_i2f);
    register_handler("__aeabi_ui2f", h_ui2f);
    register_handler("__aeabi_f2iz", h_f2iz);
    register_handler("__aeabi_i2d", h_i2d);
    register_handler("__aeabi_d2iz", h_d2iz);
    register_handler("__aeabi_f2d", h_f2d);
    register_handler("__aeabi_l2d", h_l2d);
    register_handler("__aeabi_ul2d", h_ul2d);
    register_handler("__aeabi_l2f", h_l2f);
    register_handler("__aeabi_ul2f", h_ul2f);
    register_handler("__aeabi_d2lz", h_d2lz);
    register_handler("__aeabi_f2lz", h_f2lz);
    register_handler("__aeabi_d2f", h_d2f);
    register_handler("__aeabi_fcmplt", h_fcmplt);
    register_handler("__aeabi_fcmple", h_fcmple);
    register_handler("__aeabi_fcmpge", h_fcmpge);
    register_handler("__aeabi_fcmpgt", h_fcmpgt);
    register_handler("__aeabi_fcmpeq", h_fcmpeq);
    register_handler("__aeabi_dcmplt", h_dcmplt);
    register_handler("__aeabi_dcmple", h_dcmple);
    register_handler("__aeabi_dcmpge", h_dcmpge);
    register_handler("__aeabi_dcmpgt", h_dcmpgt);
    register_handler("__aeabi_dcmpeq", h_dcmpeq);
    register_handler("tanf", h_tanf);
    register_handler("tan", h_tan);

    register_handler("__cxa_guard_acquire", h_guard_acquire);
    register_handler("__cxa_guard_release", h_guard_release);
    register_handler("__cxa_guard_abort", h_noop);
    register_handler("__aeabi_atexit", h_noop);
    register_handler("__cxa_atexit", h_noop);
    register_handler("__gxx_personality_v0", h_noop);
    register_handler("rand", h_rand);
    register_handler("srand", h_srand);
    register_handler("iswspace", h_iswspace);
    register_handler("towlower", h_towlower);
}
} // namespace hle
