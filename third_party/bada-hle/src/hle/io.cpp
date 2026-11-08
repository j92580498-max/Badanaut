#include "cpu.h"
#include "dispatch.h"
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

// File-system + ByteBuffer HLE. The game loads assets (textures/fonts/meshes)
// from the FruitNinjaBada package via Osp::Io::File into Osp::Base::ByteBuffer.
//
// ByteBuffer's real layout (shared __BufferData + array start) is non-trivial,
// but the game only touches it through Osp methods we control, so we back it
// with a host-side registry keyed by the guest object address.
//
// Bada paths are CASE-INSENSITIVE; the host FS (Linux) is not, so each path
// component is resolved case-insensitively against the package directory.

namespace fs = std::filesystem;

namespace {

std::string g_pkg_root; // .../FruitNinjaBada
std::string g_home;     // writable /Home relocation (.../FruitNinjaBada/Home)

void mkdir_p(const std::string& path) {
    std::error_code ec;
    fs::create_directories(path, ec);
}

struct Buf {
    uint32_t data = 0;
    int cap = 0;
    int pos = 0;
    int limit = 0;
};
struct File {
    std::vector<uint8_t> data;
    size_t pos = 0;
};
std::map<uint32_t, Buf> g_bufs;           // ByteBuffer object -> backing store
std::map<uint32_t, File> g_files;         // File object -> loaded contents
std::map<uint32_t, uint64_t> g_attr_size; // FileAttributes object -> size

bool iequals(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); i++) {
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) {
            return false;
        }
    }
    return true;
}

// Resolve `rel` (relative to root) walking each component case-insensitively.
std::string ci_resolve(const std::string& root, const std::string& rel) try {
    fs::path cur = root;
    size_t i = 0;
    while (i < rel.size()) {
        while (i < rel.size() && (rel[i] == '/' || rel[i] == '\\')) {
            i++;
        }
        size_t j = i;
        while (j < rel.size() && rel[j] != '/' && rel[j] != '\\') {
            j++;
        }
        if (j == i) {
            break;
        }
        std::string comp = rel.substr(i, j - i);
        i = j;
        std::error_code ec;
        if (fs::exists(cur / comp, ec)) {
            cur /= comp;
            continue;
        }                  // exact (fast path)
        std::string match; // else case-insensitive scan
        for (auto& e : fs::directory_iterator(cur, ec)) {
            // u8string, not string: a name outside the ANSI code page makes
            // MSVC's narrowing conversion throw.
            std::string fn = e.path().filename().u8string();
            if (iequals(fn, comp)) {
                match = fn;
                break;
            }
        }
        if (ec || match.empty()) {
            return "";
        }
        cur /= match;
    }
    return cur.string();
} catch (const std::exception&) {
    // A guest path holding junk bytes cannot name a file. Report "not found"
    // instead of unwinding out through the emulator.
    return "";
}

// The guest path with a leading "/Home/" stripped (bada's app private dir).
std::string home_rel(const std::string& guest) {
    std::string g = guest;
    while (!g.empty() && (g[0] == '/' || g[0] == '\\')) {
        g.erase(g.begin());
    }
    if (g.size() >= 5 && iequals(g.substr(0, 5), "Home/")) {
        g = g.substr(5);
    }
    return g;
}

// Host path for WRITING a guest /Home path: under the writable home dir, with
// the sub-path preserved and parent dirs created. The game writes generated/
// cached data (and save files) here; reads must resolve back to the same place.
std::string write_path(const std::string& guest) {
    std::string full = g_home + "/" + home_rel(guest);
    if (auto s = full.find_last_of('/'); s != std::string::npos) {
        mkdir_p(full.substr(0, s));
    }
    return full;
}

// Resolve a guest path to a host file (case-insensitive). Check the writable
// /Home relocation FIRST (so a generated/written file overrides), then the
// read-only bundle (Data/, then Res/). Sub-paths preserved.
std::string resolve(const std::string& guest) {
    std::string rel = home_rel(guest);
    std::string base = rel;
    if (auto p = base.find_last_of("/\\"); p != std::string::npos) {
        base = base.substr(p + 1);
    }
    if (auto r = ci_resolve(g_home, rel); !r.empty()) {
        return r;
    }
    for (const std::string& cand : {"Data/" + rel, "Res/" + rel, rel, "Data/" + base, "Res/" + base}) {
        if (auto r = ci_resolve(g_pkg_root, cand); !r.empty()) {
            return r;
        }
    }
    return "";
}

// Osp::Base::String: +0 vptr, +4 __capacity, +8 __length, +12 __pValue (mchar* UTF-16)
std::string read_string(Cpu& c, uint32_t obj) {
    uint32_t pv = c.read_u32(obj + 12);
    int len = (int)c.read_u32(obj + 8);
    std::string s;
    for (int i = 0; i < len && i < 4096; i++) {
        uint16_t ch;
        c.read_mem(pv + i * 2, &ch, 2);
        s.push_back((char)ch);
    }
    return s;
}

// ---- ByteBuffer ----
void bb_ctor(Cpu& c) {
    uint32_t o = c.arg32(0);
    if (o) {
        c.write_u32(o, c.noop_vtable());
    }
    g_bufs[o] = Buf{};
    c.set_ret32(o);
}
void bb_construct(Cpu& c) {
    uint32_t o = c.arg32(0);
    int cap = c.arg32(1);
    uint32_t data = c.alloc(cap ? cap : 1, 4);
    std::vector<uint8_t> z(cap, 0);
    if (cap) {
        c.write_mem(data, z.data(), cap);
    }
    g_bufs[o] = {data, cap, 0, cap};
    c.set_ret32(0);
}
// ByteBuffer::Construct(const ByteBuffer&) -- bada shares the source's storage,
// so copy the descriptor rather than the bytes.
void bb_construct_copy(Cpu& c) {
    auto src = g_bufs.find(c.arg32(1));
    g_bufs[c.arg32(0)] = src == g_bufs.end() ? Buf{} : src->second;
    c.set_ret32(0);
}
void bb_dtor(Cpu& c) { g_bufs.erase(c.arg32(0)); c.set_ret32(0); }
void bb_getpointer(Cpu& c) { c.set_ret32(g_bufs.count(c.arg32(0)) ? g_bufs[c.arg32(0)].data : 0); }
void bb_getbyte(Cpu& c) {
    auto it = g_bufs.find(c.arg32(0));
    int idx = c.arg32(1);
    uint8_t v = 0;
    if (it != g_bufs.end() && idx < it->second.cap) {
        c.read_mem(it->second.data + idx, &v, 1);
    }
    c.write_mem(c.arg32(2), &v, 1);
    c.set_ret32(0);
}
void bb_setbyte(Cpu& c) {
    auto it = g_bufs.find(c.arg32(0));
    uint8_t v = c.arg32(1);
    if (it != g_bufs.end() && it->second.pos < it->second.cap) {
        c.write_mem(it->second.data + it->second.pos++, &v, 1);
    }
    c.set_ret32(0);
}

// BufferBase, the ByteBuffer half the app uses to read bytes back out.
void bb_getlimit(Cpu& c) {
    auto it = g_bufs.find(c.arg32(0));
    c.set_ret32(it == g_bufs.end() ? 0 : (uint32_t)it->second.limit);
}
void bb_setposition(Cpu& c) {
    auto it = g_bufs.find(c.arg32(0));
    if (it != g_bufs.end()) {
        it->second.pos = (int)c.arg32(1);
    }
    c.set_ret32(0);
}
// Flip(PositionTo): limit = position, position = 0 -- switches a buffer from
// filling to draining.
void bb_flip(Cpu& c) {
    auto it = g_bufs.find(c.arg32(0));
    if (it != g_bufs.end()) {
        it->second.limit = it->second.pos;
        it->second.pos = 0;
    }
    c.set_ret32(0);
}
void bb_clear(Cpu& c) {
    auto found = g_bufs.find(c.arg32(0));
    if (found != g_bufs.end()) {
        found->second.pos = 0;
        found->second.limit = found->second.cap;
    }
    c.set_ret32(0);
}
void bb_rewind(Cpu& c) {
    auto found = g_bufs.find(c.arg32(0));
    if (found != g_bufs.end()) found->second.pos = 0;
    c.set_ret32(0);
}
void bb_setarray(Cpu& c) {
    auto found = g_bufs.find(c.arg32(0));
    const uint32_t source = c.arg32(1);
    const int index = static_cast<int>(c.arg32(2)), count = static_cast<int>(c.arg32(3));
    if (found == g_bufs.end() || index < 0 || count < 0 ||
        index > found->second.limit || count > found->second.limit - index || (count && !source)) {
        c.set_ret32(0x80000000);
        return;
    }
    std::vector<uint8_t> bytes(count);
    if (count) {
        c.read_mem(source, bytes.data(), count);
        c.write_mem(found->second.data + index, bytes.data(), count);
    }
    c.set_ret32(0);
}
// GetArray(byte* dst, int index, int count)
void bb_getarray(Cpu& c) {
    auto it = g_bufs.find(c.arg32(0));
    uint32_t dst = c.arg32(1);
    int index = (int)c.arg32(2), count = (int)c.arg32(3);
    if (it == g_bufs.end() || index < 0 || count < 0 || index + count > it->second.cap) {
        c.set_ret32(0x80000000);
        return;
    }
    std::vector<uint8_t> b((size_t)count);
    if (count) {
        c.read_mem(it->second.data + index, b.data(), (uint32_t)count);
        c.write_mem(dst, b.data(), (uint32_t)count);
    }
    c.set_ret32(0);
}

// ---- Osp::Text::AsciiEncoding ----
// GetBytesN(const String&) returns a *new* ByteBuffer holding the string as
// ASCII. PvZ reads the name the player typed back out this way, so a stub
// handed it an empty name and it just asked again.
void ascii_ctor(Cpu& c) {
    uint32_t o = c.arg32(0);
    if (o) {
        c.write_u32(o, c.noop_vtable());
    }
    c.set_ret32(o);
}
void ascii_getbytesn(Cpu& c) {
    std::string s = read_string(c, c.arg32(1));
    uint32_t obj = c.make_stub_object(32);
    uint32_t data = c.alloc(s.size() + 1, 4);
    if (!s.empty()) {
        c.write_mem(data, s.data(), (uint32_t)s.size());
    }
    uint8_t z = 0;
    c.write_mem(data + s.size(), &z, 1);
    g_bufs[obj] = {data, (int)s.size(), 0, (int)s.size()};
    c.set_ret32(obj);
}

// ---- File ----
void file_ctor(Cpu& c) {
    uint32_t o = c.arg32(0);
    if (o) {
        c.write_u32(o, c.noop_vtable());
    }
    c.set_ret32(o);
}
void file_construct(Cpu& c) {
    uint32_t o = c.arg32(0);
    std::string path = read_string(c, c.arg32(1));
    std::string host = resolve(path);
    if (host.empty()) {
        printf("[io] File::Construct(\"%s\") -> NOT FOUND\n", path.c_str());
        c.set_ret32(0x80000000);
        return;
    }
    FILE* f = fopen(host.c_str(), "rb");
    if (!f) {
        c.set_ret32(0x80000000);
        return;
    }
    File fl;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    fl.data.resize(n);
    if (n) {
        size_t rd = fread(fl.data.data(), 1, n, f);
        (void)rd;
    }
    fclose(f);
    g_files[o] = std::move(fl);
    c.set_ret32(0);
}
void file_read(Cpu& c) {
    auto fi = g_files.find(c.arg32(0));
    auto bi = g_bufs.find(c.arg32(1));
    if (fi == g_files.end() || bi == g_bufs.end()) {
        c.set_ret32(0x80000000);
        return;
    }
    File& f = fi->second;
    Buf& b = bi->second;
    size_t n = std::min((size_t)b.cap, f.data.size() - f.pos);
    if (n) {
        c.write_mem(b.data, &f.data[f.pos], n);
    }
    f.pos += n;
    b.limit = n;
    c.set_ret32(0); // returns count? engine treats E_SUCCESS=0; report via buffer limit
}
// File::Read(void* buffer, int length) -> bytes read into the guest buffer.
void file_read_raw(Cpu& c) {
    auto fi = g_files.find(c.arg32(0));
    uint32_t buf = c.arg32(1);
    int len = (int)c.arg32(2);
    if (fi == g_files.end() || len <= 0) {
        c.set_ret32(0);
        return;
    }
    File& f = fi->second;
    size_t n = std::min((size_t)len, f.data.size() - f.pos);
    if (n) {
        c.write_mem(buf, &f.data[f.pos], (uint32_t)n);
    }
    f.pos += n;
    c.set_ret32((uint32_t)n); // count of bytes read
}
// File::Seek(FileSeekPosition pos, long offset). pos: BEGIN=0, CUR=1, END=2.
void file_seek(Cpu& c) {
    auto fi = g_files.find(c.arg32(0));
    if (fi == g_files.end()) {
        c.set_ret32(0x80000000);
        return;
    }
    File& f = fi->second;
    int whence = (int)c.arg32(1);
    long off = (int32_t)c.arg32(2);
    long base = whence == 1 ? (long)f.pos : whence == 2 ? (long)f.data.size() : 0;
    long np = base + off;
    if (np < 0) {
        np = 0;
    }
    if (np > (long)f.data.size()) {
        np = (long)f.data.size();
    }
    f.pos = (size_t)np;
    c.set_ret32(0);
}
// File::Tell() const -> current byte offset.
void file_tell(Cpu& c) {
    auto fi = g_files.find(c.arg32(0));
    c.set_ret32(fi == g_files.end() ? 0 : (uint32_t)fi->second.pos);
}
void file_dtor(Cpu& c) {
    g_files.erase(c.arg32(0));
    c.set_ret32(0);
}
void file_getattributes(Cpu& c) {
    std::string path = read_string(c, c.arg32(0));
    uint32_t attr = c.arg32(1);
    std::string host = resolve(path);
    std::error_code ec;
    if (host.empty()) {
        c.set_ret32(0x80000000);
        return;
    }
    auto sz = fs::file_size(host, ec);
    if (ec) {
        c.set_ret32(0x80000000);
        return;
    }
    g_attr_size[attr] = sz;
    c.set_ret32(0);
}
void fileattr_ctor(Cpu& c) {
    uint32_t o = c.arg32(0);
    if (o) {
        c.write_u32(o, c.noop_vtable());
    }
    c.set_ret32(o);
}
void fileattr_getsize(Cpu& c) { c.set_ret64(g_attr_size.count(c.arg32(0)) ? g_attr_size[c.arg32(0)] : 0); }

// ---- C stdio (FreeType and the engine load files via fopen/fread) ----
std::map<uint32_t, FILE*> g_cfiles;
uint32_t g_cfile_next = 0x9000;
void c_fopen(Cpu& c) {
    std::string path = c.read_cstr(c.arg32(0)), mode = c.read_cstr(c.arg32(1));
    bool writing = mode.find('w') != std::string::npos || mode.find('a') != std::string::npos;
    std::string host = writing ? write_path(path) : resolve(path);
    if (host.empty()) {
        // Name the caller: which loader gave up on a file tells you more than
        // the missing name does.
        uint32_t off = 0;
        std::string who = c.symbolize(c.reg(UC_ARM_REG_LR) & ~1u, &off);
        printf("[io] fopen(\"%s\") NOT FOUND  <- %s+0x%x\n", path.c_str(),
               who.empty() ? "?" : hle::demangle(who).c_str(), off);
        c.set_ret32(0);
        return;
    }
    FILE* f = fopen(host.c_str(), mode.find('b') != std::string::npos ? mode.c_str() : (mode + "b").c_str());
    if (!f) {
        c.set_ret32(0);
        return;
    }
    uint32_t h = g_cfile_next++;
    g_cfiles[h] = f;
    c.set_ret32(h);
}
FILE* cf(uint32_t h) {
    auto it = g_cfiles.find(h);
    return it == g_cfiles.end() ? nullptr : it->second;
}
void c_fread(Cpu& c) {
    uint32_t ptr = c.arg32(0);
    size_t sz = c.arg32(1), nm = c.arg32(2);
    FILE* f = cf(c.arg32(3));
    if (!f || !sz || !nm) {
        c.set_ret32(0);
        return;
    }
    size_t total = sz * nm;
    if (total > 64u * 1024 * 1024) {
        printf("[io] fread rejected huge size %zu\n", total);
        c.set_ret32(0);
        return;
    }
    std::vector<uint8_t> b(total);
    size_t got = fread(b.data(), sz, nm, f);
    if (got) {
        c.write_mem(ptr, b.data(), got * sz);
    }
    c.set_ret32(got);
}
void c_fwrite(Cpu& c) {
    uint32_t ptr = c.arg32(0);
    size_t sz = c.arg32(1), nm = c.arg32(2);
    FILE* f = cf(c.arg32(3));
    if (!f || !sz) {
        c.set_ret32(0);
        return;
    }
    std::vector<uint8_t> b(sz * nm);
    c.read_mem(ptr, b.data(), b.size());
    c.set_ret32(fwrite(b.data(), sz, nm, f));
}
void c_fseek(Cpu& c) {
    FILE* f = cf(c.arg32(0));
    c.set_ret32(f ? fseek(f, (int32_t)c.arg32(1), c.arg32(2)) : -1);
}
void c_ftell(Cpu& c) {
    FILE* f = cf(c.arg32(0));
    c.set_ret32(f ? ftell(f) : -1);
}
void c_rewind(Cpu& c) {
    FILE* f = cf(c.arg32(0));
    if (f) {
        rewind(f);
    }
    c.set_ret32(0);
}
// PvZ's XML parser reads the file one character at a time through fgetc/ungetc,
// so stubbing these two made it see EOF at once and parse nothing.
void c_fgetc(Cpu& c) {
    FILE* f = cf(c.arg32(0));
    c.set_ret32(f ? (uint32_t)fgetc(f) : (uint32_t)EOF);
}
void c_ungetc(Cpu& c) {
    FILE* f = cf(c.arg32(1));
    c.set_ret32(f ? (uint32_t)ungetc((int)c.arg32(0), f) : (uint32_t)EOF);
}
void c_fputc(Cpu& c) {
    FILE* f = cf(c.arg32(1));
    c.set_ret32(f ? (uint32_t)fputc((int)c.arg32(0), f) : (uint32_t)EOF);
}
void c_fputs(Cpu& c) {
    FILE* f = cf(c.arg32(1));
    std::string s = c.read_cstr(c.arg32(0));
    c.set_ret32(f ? (uint32_t)fputs(s.c_str(), f) : (uint32_t)EOF);
}
void c_feof(Cpu& c) {
    FILE* f = cf(c.arg32(0));
    c.set_ret32(f ? (uint32_t)feof(f) : 1);
}
void c_ferror(Cpu& c) {
    FILE* f = cf(c.arg32(0));
    c.set_ret32(f ? (uint32_t)ferror(f) : 1);
}
// fprintf(FILE*, fmt, ...). An unknown handle is the guest's stdout/stderr --
// echo it, so the app's own logging shows up.
void c_fprintf(Cpu& c) {
    std::string out = hle::format_args(c, c.read_cstr(c.arg32(1)), 2);
    if (FILE* f = cf(c.arg32(0))) {
        fwrite(out.data(), 1, out.size(), f);
    } else {
        printf("[app] %s", out.c_str());
    }
    c.set_ret32((uint32_t)out.size());
}
void c_fflush(Cpu& c) {
    FILE* f = cf(c.arg32(0));
    c.set_ret32(f ? (uint32_t)fflush(f) : 0);
}
// ---- POSIX file descriptors ----
// PvZ loads its PNGs through open/read, not fopen. Left unregistered these
// defaulted to returning 0 -- access() said "exists", open() handed back fd 0
// and read() gave no bytes, so every PNG decoded to nothing, silently.
std::map<int, FILE*> g_fds;
int g_fd_next = 3;
FILE* fdf(int fd) {
    auto it = g_fds.find(fd);
    return it == g_fds.end() ? nullptr : it->second;
}
void c_open(Cpu& c) {
    std::string path = c.read_cstr(c.arg32(0));
    uint32_t flags = c.arg32(1);
    bool writing = (flags & 3) != 0 || (flags & 0100 /*O_CREAT*/) != 0;
    std::string host = writing ? write_path(path) : resolve(path);
    FILE* f = host.empty() ? nullptr : fopen(host.c_str(), writing ? "wb" : "rb");
    if (!f) {
        printf("[io] open(\"%s\") NOT FOUND\n", path.c_str());
        c.set_ret32((uint32_t)-1);
        return;
    }
    int fd = g_fd_next++;
    g_fds[fd] = f;
    c.set_ret32((uint32_t)fd);
}
void c_read(Cpu& c) {
    FILE* f = fdf((int)c.arg32(0));
    uint32_t ptr = c.arg32(1), n = c.arg32(2);
    if (!f || !n) {
        c.set_ret32(f ? 0 : (uint32_t)-1);
        return;
    }
    std::vector<uint8_t> b(n);
    size_t got = fread(b.data(), 1, n, f);
    if (got) {
        c.write_mem(ptr, b.data(), (uint32_t)got);
    }
    c.set_ret32((uint32_t)got);
}
void c_write(Cpu& c) {
    FILE* f = fdf((int)c.arg32(0));
    uint32_t ptr = c.arg32(1), n = c.arg32(2);
    if (!f) {
        c.set_ret32(n); // stdout/stderr: pretend it went out
        return;
    }
    std::vector<uint8_t> b(n);
    c.read_mem(ptr, b.data(), n);
    c.set_ret32((uint32_t)fwrite(b.data(), 1, n, f));
}
void c_lseek(Cpu& c) {
    FILE* f = fdf((int)c.arg32(0));
    if (!f) {
        c.set_ret32((uint32_t)-1);
        return;
    }
    fseek(f, (int32_t)c.arg32(1), c.arg32(2));
    c.set_ret32((uint32_t)ftell(f));
}
void c_close(Cpu& c) {
    int fd = (int)c.arg32(0);
    if (FILE* f = fdf(fd)) {
        fclose(f);
        g_fds.erase(fd);
    }
    c.set_ret32(0);
}
void c_access(Cpu& c) {
    std::string path = c.read_cstr(c.arg32(0));
    c.set_ret32(resolve(path).empty() ? (uint32_t)-1 : 0);
}
void c_getcwd(Cpu& c) {
    uint32_t buf = c.arg32(0);
    if (buf && c.arg32(1) >= 2) {
        c.write_mem(buf, "/", 2);
    }
    c.set_ret32(buf);
}
void c_remove(Cpu& c) {
    std::string host = resolve(c.read_cstr(c.arg32(0)));
    std::error_code ec;
    c.set_ret32(host.empty() || !fs::remove(host, ec) ? (uint32_t)-1 : 0);
}
void c_rename(Cpu& c) {
    std::string from = resolve(c.read_cstr(c.arg32(0)));
    std::string to = write_path(c.read_cstr(c.arg32(1)));
    std::error_code ec;
    if (from.empty()) {
        c.set_ret32((uint32_t)-1);
        return;
    }
    fs::rename(from, to, ec);
    c.set_ret32(ec ? (uint32_t)-1 : 0);
}

void c_fclose(Cpu& c) {
    uint32_t h = c.arg32(0);
    FILE* f = cf(h);
    if (f) {
        fclose(f);
        g_cfiles.erase(h);
    }
    c.set_ret32(0);
}

} // namespace

namespace hle {
void set_package_root(const std::string& root) {
    g_pkg_root = root;
    g_home = root + "/Home";
    mkdir_p(g_home);
    printf("[io] package root: %s (home: %s)\n", root.c_str(), g_home.c_str());
}

bool byte_buffer_view(uint32_t object, uint32_t& data, uint32_t& length) {
    const auto found = g_bufs.find(object);
    if (found == g_bufs.end() || found->second.limit < 0 || found->second.limit > found->second.cap) return false;
    data = found->second.data;
    length = static_cast<uint32_t>(found->second.limit);
    return true;
}

void register_io() {
    register_handler("_ZN3Osp4Base10ByteBufferC1Ev", bb_ctor);
    register_handler("_ZN3Osp4Base10ByteBufferC2Ev", bb_ctor);
    register_handler("_ZN3Osp4Base10ByteBuffer9ConstructEi", bb_construct);
    register_handler("_ZN3Osp4Base10ByteBuffer9ConstructERKS1_", bb_construct_copy);
    register_handler("_ZNK3Osp4Base10BufferBase8GetLimitEv", bb_getlimit);
    register_handler("_ZN3Osp4Base10BufferBase11SetPositionEi", bb_setposition);
    register_handler("_ZN3Osp4Base10BufferBase4FlipENS0_10PositionToE", bb_flip);
    register_handler("_ZN3Osp4Base10BufferBase5ClearEv", bb_clear);
    register_handler("_ZN3Osp4Base10BufferBase6RewindEv", bb_rewind);
    register_handler("_ZN3Osp4Base10ByteBuffer8SetArrayEPKhii", bb_setarray);
    register_handler("_ZN3Osp4Base10ByteBuffer8GetArrayEPhii", bb_getarray);
    register_handler("_ZN3Osp4Text13AsciiEncodingC1Ev", ascii_ctor);
    register_handler("_ZN3Osp4Text13AsciiEncodingC2Ev", ascii_ctor);
    register_handler("_ZN3Osp4Text13AsciiEncodingD1Ev", bb_dtor);
    register_handler("_ZN3Osp4Text13AsciiEncodingD2Ev", bb_dtor);
    register_handler("_ZNK3Osp4Text13AsciiEncoding9GetBytesNERKNS_4Base6StringE", ascii_getbytesn);
    register_handler("_ZN3Osp4Base10ByteBufferD1Ev", bb_dtor);
    register_handler("_ZN3Osp4Base10ByteBufferD2Ev", bb_dtor);
    register_handler("_ZNK3Osp4Base10ByteBuffer10GetPointerEv", bb_getpointer);
    register_handler("_ZNK3Osp4Base10ByteBuffer7GetByteEiRh", bb_getbyte);
    register_handler("_ZN3Osp4Base10ByteBuffer7SetByteEh", bb_setbyte);

    register_handler("_ZN3Osp2Io4FileC1Ev", file_ctor);
    register_handler("_ZN3Osp2Io4FileD1Ev", file_dtor);
    register_handler("_ZN3Osp2Io4File9ConstructERKNS_4Base6StringES5_b", file_construct);
    register_handler("_ZN3Osp2Io4File4ReadERNS_4Base10ByteBufferE", file_read);
    register_handler("_ZN3Osp2Io4File4ReadEPvi", file_read_raw);              // Read(void*, int)
    register_handler("_ZN3Osp2Io4File4SeekENS0_16FileSeekPositionEl", file_seek);
    register_handler("_ZNK3Osp2Io4File4TellEv", file_tell);
    register_handler("_ZN3Osp2Io4File13GetAttributesERKNS_4Base6StringERNS0_14FileAttributesE", file_getattributes);
    register_handler("_ZN3Osp2Io14FileAttributesC1Ev", fileattr_ctor);
    register_handler("_ZN3Osp2Io14FileAttributesD1Ev", file_dtor);
    register_handler("_ZNK3Osp2Io14FileAttributes11GetFileSizeEv", fileattr_getsize);

    register_handler("fopen", c_fopen);
    register_handler("fread", c_fread);
    register_handler("fwrite", c_fwrite);
    register_handler("fseek", c_fseek);
    register_handler("ftell", c_ftell);
    register_handler("rewind", c_rewind);
    register_handler("fgetc", c_fgetc);
    register_handler("getc", c_fgetc);
    register_handler("ungetc", c_ungetc);
    register_handler("fputc", c_fputc);
    register_handler("fputs", c_fputs);
    register_handler("feof", c_feof);
    register_handler("ferror", c_ferror);
    register_handler("fflush", c_fflush);
    register_handler("fprintf", c_fprintf);
    register_handler("fclose", c_fclose);

    register_handler("open", c_open);
    register_handler("read", c_read);
    register_handler("write", c_write);
    register_handler("lseek", c_lseek);
    register_handler("close", c_close);
    register_handler("access", c_access);
    register_handler("getcwd", c_getcwd);
    register_handler("remove", c_remove);
    register_handler("rename", c_rename);
}
} // namespace hle
