#include "cpu.h"
#include "dispatch.h"
#include "gl_api.h"
#include "display.h"
#include "PVRTDecompress.h"
#include <SDL2/SDL.h>
#ifdef BADA_GLES_BACKEND
#include <GLES/gl.h>
#include <GLES2/gl2.h>
#else
// SDL_opengl.h pulls in <windows.h> in the right order on MSVC (so GL/gl.h's
// WINGDIAPI/APIENTRY are defined) and bundles SDL_opengl_glext.h, which supplies
// the GL 2.0+/glext typedefs and tokens we resolve via SDL_GL_GetProcAddress.
#include <SDL2/SDL_opengl.h>
#endif
#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <cstdio>
#include <cstring>
#include <map>
#include <vector>

// GL ES 2.0 -> host GL (desktop compatibility or native ES from SDL). The binary's GL
// symbols are suffixed `_2` (e.g. glClear_2); we strip the suffix, resolve the
// host fn via SDL_GL_GetProcAddress, and marshal args from guest regs.
//
// Two ES->desktop gaps handled here:
//  * Mali binary shaders -> we force the GLSL-source path (GL_SHADER_COMPILER=1,
//    NUM_SHADER_BINARY_FORMATS=0) and translate glShaderSource from guest memory.
//  * Client-side vertex arrays (guest pointers) -> recorded at
//    glVertexAttribPointer and copied guest->host at glDrawElements/glDrawArrays.

namespace {

SDL_Window* g_window = nullptr;
int g_surface_width = 480, g_surface_height = 800;
GLuint g_framebuffer = 0;
GLint g_viewport[4] = {0, 0, 480, 800};
GLint g_scissor[4] = {};
bool g_scissor_set = false;
int g_logical_landscape = -1;

void rotate_matrix(GLfloat* matrix, int turns) {
    for (int turn = 0; turn < (turns & 3); ++turn) {
        for (int column = 0; column < 4; ++column) {
            const GLfloat x = matrix[column * 4], y = matrix[column * 4 + 1];
            matrix[column * 4] = -y;
            matrix[column * 4 + 1] = x;
        }
    }
}

// Correct only default-framebuffer draws. Restoring the projection immediately
// preserves guest matrix queries/stacks and offscreen framebuffer rendering.
struct PresentationProjection {
    bool active = !g_framebuffer && display::rotation != 0;
    GLint mode = GL_MODELVIEW;
    GLfloat original[16]{};
    PresentationProjection() {
        if (!active) return;
        GLfloat rotated[16];
        glGetIntegerv(GL_MATRIX_MODE, &mode);
        glGetFloatv(GL_PROJECTION_MATRIX, original);
        std::copy(original, original + 16, rotated);
        rotate_matrix(rotated, display::rotation);
        glMatrixMode(GL_PROJECTION);
        glLoadMatrixf(rotated);
        glMatrixMode(mode);
    }
    ~PresentationProjection() {
        if (!active) return;
        glMatrixMode(GL_PROJECTION);
        glLoadMatrixf(original);
        glMatrixMode(mode);
    }
};

hle::SurfaceRect logical_surface() {
    const bool swap = display::rotation & 1;
    return {0, 0, swap ? g_surface_height : g_surface_width,
                  swap ? g_surface_width : g_surface_height};
}
#ifdef BADA_GLES_BACKEND
// ES 1.1 only accepts STATIC_DRAW and DYNAMIC_DRAW buffer usage.
constexpr GLenum client_buffer_usage = GL_DYNAMIC_DRAW;
#else
constexpr GLenum client_buffer_usage = GL_STREAM_DRAW;
#endif
GLuint g_canvas_texture = 0;
bool g_canvas_window_sized = false;
hle::CanvasUploadInfo g_canvas_upload_info;
void fit_window(GLint x, GLint y, GLsizei w, GLsizei h); // defined in the ES1 section
bool ff_query(GLenum pname, GLint& value); // guest client-array state, before VBO staging
int getiv_count(GLenum pname);

hle::SurfaceRect host_rect(const GLint* rectangle) {
    if (g_framebuffer || !g_window) return {rectangle[0], rectangle[1], rectangle[2], rectangle[3]};
    int width = 0, height = 0;
    SDL_GL_GetDrawableSize(g_window, &width, &height);
    const bool swap = display::rotation & 1;
    const int native_width = swap ? g_surface_height : g_surface_width;
    const int native_height = swap ? g_surface_width : g_surface_height;
    const auto area = display::content_rect(native_width, native_height, width, height);
    if (!area.width || !area.height) return {rectangle[0], rectangle[1], rectangle[2], rectangle[3]};
    hle::SurfaceRect rotated{rectangle[0], rectangle[1], rectangle[2], rectangle[3]};
    int source_height = g_surface_height, source_width = g_surface_width;
    for (int turn = 0; turn < display::rotation; ++turn) {
        rotated = {source_height - rotated.y - rotated.height, rotated.x, rotated.height, rotated.width};
        std::swap(source_width, source_height);
    }
    const double sx = static_cast<double>(area.width) / native_width;
    const double sy = static_cast<double>(area.height) / native_height;
    const int left = static_cast<int>(std::lround(rotated.x * sx));
    const int bottom = static_cast<int>(std::lround(rotated.y * sy));
    return {area.x + left, area.y + bottom,
        static_cast<int>(std::lround((static_cast<double>(rotated.x) + rotated.width) * sx)) - left,
        static_cast<int>(std::lround((static_cast<double>(rotated.y) + rotated.height) * sy)) - bottom};
}

void apply_viewport() {
    const auto rectangle = host_rect(g_viewport);
    glViewport(rectangle.x, rectangle.y, rectangle.width, rectangle.height);
}

void apply_scissor() {
    if (!g_scissor_set) return;
    const auto rectangle = host_rect(g_scissor);
    glScissor(rectangle.x, rectangle.y, rectangle.width, rectangle.height);
}

// ---- host GL entry points (resolved lazily) ----
template <class F>
F host(const char* name) {
    static std::map<std::string, void*, std::less<>> cache;
    auto it = cache.find(name);
    if (it != cache.end()) {
        return (F)it->second;
    }
    void* p = SDL_GL_GetProcAddress(name);
    if (!p) printf("[gl] host entry point unavailable: %s\n", name);
    cache[name] = p;
    return (F)p;
}

// ---- client vertex-attrib array state (for deferred translation) ----
struct AttribArray {
    bool enabled = false;
    uint32_t guest_ptr = 0;
    GLint size = 4;
    GLenum type = GL_FLOAT;
    GLboolean norm = GL_FALSE;
    GLsizei stride = 0;
    std::vector<uint8_t> staging;
};
std::map<GLuint, AttribArray> g_attribs;

uint32_t type_size(GLenum t) {
    switch (t) {
    case GL_FLOAT: return 4;
    case GL_UNSIGNED_BYTE:
    case GL_BYTE: return 1;
    case GL_UNSIGNED_SHORT:
    case GL_SHORT: return 2;
    default: return 4;
    }
}

// Host GL objects for client-array translation. WSLg/D3D12 gives a core-profile
// context, where client-side arrays are illegal -- so we upload guest arrays into
// real VBOs bound under a VAO and draw with buffer offsets.
GLuint g_vao = 0, g_ebo = 0;
std::map<GLuint, GLuint> g_vbo; // per attrib-location VBO
std::vector<uint8_t> g_indices;

typedef void (*genfn)(GLsizei, GLuint*);
typedef void (*bindfn)(GLenum, GLuint);
typedef void (*datafn)(GLenum, GLsizeiptr, const void*, GLenum);
void ensure_vao() {
#ifndef BADA_GLES_BACKEND
    if (!g_vao) {
        host<void (*)(GLsizei, GLuint*)>("glGenVertexArrays")(1, &g_vao);
    }
    host<void (*)(GLuint)>("glBindVertexArray")(g_vao);
#endif
}

// ============================ GL handlers ============================
void gl_clearcolor(Cpu& c) { glClearColor(c.argf32(0), c.argf32(1), c.argf32(2), c.argf32(3)); }
void gl_clear(Cpu& c) { glClear(c.arg32(0)); }
void gl_viewport(Cpu& c) {
    GLint x = c.arg32(0), y = c.arg32(1);
    GLsizei w = c.arg32(2), h = c.arg32(3);
    if (!g_framebuffer) fit_window(x, y, w, h);
    g_viewport[0] = x; g_viewport[1] = y; g_viewport[2] = w; g_viewport[3] = h;
    apply_viewport();
}
void gl_scissor(Cpu& c) {
    for (int i = 0; i < 4; ++i) g_scissor[i] = static_cast<GLint>(c.arg32(i));
    g_scissor_set = true;
    apply_scissor();
}
void gl_enable(Cpu& c) { glEnable(c.arg32(0)); }
void gl_disable(Cpu& c) { glDisable(c.arg32(0)); }
void gl_finish(Cpu&) { glFinish(); }

// Force the GLSL-source shader path (Mali binary shaders don't exist on desktop).
void gl_getbooleanv(Cpu& c) {
    GLenum pn = c.arg32(0);
    uint32_t out = c.arg32(1);
    const int count = getiv_count(pn);
    std::vector<GLboolean> values(std::max(16, count), GL_FALSE);
    GLint client_value = 0;
    if (pn == GL_SHADER_COMPILER) values[0] = GL_TRUE;
    else if (ff_query(pn, client_value)) values[0] = client_value != 0;
    else if (pn == GL_VIEWPORT || (pn == GL_SCISSOR_BOX && g_scissor_set)) {
        const GLint* rectangle = pn == GL_VIEWPORT ? g_viewport : g_scissor;
        for (int i = 0; i < 4; ++i) values[i] = rectangle[i] != 0;
    } else glGetBooleanv(pn, values.data());
    c.write_mem(out, values.data(), count);
}
// Number of values glGetIntegerv writes for a pname (most are 1; some are 2/4).
int getiv_count(GLenum pn) {
    switch (pn) {
    case 0x86A3: { // COMPRESSED_TEXTURE_FORMATS is an implementation-sized list.
        GLint count = 0;
        glGetIntegerv(0x86A2 /* NUM_COMPRESSED_TEXTURE_FORMATS */, &count);
        return std::max(0, count);
    }
    case 0x0BA6:
    case 0x0BA7:
    case 0x0BA8: return 16; // MODELVIEW, PROJECTION, TEXTURE matrices
    case 0x0B02: return 3; // CURRENT_NORMAL
    case 0x0B00:
    case 0x0B03:
    case 0x0B53:
    case 0x0B66:
    case 0x0C22:
    case 0x0C23:
    case 0x0BA2:
    case 0x0C10: return 4; // VIEWPORT, SCISSOR_BOX
    case 0x0D3A:
    case 0x0B12:
    case 0x846D:
    case 0x846E: return 2; // MAX_VIEWPORT_DIMS, ranges
    default: return 1;
    }
}
void getiv_into_guest(Cpu& c, GLenum pn, uint32_t out) {
    GLint client_value = 0;
    if (ff_query(pn, client_value)) {
        c.write_u32(out, static_cast<uint32_t>(client_value));
        return;
    }
    if (pn == GL_VIEWPORT || (pn == GL_SCISSOR_BOX && g_scissor_set)) {
        c.write_mem(out, pn == GL_VIEWPORT ? g_viewport : g_scissor, 4 * sizeof(GLint));
        return;
    }
    // glGetIntegerv writes a pname-dependent number of GLints into the caller's
    // buffer. getiv_count() is only how many we forward to the *guest*; the host
    // driver writes as many as the pname implies regardless. Most pnames write
    // <=4, but GL_COMPRESSED_TEXTURE_FORMATS writes GL_NUM_COMPRESSED_TEXTURE_
    // FORMATS entries (20+ on desktop GL) -- so size the scratch buffer to what
    // the driver will actually write, or it smashes the stack (MSVC /GS fastfail
    // on Windows; silent corruption elsewhere).
    auto host_getiv = host<void (*)(GLenum, GLint*)>("glGetIntegerv");
    size_t cap = 16;
    if (pn == 0x86A3 /*GL_COMPRESSED_TEXTURE_FORMATS*/) {
        GLint nf = 0;
        host_getiv(0x86A2 /*GL_NUM_COMPRESSED_TEXTURE_FORMATS*/, &nf);
        if (nf > (GLint)cap) {
            cap = (size_t)nf;
        }
    }
    std::vector<GLint> v(cap, 0);
    host_getiv(pn, v.data());
    int n = getiv_count(pn);
    for (int i = 0; i < n; i++) {
        c.write_u32(out + i * 4, (uint32_t)v[i]);
    }
}
void gl_getintegerv(Cpu& c) {
    GLenum pn = c.arg32(0);
    uint32_t out = c.arg32(1);
    if (pn == 0x8DF9 /*GL_NUM_SHADER_BINARY_FORMATS*/) {
        c.write_u32(out, 0);
        return;
    }
    getiv_into_guest(c, pn, out);
}

void gl_createshader(Cpu& c) { c.set_ret32(host<GLuint (*)(GLenum)>("glCreateShader")(c.arg32(0))); }
void gl_createprogram(Cpu& c) { c.set_ret32(host<GLuint (*)()>("glCreateProgram")()); }
void gl_attachshader(Cpu& c) { host<void (*)(GLuint, GLuint)>("glAttachShader")(c.arg32(0), c.arg32(1)); }
void gl_compileshader(Cpu& c) {
    GLuint sh = c.arg32(0);
    host<void (*)(GLuint)>("glCompileShader")(sh);
    GLint ok = 0;
    host<void (*)(GLuint, GLenum, GLint*)>("glGetShaderiv")(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512] = {0};
        host<void (*)(GLuint, GLsizei, GLsizei*, char*)>("glGetShaderInfoLog")(sh, 512, nullptr, log);
        printf("[gl] SHADER COMPILE FAILED: %s\n", log);
    }
}
void gl_linkprogram(Cpu& c) {
    GLuint p = c.arg32(0);
    host<void (*)(GLuint)>("glLinkProgram")(p);
    GLint ok = 0;
    host<void (*)(GLuint, GLenum, GLint*)>("glGetProgramiv")(p, GL_LINK_STATUS, &ok);
    printf("[gl] link program %u: %s\n", p, ok ? "OK" : "FAILED");
    if (!ok) {
        char log[512] = {0};
        host<void (*)(GLuint, GLsizei, GLsizei*, char*)>("glGetProgramInfoLog")(p, 512, nullptr, log);
        printf("[gl]   %s\n", log);
    }
}
void gl_useprogram(Cpu& c) { host<void (*)(GLuint)>("glUseProgram")(c.arg32(0)); }
void gl_uniform1i(Cpu& c) { host<void (*)(GLint, GLint)>("glUniform1i")(c.arg32(0), c.arg32(1)); }
void gl_deleteshader(Cpu& c) { host<void (*)(GLuint)>("glDeleteShader")(c.arg32(0)); }
void gl_deleteprogram(Cpu& c) { host<void (*)(GLuint)>("glDeleteProgram")(c.arg32(0)); }
void gl_getstring(Cpu& c) {
    const auto* value = glGetString(c.arg32(0));
    if (!value) { c.set_ret32(0); return; }
    const size_t length = strlen((const char*)value) + 1;
    uint32_t out = c.alloc(length);
    c.write_mem(out, value, length);
    c.set_ret32(out);
}

// glShaderSource(shader, count, const char** strings, const int* lengths)
void gl_shadersource(Cpu& c) {
    GLuint sh = c.arg32(0);
    GLsizei count = c.arg32(1);
    uint32_t strs = c.arg32(2);
    std::vector<std::string> src;
    std::vector<const char*> ptrs;
    for (int i = 0; i < count; i++) {
        src.push_back(c.read_cstr(c.read_u32(strs + i * 4), 1 << 16));
    }
    for (auto& s : src) {
        ptrs.push_back(s.c_str());
    }
    host<void (*)(GLuint, GLsizei, const char**, const GLint*)>("glShaderSource")(sh, count, ptrs.data(), nullptr);
}
void gl_getshaderiv(Cpu& c) {
    GLint v = 0;
    host<void (*)(GLuint, GLenum, GLint*)>("glGetShaderiv")(c.arg32(0), c.arg32(1), &v);
    c.write_u32(c.arg32(2), v);
}
void gl_getprogramiv(Cpu& c) {
    GLint v = 0;
    host<void (*)(GLuint, GLenum, GLint*)>("glGetProgramiv")(c.arg32(0), c.arg32(1), &v);
    c.write_u32(c.arg32(2), v);
}
void gl_getattriblocation(Cpu& c) {
    std::string n = c.read_cstr(c.arg32(1));
    GLint l = host<GLint (*)(GLuint, const char*)>("glGetAttribLocation")(c.arg32(0), n.c_str());
    printf("[gl] attribLoc(\"%s\")=%d\n", n.c_str(), l);
    c.set_ret32(l);
}
void gl_getuniformlocation(Cpu& c) {
    std::string n = c.read_cstr(c.arg32(1));
    GLint l = host<GLint (*)(GLuint, const char*)>("glGetUniformLocation")(c.arg32(0), n.c_str());
    printf("[gl] uniformLoc(\"%s\")=%d\n", n.c_str(), l);
    c.set_ret32(l);
}
void gl_bindattriblocation(Cpu& c) {
    std::string n = c.read_cstr(c.arg32(2));
    host<void (*)(GLuint, GLuint, const char*)>("glBindAttribLocation")(c.arg32(0), c.arg32(1), n.c_str());
}

// Defer enable to draw time (needs the VAO bound); just track state here.
void gl_enablevattrib(Cpu& c) { g_attribs[c.arg32(0)].enabled = true; }
void gl_disablevattrib(Cpu& c) { g_attribs[c.arg32(0)].enabled = false; }

// glVertexAttribPointer(idx,size,type,norm,stride,ptr): record guest ptr; bind at draw.
void gl_vattribpointer(Cpu& c) {
    auto& a = g_attribs[c.arg32(0)];
    a.size = c.arg32(1);
    a.type = c.arg32(2);
    a.norm = c.arg32(3);
    a.stride = c.arg32(4);
    a.guest_ptr = c.arg32(5);
}
// glUniformMatrix4fv(loc,count,transpose,value): copy 16*count floats from guest.
void gl_uniformmatrix4fv(Cpu& c) {
    GLint loc = c.arg32(0);
    GLsizei n = c.arg32(1);
    GLboolean t = c.arg32(2);
    std::vector<float> m(16 * n);
    c.read_mem(c.arg32(3), m.data(), 16 * 4 * n);
    host<void (*)(GLint, GLsizei, GLboolean, const GLfloat*)>("glUniformMatrix4fv")(loc, n, t, m.data());
    static bool once = false;
    if (!once && n) {
        once = true;
        printf("[gl] MVP loc=%d: [%.2f %.2f %.2f %.2f / %.2f %.2f %.2f %.2f / %.2f %.2f %.2f %.2f / %.2f %.2f %.2f %.2f]\n",
               loc, m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9], m[10], m[11], m[12], m[13], m[14], m[15]);
    }
}

// Upload each enabled attrib's guest array into its VBO and wire it up.
void bind_client_arrays(Cpu& c, uint32_t vert_count) {
    for (auto& [idx, a] : g_attribs) {
        if (!a.enabled) {
            host<void (*)(GLuint)>("glDisableVertexAttribArray")(idx);
            continue;
        }
        if (!a.guest_ptr) {
            continue;
        }
        uint32_t stride = a.stride ? a.stride : a.size * type_size(a.type);
        const size_t bytes = vert_count ? static_cast<size_t>(stride) * (vert_count - 1) + a.size * type_size(a.type) : 0;
        auto& data = a.staging;
        if (data.size() < bytes) data.resize(bytes);
        c.read_mem(a.guest_ptr, data.data(), bytes);
        GLuint& vbo = g_vbo[idx];
        if (!vbo) {
            host<genfn>("glGenBuffers")(1, &vbo);
        }
        host<bindfn>("glBindBuffer")(GL_ARRAY_BUFFER, vbo);
        host<datafn>("glBufferData")(GL_ARRAY_BUFFER, bytes, bytes ? data.data() : nullptr, client_buffer_usage);
        host<void (*)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void*)>("glVertexAttribPointer")(idx, a.size, a.type, a.norm, a.stride, (const void*)0);
        host<void (*)(GLuint)>("glEnableVertexAttribArray")(idx);
    }
}
// glDrawElements(mode,count,type,indices): upload indices+verts to VBOs, draw.
void gl_drawelements(Cpu& c) {
    GLenum mode = c.arg32(0);
    GLsizei count = c.arg32(1);
    GLenum type = c.arg32(2);
    uint32_t iptr = c.arg32(3);
    uint32_t isz = type_size(type);
    const size_t bytes = static_cast<size_t>(count) * isz;
    auto& idx = g_indices;
    if (idx.size() < bytes) idx.resize(bytes);
    c.read_mem(iptr, idx.data(), bytes);
    uint32_t maxi = 0;
    for (int i = 0; i < count; i++) {
        uint32_t v = (isz == 1) ? idx[i] : (isz == 2) ? ((uint16_t*)idx.data())[i]
                                                      : ((uint32_t*)idx.data())[i];
        if (v > maxi) {
            maxi = v;
        }
    }
    ensure_vao();
    bind_client_arrays(c, maxi + 1);
    if (!g_ebo) {
        host<genfn>("glGenBuffers")(1, &g_ebo);
    }
    host<bindfn>("glBindBuffer")(GL_ELEMENT_ARRAY_BUFFER, g_ebo);
    host<datafn>("glBufferData")(GL_ELEMENT_ARRAY_BUFFER, bytes, bytes ? idx.data() : nullptr, client_buffer_usage);
    host<void (*)(GLenum, GLsizei, GLenum, const void*)>("glDrawElements")(mode, count, type, (const void*)0);
    static bool once = false;
    if (!once) {
        once = true;
        printf("[gl] first draw: mode=%u count=%d verts=%u err=0x%x\n", mode, count, maxi + 1, glGetError());
    }
}
void gl_drawarrays(Cpu& c) {
    GLenum mode = c.arg32(0);
    GLint first = c.arg32(1);
    GLsizei count = c.arg32(2);
    ensure_vao();
    bind_client_arrays(c, first + count);
    host<void (*)(GLenum, GLint, GLsizei)>("glDrawArrays")(mode, first, count);
}

// ============================ GL ES 1.x fixed-function ============================
// FruitNinja uses the fixed pipeline (glFoo_1). Desktop compat profile supports
// it natively, so forward 1:1; client arrays + texture data translate from guest.
struct FFArray {
    bool enabled = false;
    uint32_t ptr = 0; // client pointer (guest addr) OR byte offset into `vbo`
    GLint size = 4;
    GLenum type = GL_FLOAT;
    GLsizei stride = 0;
    GLuint vbo = 0; // guest's GL_ARRAY_BUFFER bound when the pointer was set (0 = client array)
};
FFArray g_ff_vertex, g_ff_color;
FFArray g_ff_normal{false, 0, 3};
std::map<GLenum, FFArray> g_ff_texcoords{{GL_TEXTURE0, {}}};
FFArray& ff_texcoord() {
    GLint unit = GL_TEXTURE0;
    glGetIntegerv(GL_CLIENT_ACTIVE_TEXTURE, &unit);
    return g_ff_texcoords[static_cast<GLenum>(unit)];
}
bool ff_query(GLenum pname, GLint& value) {
    // glGetPointerv/GetIntegerv must describe the guest arrays, not the host
    // VBOs used to stage them. Khronos ES 1.1 assigns texture-coordinate state
    // to the active client unit; touchHLE preserves the same distinction.
    const FFArray* array = nullptr;
    enum Field { enabled, size, type, stride, buffer } field = enabled;
    switch (pname) {
    case GL_VERTEX_ARRAY: array = &g_ff_vertex; break;
    case GL_VERTEX_ARRAY_SIZE: array = &g_ff_vertex; field = size; break;
    case GL_VERTEX_ARRAY_TYPE: array = &g_ff_vertex; field = type; break;
    case GL_VERTEX_ARRAY_STRIDE: array = &g_ff_vertex; field = stride; break;
    case GL_VERTEX_ARRAY_BUFFER_BINDING: array = &g_ff_vertex; field = buffer; break;
    case GL_COLOR_ARRAY: array = &g_ff_color; break;
    case GL_COLOR_ARRAY_SIZE: array = &g_ff_color; field = size; break;
    case GL_COLOR_ARRAY_TYPE: array = &g_ff_color; field = type; break;
    case GL_COLOR_ARRAY_STRIDE: array = &g_ff_color; field = stride; break;
    case GL_COLOR_ARRAY_BUFFER_BINDING: array = &g_ff_color; field = buffer; break;
    case GL_NORMAL_ARRAY: array = &g_ff_normal; break;
    case GL_NORMAL_ARRAY_TYPE: array = &g_ff_normal; field = type; break;
    case GL_NORMAL_ARRAY_STRIDE: array = &g_ff_normal; field = stride; break;
    case GL_NORMAL_ARRAY_BUFFER_BINDING: array = &g_ff_normal; field = buffer; break;
    case GL_TEXTURE_COORD_ARRAY: array = &ff_texcoord(); break;
    case GL_TEXTURE_COORD_ARRAY_SIZE: array = &ff_texcoord(); field = size; break;
    case GL_TEXTURE_COORD_ARRAY_TYPE: array = &ff_texcoord(); field = type; break;
    case GL_TEXTURE_COORD_ARRAY_STRIDE: array = &ff_texcoord(); field = stride; break;
    case GL_TEXTURE_COORD_ARRAY_BUFFER_BINDING: array = &ff_texcoord(); field = buffer; break;
    default: return false;
    }
    switch (field) {
    case enabled: value = array->enabled; break;
    case size: value = array->size; break;
    case type: value = array->type; break;
    case stride: value = array->stride; break;
    case buffer: value = array->vbo; break;
    }
    return true;
}
struct FFStream {
    GLuint buffer = 0;
    std::vector<uint8_t> staging;
};
std::map<size_t, FFStream> g_ff_streams;
std::vector<uint8_t> g_ff_indices;

// state forwards
void g1_clear(Cpu& c) { glClear(c.arg32(0)); }
void g1_clearcolor(Cpu& c) { glClearColor(c.argf32(0), c.argf32(1), c.argf32(2), c.argf32(3)); }
void g1_cleardepthf(Cpu& c) {
#ifdef BADA_GLES_BACKEND
    glClearDepthf(c.argf32(0));
#else
    glClearDepth(c.argf32(0));
#endif
}
void g1_enable(Cpu& c) { glEnable(c.arg32(0)); }
void g1_disable(Cpu& c) { glDisable(c.arg32(0)); }
void g1_depthfunc(Cpu& c) { glDepthFunc(c.arg32(0)); }
void g1_depthmask(Cpu& c) { glDepthMask(c.arg32(0)); }
void g1_cullface(Cpu& c) { glCullFace(c.arg32(0)); }
void g1_blendfunc(Cpu& c) { glBlendFunc(c.arg32(0), c.arg32(1)); }
// These forwards follow touchHLE's gles_guest.rs. Scalar float arguments use
// independent VFP registers in the hard ABI and follow integer args in soft.
void g1_colormask(Cpu& c) { glColorMask(c.arg32(0), c.arg32(1), c.arg32(2), c.arg32(3)); }
void g1_color4f(Cpu& c) { glColor4f(c.argf32(0), c.argf32(1), c.argf32(2), c.argf32(3)); }
void g1_frontface(Cpu& c) { glFrontFace(c.arg32(0)); }
void g1_hint(Cpu& c) { glHint(c.arg32(0), c.arg32(1)); }
void g1_linewidth(Cpu& c) { glLineWidth(c.argf32(0)); }
void g1_pointsize(Cpu& c) { glPointSize(c.argf32(0)); }
void g1_alphafunc(Cpu& c) { glAlphaFunc(c.arg32(0), c.argf32(c.hard_float ? 0 : 1)); }
void g1_fogf(Cpu& c) { glFogf(c.arg32(0), c.argf32(c.hard_float ? 0 : 1)); }
void g1_fogfv(Cpu& c) {
    GLfloat values[4] = {};
    c.read_mem(c.arg32(1), values, (c.arg32(0) == GL_FOG_COLOR ? 4 : 1) * sizeof(GLfloat));
    glFogfv(c.arg32(0), values);
}
void g1_lightmodelfv(Cpu& c) {
    GLfloat values[4] = {};
    c.read_mem(c.arg32(1), values, (c.arg32(0) == GL_LIGHT_MODEL_AMBIENT ? 4 : 1) * sizeof(GLfloat));
    glLightModelfv(c.arg32(0), values);
}
void g1_materialfv(Cpu& c) {
    GLfloat values[4] = {};
    c.read_mem(c.arg32(2), values, (c.arg32(1) == GL_SHININESS ? 1 : 4) * sizeof(GLfloat));
    glMaterialfv(c.arg32(0), c.arg32(1), values);
}
void g1_isenabled(Cpu& c) {
    GLint value = 0;
    c.set_ret32(ff_query(c.arg32(0), value) ? value != 0 : glIsEnabled(c.arg32(0)));
}
void g1_getfloatv(Cpu& c) {
    const GLenum pname = c.arg32(0);
    const int count = getiv_count(pname);
    std::vector<GLfloat> values(std::max(16, count), 0);
    GLint client_value = 0;
    if (ff_query(pname, client_value)) values[0] = static_cast<GLfloat>(client_value);
    else if (pname == GL_VIEWPORT || (pname == GL_SCISSOR_BOX && g_scissor_set)) {
        const GLint* rectangle = pname == GL_VIEWPORT ? g_viewport : g_scissor;
        for (int i = 0; i < 4; ++i) values[i] = static_cast<GLfloat>(rectangle[i]);
    } else glGetFloatv(pname, values.data());
    c.write_mem(c.arg32(1), values.data(), count * sizeof(GLfloat));
}
void g1_getpointerv(Cpu& c) {
    uint32_t pointer = 0;
    switch (c.arg32(0)) {
    case GL_VERTEX_ARRAY_POINTER: pointer = g_ff_vertex.ptr; break;
    case GL_COLOR_ARRAY_POINTER: pointer = g_ff_color.ptr; break;
    case GL_NORMAL_ARRAY_POINTER: pointer = g_ff_normal.ptr; break;
    case GL_TEXTURE_COORD_ARRAY_POINTER: pointer = ff_texcoord().ptr; break;
    default: {
        void* ignored = nullptr;
        glGetPointerv(c.arg32(0), &ignored); // preserve the host's invalid-enum error
        return;
    }
    }
    c.write_u32(c.arg32(1), pointer);
}
void g1_shademodel(Cpu& c) { glShadeModel(c.arg32(0)); }
// The first full-surface viewport reveals the guest framebuffer dimensions.
// Some ES1 SDKs rotate their landscape window inside a portrait framebuffer;
// their logical window orientation is resolved separately after initialization.
bool g_window_fit = false;
void fit_window(GLint x, GLint y, GLsizei w, GLsizei h) {
    if (g_window_fit || !g_window || x || y || w < 64 || h < 64) {
        return;
    }
    g_window_fit = true;
    hle::gl_set_surface_size(w, h, "GL viewport");
}
void g1_viewport(Cpu& c) {
    gl_viewport(c);
}
void g1_finish(Cpu&) { glFinish(); }
void g1_flush(Cpu&) { glFlush(); }
void g1_color4ub(Cpu& c) {
#ifdef BADA_GLES_BACKEND
    glColor4f((uint8_t)c.arg32(0) / 255.0f, (uint8_t)c.arg32(1) / 255.0f,
              (uint8_t)c.arg32(2) / 255.0f, (uint8_t)c.arg32(3) / 255.0f);
#else
    glColor4ub(c.arg32(0), c.arg32(1), c.arg32(2), c.arg32(3));
#endif
}
// matrix
void g1_matrixmode(Cpu& c) { glMatrixMode(c.arg32(0)); }
void g1_loadidentity(Cpu&) { glLoadIdentity(); }
void g1_pushmatrix(Cpu&) { glPushMatrix(); }
void g1_popmatrix(Cpu&) { glPopMatrix(); }
void g1_frustumf(Cpu& c) {
#ifdef BADA_GLES_BACKEND
    glFrustumf(c.argf32(0), c.argf32(1), c.argf32(2), c.argf32(3), c.argf32(4), c.argf32(5));
#else
    glFrustum(c.argf32(0), c.argf32(1), c.argf32(2), c.argf32(3), c.argf32(4), c.argf32(5));
#endif
}
void g1_orthof(Cpu& c) {
#ifdef BADA_GLES_BACKEND
    glOrthof(c.argf32(0), c.argf32(1), c.argf32(2), c.argf32(3), c.argf32(4), c.argf32(5));
#else
    glOrtho(c.argf32(0), c.argf32(1), c.argf32(2), c.argf32(3), c.argf32(4), c.argf32(5));
#endif
}
void g1_translatef(Cpu& c) { glTranslatef(c.argf32(0), c.argf32(1), c.argf32(2)); }
void g1_rotatef(Cpu& c) { glRotatef(c.argf32(0), c.argf32(1), c.argf32(2), c.argf32(3)); }
void g1_scalef(Cpu& c) { glScalef(c.argf32(0), c.argf32(1), c.argf32(2)); }
void g1_loadmatrixf(Cpu& c) {
    float m[16];
    c.read_mem(c.arg32(0), m, 64);
    glLoadMatrixf(m);
}
void g1_multmatrixf(Cpu& c) {
    float m[16];
    c.read_mem(c.arg32(0), m, 64);
    glMultMatrixf(m);
}
// textures
void g1_activetexture(Cpu& c) { host<void (*)(GLenum)>("glActiveTexture")(c.arg32(0)); }
void g1_clientactivetexture(Cpu& c) { host<void (*)(GLenum)>("glClientActiveTexture")(c.arg32(0)); }
void g1_bindtexture(Cpu& c) { glBindTexture(c.arg32(0), c.arg32(1)); }
void g1_texparameteri(Cpu& c) { glTexParameteri(c.arg32(0), c.arg32(1), c.arg32(2)); }
void g1_texenvi(Cpu& c) { glTexEnvi(c.arg32(0), c.arg32(1), c.arg32(2)); }
void g1_texenvf(Cpu& c) { glTexEnvf(c.arg32(0), c.arg32(1), c.argf32(c.hard_float ? 0 : 2)); }
void g1_gentextures(Cpu& c) {
    GLsizei n = c.arg32(0);
    std::vector<GLuint> ids(n);
    glGenTextures(n, ids.data());
    for (int i = 0; i < n; i++) {
        c.write_u32(c.arg32(1) + i * 4, ids[i]);
    }
}
void g1_deletetextures(Cpu& c) {
    GLsizei n = c.arg32(0);
    std::vector<GLuint> ids(n);
    for (int i = 0; i < n; i++) {
        ids[i] = c.read_u32(c.arg32(1) + i * 4);
    }
    glDeleteTextures(n, ids.data());
}
uint32_t pixel_bytes(GLenum fmt, GLenum type) {
    // Packed 16-bit types are a single 2-byte value PER PIXEL (not per channel).
    switch (type) {
    case GL_UNSIGNED_SHORT_5_6_5:
    case GL_UNSIGNED_SHORT_4_4_4_4:
    case GL_UNSIGNED_SHORT_5_5_5_1:
        return 2;
    default:
        break;
    }
    // Unpacked: one byte per channel (GL_UNSIGNED_BYTE).
    // touchHLE's gles_guest.rs image_size_estimate treats BGRA_EXT as four
    // bytes too. Nova uses it for its title, font and sprite textures.
    int ch = (fmt == GL_RGBA || fmt == 0x80E1 /*GL_BGRA_EXT*/) ? 4 : (fmt == GL_RGB) ? 3
                                : (fmt == GL_LUMINANCE_ALPHA) ? 2
                                                              : 1;
    return ch;
}
void g1_teximage2d(Cpu& c) {
    GLenum tgt = c.arg32(0);
    GLint lvl = c.arg32(1), ifmt = c.arg32(2);
    GLsizei w = c.arg32(3), h = c.arg32(4);
    GLint bd = c.arg32(5);
    GLenum fmt = c.arg32(6), type = c.arg32(7);
    uint32_t px = c.arg32(8);
    std::vector<uint8_t> data;
    const void* p = nullptr;
    if (px) {
        data.resize(w * h * pixel_bytes(fmt, type));
        c.read_mem(px, data.data(), data.size());
        p = data.data();
    }
#ifdef BADA_GLES_BACKEND
    // As in touchHLE's gles1_native.rs, EXT_texture_format_BGRA8888 requires
    // matching BGRA internal/external formats on native OpenGL ES. Desktop GL
    // accepts Nova's original RGBA internal format with BGRA source pixels.
    if (fmt == 0x80E1 /*GL_BGRA_EXT*/) ifmt = fmt;
#endif
    glTexImage2D(tgt, lvl, ifmt, w, h, bd, fmt, type, p);
}
void g1_texsubimage2d(Cpu& c) {
    GLenum tgt = c.arg32(0);
    GLint lvl = c.arg32(1), x = c.arg32(2), y = c.arg32(3);
    GLsizei w = c.arg32(4), h = c.arg32(5);
    GLenum fmt = c.arg32(6), type = c.arg32(7);
    uint32_t px = c.arg32(8);
    std::vector<uint8_t> data;
    const void* p = nullptr;
    if (px) {
        data.resize(w * h * pixel_bytes(fmt, type));
        c.read_mem(px, data.data(), data.size());
        p = data.data();
    }
    glTexSubImage2D(tgt, lvl, x, y, w, h, fmt, type, p);
}
void g1_compressedteximage2d(Cpu& c) {
    GLenum target = c.arg32(0), format = c.arg32(2);
    GLint level = c.arg32(1), border = c.arg32(5);
    GLsizei width = c.arg32(3), height = c.arg32(4), size = c.arg32(6);
    uint32_t source = c.arg32(7);
    std::vector<uint8_t> data(size > 0 && source ? static_cast<size_t>(size) : 0);
    if (!data.empty()) c.read_mem(source, data.data(), static_cast<uint32_t>(data.size()));

    // touchHLE's try_decode_pvrtc uses the PowerVR decoder and uploads RGBA
    // when the host cannot consume the original PVRTC texture. Decode on both
    // backends so Nova also works on Android GPUs without this extension.
    bool pvrtc = format >= 0x8C00 && format <= 0x8C03;
    bool two_bit = format == 0x8C01 || format == 0x8C03;
    bool valid_dimensions = width > 0 && height > 0 &&
        !(width & (width - 1)) && !(height & (height - 1));
    if (pvrtc && valid_dimensions && level >= 0 && !border && source) {
        const uint64_t required = static_cast<uint64_t>(std::max(width, two_bit ? 16 : 8)) *
            std::max(height, 8) / (two_bit ? 4 : 2);
        const uint64_t decoded = static_cast<uint64_t>(width) * height * 4;
        if (data.size() == required && decoded <= UINT32_MAX) {
            std::vector<uint8_t> pixels(static_cast<size_t>(decoded));
            pvr::PVRTDecompressPVRTC(data.data(), two_bit, width, height, pixels.data());
            if (format == 0x8C00 || format == 0x8C01) {
                // RGB formats ignore the encoded alpha, as ordinary GL_RGB does.
                for (size_t i = 3; i < pixels.size(); i += 4) pixels[i] = 255;
            }
            glTexImage2D(target, level, GL_RGBA, width, height, border,
                         GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
            return;
        }
    }
    host<void (*)(GLenum, GLint, GLenum, GLsizei, GLsizei, GLint, GLsizei, const void*)>(
        "glCompressedTexImage2D")(target, level, format, width, height, border, size,
                                 data.empty() ? nullptr : data.data());
}
void g1_pixelstorei(Cpu& c) { glPixelStorei(c.arg32(0), c.arg32(1)); }
void g1_geterror(Cpu& c) {
    GLenum e = glGetError();
    static int logged = 0;
    if (e && logged < 8) {
        logged++;
        printf("[gl] glGetError -> 0x%x\n", e);
    }
    c.set_ret32(e);
}
// GL_OES_framebuffer_object -> desktop ARB/core FBOs. The OES tokens share the
// EXT/core values, so the arguments pass straight through. Older drivers only
// export the EXT names, so fall back to those.
template <class F>
F fbo(const char* core, const char* ext) {
#ifdef BADA_GLES_BACKEND
    int major = 0;
    SDL_GL_GetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, &major);
    if (major == 1) {
        return host<F>((std::string(core) + "OES").c_str());
    }
#endif
    F f = host<F>(core);
    return f ? f : host<F>(ext);
}
void g1_bindframebuffer(Cpu& c) {
    fbo<bindfn>("glBindFramebuffer", "glBindFramebufferEXT")(c.arg32(0), c.arg32(1));
    g_framebuffer = c.arg32(1);
    apply_viewport();
    apply_scissor();
}
void g1_genframebuffers(Cpu& c) {
    GLsizei n = c.arg32(0);
    std::vector<GLuint> ids(n);
    fbo<genfn>("glGenFramebuffers", "glGenFramebuffersEXT")(n, ids.data());
    for (int i = 0; i < n; i++) {
        c.write_u32(c.arg32(1) + i * 4, ids[i]);
    }
}
void g1_deleteframebuffers(Cpu& c) {
    GLsizei n = c.arg32(0);
    std::vector<GLuint> ids(n);
    for (int i = 0; i < n; i++) {
        ids[i] = c.read_u32(c.arg32(1) + i * 4);
    }
    fbo<void (*)(GLsizei, const GLuint*)>("glDeleteFramebuffers", "glDeleteFramebuffersEXT")(n, ids.data());
    if (g_framebuffer && std::find(ids.begin(), ids.end(), g_framebuffer) != ids.end()) {
        g_framebuffer = 0; // GL resets the binding when the bound FBO is deleted.
        apply_viewport();
        apply_scissor();
    }
}
void g1_framebuffertexture2d(Cpu& c) {
    fbo<void (*)(GLenum, GLenum, GLenum, GLuint, GLint)>("glFramebufferTexture2D",
                                                         "glFramebufferTexture2DEXT")(
        c.arg32(0), c.arg32(1), c.arg32(2), c.arg32(3), c.arg32(4));
}
void g1_checkframebufferstatus(Cpu& c) {
    c.set_ret32(fbo<GLenum (*)(GLenum)>("glCheckFramebufferStatus",
                                        "glCheckFramebufferStatusEXT")(c.arg32(0)));
}
// buffers
void g1_bindbuffer(Cpu& c) { host<bindfn>("glBindBuffer")(c.arg32(0), c.arg32(1)); }
void g1_genbuffers(Cpu& c) {
    GLsizei n = c.arg32(0);
    std::vector<GLuint> ids(n);
    host<genfn>("glGenBuffers")(n, ids.data());
    for (int i = 0; i < n; i++) {
        c.write_u32(c.arg32(1) + i * 4, ids[i]);
    }
}
void g1_bufferdata(Cpu& c) {
    GLenum t = c.arg32(0);
    GLsizeiptr sz = c.arg32(1);
    uint32_t d = c.arg32(2);
    GLenum u = c.arg32(3);
    std::vector<uint8_t> b(sz);
    if (d) {
        c.read_mem(d, b.data(), sz);
    }
    host<datafn>("glBufferData")(t, sz, d ? b.data() : nullptr, u);
}
// client arrays
void g1_enableclientstate(Cpu& c) {
    GLenum a = c.arg32(0);
    if (a == GL_VERTEX_ARRAY) {
        g_ff_vertex.enabled = true;
    } else if (a == GL_COLOR_ARRAY) {
        g_ff_color.enabled = true;
    } else if (a == GL_TEXTURE_COORD_ARRAY) {
        ff_texcoord().enabled = true;
    } else if (a == GL_NORMAL_ARRAY) {
        g_ff_normal.enabled = true;
    }
}
void g1_disableclientstate(Cpu& c) {
    GLenum a = c.arg32(0);
    if (a == GL_VERTEX_ARRAY) {
        g_ff_vertex.enabled = false;
    } else if (a == GL_COLOR_ARRAY) {
        g_ff_color.enabled = false;
    } else if (a == GL_TEXTURE_COORD_ARRAY) {
        ff_texcoord().enabled = false;
    } else if (a == GL_NORMAL_ARRAY) {
        g_ff_normal.enabled = false;
    }
}
GLuint cur_array_buffer() {
    GLint b = 0;
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &b);
    return (GLuint)b;
}
void g1_vertexpointer(Cpu& c) { g_ff_vertex = {g_ff_vertex.enabled, c.arg32(3), (GLint)c.arg32(0), c.arg32(1), (GLsizei)c.arg32(2), cur_array_buffer()}; }
void g1_colorpointer(Cpu& c) { g_ff_color = {g_ff_color.enabled, c.arg32(3), (GLint)c.arg32(0), c.arg32(1), (GLsizei)c.arg32(2), cur_array_buffer()}; }
void g1_texcoordpointer(Cpu& c) {
    auto& array = ff_texcoord();
    array = {array.enabled, c.arg32(3), (GLint)c.arg32(0), c.arg32(1), (GLsizei)c.arg32(2), cur_array_buffer()};
}
void g1_normalpointer(Cpu& c) { g_ff_normal = {g_ff_normal.enabled, c.arg32(2), 3, c.arg32(0), (GLsizei)c.arg32(1), cur_array_buffer()}; }
void ff_bind_all(Cpu& c, uint32_t vcount) {
    struct Binding { FFArray* array; GLenum state; void (*set)(const FFArray&, uint32_t); GLenum texture_unit = 0; };
    std::vector<Binding> bindings = {
        {&g_ff_vertex, GL_VERTEX_ARRAY, [](const FFArray& a, uint32_t off) { glVertexPointer(a.size, a.type, a.stride, (const void*)(uintptr_t)off); }},
        {&g_ff_color, GL_COLOR_ARRAY, [](const FFArray& a, uint32_t off) { glColorPointer(a.size, a.type, a.stride, (const void*)(uintptr_t)off); }},
        {&g_ff_normal, GL_NORMAL_ARRAY, [](const FFArray& a, uint32_t off) { glNormalPointer(a.type, a.stride, (const void*)(uintptr_t)off); }},
    };
    for (auto& entry : g_ff_texcoords) {
        bindings.push_back({&entry.second, GL_TEXTURE_COORD_ARRAY,
            [](const FFArray& a, uint32_t off) { glTexCoordPointer(a.size, a.type, a.stride, (const void*)(uintptr_t)off); }, entry.first});
    }
    GLint saved_client_unit = GL_TEXTURE0;
    glGetIntegerv(GL_CLIENT_ACTIVE_TEXTURE, &saved_client_unit);
    const auto activate = [](const Binding& binding) {
        if (binding.texture_unit) host<void (*)(GLenum)>("glClientActiveTexture")(binding.texture_unit);
    };
    const auto stride_of = [](const FFArray& a) { return a.stride ? a.stride : a.size * type_size(a.type); };
    std::vector<bool> uploaded(bindings.size());
    for (const auto& binding : bindings) {
        activate(binding);
        const auto& a = *binding.array;
        if (a.enabled && (a.vbo || a.ptr)) glEnableClientState(binding.state);
        else glDisableClientState(binding.state);
    }
    for (size_t slot = 0; slot < bindings.size(); ++slot) {
        const auto& binding = bindings[slot];
        const auto& a = *binding.array;
        if (!a.enabled || uploaded[slot] || (!a.vbo && !a.ptr)) continue;
        if (a.vbo) {
            // Guest VBOs stay on the GPU; pointers are offsets into the data
            // already uploaded by glBufferData (Fruit Ninja's static models).
            host<bindfn>("glBindBuffer")(GL_ARRAY_BUFFER, a.vbo);
            activate(binding);
            binding.set(a, a.ptr);
            continue;
        }

        // Fruit Ninja's sprites interleave positions, UVs and colors. Copy and
        // upload each shared record once instead of sending its full stride
        // separately for every array. Separate arrays keep separate streams.
        const uint32_t stride = stride_of(a);
        uint32_t base = a.ptr, last = a.ptr;
        uint64_t end = static_cast<uint64_t>(a.ptr) + a.size * type_size(a.type);
        std::vector<bool> members(bindings.size());
        members[slot] = true;
        for (size_t next = slot + 1; next < bindings.size(); ++next) {
            const auto& other = *bindings[next].array;
            if (!other.enabled || other.vbo || !other.ptr || uploaded[next] || stride_of(other) != stride) continue;
            const uint32_t first = std::min(base, other.ptr), final = std::max(last, other.ptr);
            if (final - first >= stride) continue;
            base = first;
            last = final;
            end = std::max(end, static_cast<uint64_t>(other.ptr) + other.size * type_size(other.type));
            members[next] = true;
        }
        const size_t bytes = vcount ? static_cast<size_t>(stride) * (vcount - 1) + end - base : 0;
        auto& stream = g_ff_streams[slot];
        // Like touchHLE's GL marshalling, use an immediate view of mapped
        // guest memory when available. BufferData consumes it before returning;
        // other regions keep the staging path, and guest writes stay coherent.
        const uint8_t* data = bytes <= UINT32_MAX ? c.memory_ptr(base, bytes) : nullptr;
        if (bytes && !data) {
            if (stream.staging.size() < bytes) stream.staging.resize(bytes);
            c.read_mem(base, stream.staging.data(), bytes);
            data = stream.staging.data();
        }
        if (!stream.buffer) host<genfn>("glGenBuffers")(1, &stream.buffer);
        host<bindfn>("glBindBuffer")(GL_ARRAY_BUFFER, stream.buffer);
        // BufferData lets the driver orphan in-flight storage. Updating the
        // same live storage with BufferSubData can stall a mobile GPU.
        host<datafn>("glBufferData")(GL_ARRAY_BUFFER, bytes, data, client_buffer_usage);
        for (size_t member = slot; member < bindings.size(); ++member) {
            if (!members[member]) continue;
            const auto& view = *bindings[member].array;
            activate(bindings[member]);
            bindings[member].set(view, view.ptr - base);
            uploaded[member] = true;
        }
    }
    host<void (*)(GLenum)>("glClientActiveTexture")(saved_client_unit);
}
void g1_drawarrays(Cpu& c) {
    GLenum m = c.arg32(0);
    GLint f = c.arg32(1);
    GLsizei n = c.arg32(2);
    static int dbg = getenv("BADA_GLDBG") ? 3 : 0;
    if (dbg) {
        dbg--;
        GLint tex = 0, vp[4] = {0, 0, 0, 0};
        GLfloat mv[16], pr[16], col[4];
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex);
        glGetIntegerv(GL_VIEWPORT, vp);
        glGetFloatv(GL_MODELVIEW_MATRIX, mv);
        glGetFloatv(GL_PROJECTION_MATRIX, pr);
        glGetFloatv(GL_CURRENT_COLOR, col);
        printf("[gldbg] draw mode=0x%x first=%d count=%d tex=%d tex2d=%d blend=%d depth=%d\n",
               m, f, n, tex, glIsEnabled(GL_TEXTURE_2D), glIsEnabled(GL_BLEND),
               glIsEnabled(GL_DEPTH_TEST));
        printf("[gldbg]  vp=%d,%d,%d,%d color=%.2f,%.2f,%.2f,%.2f\n", vp[0], vp[1], vp[2], vp[3],
               col[0], col[1], col[2], col[3]);
        printf("[gldbg]  proj  %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f\n", pr[0], pr[5], pr[10],
               pr[15], pr[12], pr[13], pr[14], pr[1]);
        printf("[gldbg]  model %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f\n", mv[0], mv[5], mv[10],
               mv[15], mv[12], mv[13], mv[14], mv[1]);
    }
    GLint save_abuf = 0;
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &save_abuf);
    ff_bind_all(c, f + n);
    {
        PresentationProjection projection;
        glDrawArrays(m, f, n);
    }
    host<bindfn>("glBindBuffer")(GL_ARRAY_BUFFER, (GLuint)save_abuf); // restore guest binding
}
void g1_drawelements(Cpu& c) {
    GLenum m = c.arg32(0);
    GLsizei n = c.arg32(1);
    GLenum t = c.arg32(2);
    uint32_t ip = c.arg32(3);
    GLint guest_ebo = 0, save_abuf = 0; // guest's bound buffers (restored at exit)
    glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &guest_ebo);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &save_abuf);

    auto& idx = g_ff_indices; // reusable host copy of client-side indices
    const void* host_idx;      // what we hand glDrawElements
    if (guest_ebo) {
        // Server-side indices (FruitNinja models): the guest's mirrored EBO stays
        // bound and `ip` is a byte offset into it. Vertex arrays are VBO-backed,
        // so vcount is unused by ff_bind_all.
        ff_bind_all(c, 0);
        host_idx = (const void*)(uintptr_t)ip;
    } else {
        // Client-side indices: read from guest memory, size client arrays by the
        // max index, draw from the host copy.
        uint32_t isz = type_size(t);
        const size_t bytes = static_cast<size_t>(n) * isz;
        if (idx.size() < bytes) idx.resize(bytes);
        c.read_mem(ip, idx.data(), bytes);
        uint32_t maxi = 0;
        for (int i = 0; i < n; i++) {
            uint32_t v = (isz == 1) ? idx[i] : (isz == 2) ? ((uint16_t*)idx.data())[i]
                                                          : ((uint32_t*)idx.data())[i];
            if (v > maxi) {
                maxi = v;
            }
        }
        ff_bind_all(c, maxi + 1);
        host<bindfn>("glBindBuffer")(GL_ELEMENT_ARRAY_BUFFER, 0);
        host_idx = idx.data();
    }
    {
        PresentationProjection projection;
        glDrawElements(m, n, t, host_idx);
    }
    // Restore the guest's buffer bindings (ff_bind/our client path mutate them).
    host<bindfn>("glBindBuffer")(GL_ARRAY_BUFFER, (GLuint)save_abuf);
    host<bindfn>("glBindBuffer")(GL_ELEMENT_ARRAY_BUFFER, (GLuint)guest_ebo);
}
void g1_getintegerv(Cpu& c) { getiv_into_guest(c, c.arg32(0), c.arg32(1)); }
void g1_getstring(Cpu& c) {
    static std::map<GLenum, uint32_t> cache;
    GLenum n = c.arg32(0);
    if (!cache.count(n)) {
        const char* s = n == 0x1F00 ? "Halfbrick" : n == 0x1F01 ? "bada-hle"
                                                : n == 0x1F02   ? "OpenGL ES-CM 1.1"
                                                                : "";
        uint32_t g = c.alloc(strlen(s) + 1);
        c.write_mem(g, s, strlen(s) + 1);
        cache[n] = g;
    }
    c.set_ret32(cache[n]);
}

// ============================ EGL (sgl*) stubs ============================
// SDL already created the real context; the EGL calls just need to "succeed".
constexpr uint32_t EGL_TRUE_ = 1, EGL_SUCCESS_ = 0x3000;
void egl_true(Cpu& c) { c.set_ret32(EGL_TRUE_); }
void egl_handle(Cpu& c) { c.set_ret32(1); } // nonzero display/surface/context
void egl_geterror(Cpu& c) { c.set_ret32(EGL_SUCCESS_); }
void egl_chooseconfig(Cpu& c) { // (dpy,attrs,configs,size,num)
    uint32_t configs = c.arg32(2), num = c.arg32(4);
    if (configs) {
        c.write_u32(configs, 1);
    }
    if (num) {
        c.write_u32(num, 1);
    }
    c.set_ret32(EGL_TRUE_);
}
// sglGetConfigAttrib(dpy, config, attr, &value)
void egl_getconfigattrib(Cpu& c) {
    uint32_t attr = c.arg32(2), out = c.arg32(3);
    uint32_t v = (attr == 0x3025) ? 24 : (attr == 0x3026) ? 8
                                                          : 8; // DEPTH=24, else 8
    if (out) {
        c.write_u32(out, v);
    }
    c.set_ret32(EGL_TRUE_);
}
std::function<void(Cpu&)> g_preswap; // host overlay, drawn into the guest's frame
void egl_swapbuffers(Cpu& c) {
    if (g_preswap) {
        g_preswap(c); // the app owns the swap, so this is the only seam for it
    }
    if (g_window) {
        int width = 0, height = 0;
        SDL_GL_GetDrawableSize(g_window, &width, &height);
        static int applied_width = 0, applied_height = 0;
        if (width != applied_width || height != applied_height) {
            applied_width = width; applied_height = height;
            const auto native = logical_surface();
            printf("[display] selected=%s native=%dx%d render=%dx%d applied=%dx%d rotation=%d\n",
                display::selected_resolution.c_str(), native.width, native.height,
                g_surface_width, g_surface_height, width, height, display::rotation * 90);
        }
        SDL_GL_SwapWindow(g_window); // present to the live window (eyeball)
    }
    c.set_ret32(EGL_TRUE_);
}

} // namespace

namespace hle {
void gl_set_window(void* w) { g_window = (SDL_Window*)w; }
void gl_set_surface_size(int width, int height, const char* source) {
    if (width < 64 || height < 64 || width > 4096 || height > 4096) return;
    g_surface_width = width;
    g_surface_height = height;
    if (g_logical_landscape >= 0)
        display::rotation = width == height || (width > height) == static_cast<bool>(g_logical_landscape) ? 0 : (width < height ? 1 : 3);
    const auto native = logical_surface();
    const auto buffer = display::buffer_size(native.width, native.height);
    printf("[display] source=%s native=%dx%d render=%dx%d selected=%s requested=%dx%d orientation=%s rotation=%d\n",
        source, native.width, native.height, width, height, display::selected_resolution.c_str(),
        buffer.width, buffer.height, native.width > native.height ? "landscape" : "portrait", display::rotation * 90);
    display::size_window(g_window, native.width, native.height);
    if (g_window) gl_resize();
}

void gl_set_window_orientation(bool landscape) {
    g_logical_landscape = landscape;
    gl_set_surface_size(g_surface_width, g_surface_height, "SDK logical window");
}

void gl_set_orientation(uint32_t orientation) {
    printf("[display] Bada orientation=%u\n", orientation);
    // Bada groups portrait/reverse portrait (0/1), then landscape/reverse
    // landscape (2/3). Automatic (4) is resolved by the first viewport.
    if (orientation > 3) return;
    const int short_side = std::min(display::device_width, display::device_height);
    const int long_side = std::max(display::device_width, display::device_height);
    const bool landscape = orientation >= 2;
    g_window_fit = false;
    gl_set_surface_size(landscape ? long_side : short_side, landscape ? short_side : long_side, "Bada Form/Frame");
}
SurfaceRect gl_content_rect(int width, int height) {
    const auto native = logical_surface();
    return display::content_rect(native.width, native.height, width, height);
}
void gl_map_touch(int x, int y, int width, int height, int& guest_x, int& guest_y) {
    const auto native = logical_surface();
    const auto area = gl_content_rect(width, height);
    const int top = height - area.y - area.height; // SDL input starts at the top
    const int logical_x = area.width ? std::clamp(static_cast<int>(
        static_cast<int64_t>(x - area.x) * native.width / area.width), 0, native.width - 1) : 0;
    const int logical_y = area.height ? std::clamp(static_cast<int>(
        static_cast<int64_t>(y - top) * native.height / area.height), 0, native.height - 1) : 0;
    switch (display::rotation & 3) {
    case 1: guest_x = g_surface_width - 1 - logical_y; guest_y = logical_x; break;
    case 2: guest_x = g_surface_width - 1 - logical_x; guest_y = g_surface_height - 1 - logical_y; break;
    case 3: guest_x = logical_y; guest_y = g_surface_height - 1 - logical_x; break;
    default: guest_x = logical_x; guest_y = logical_y; break;
    }
}
void gl_resize() { apply_viewport(); apply_scissor(); }
void gl_set_preswap(std::function<void(Cpu&)> fn) { g_preswap = std::move(fn); }
#ifdef BADA_GLES_BACKEND
// Implemented alongside the Android frontend, using ES arrays/shaders instead
// of desktop immediate mode. Keep the game's GL dispatch shared with desktop.
void gles_present_canvas(Cpu& cpu, void* window, uint32_t pixels, int width, int height);
CanvasUploadInfo gles_canvas_upload_info();
CanvasUploadInfo gl_canvas_upload_info() { return gles_canvas_upload_info(); }
void gl_present_canvas(Cpu& cpu, uint32_t pixels, int width, int height) {
    fit_window(0, 0, width, height);
    gles_present_canvas(cpu, g_window, pixels, width, height);
}
#else
CanvasUploadInfo gl_canvas_upload_info() { return g_canvas_upload_info; }
void gl_present_canvas(Cpu& cpu, uint32_t pixels, int width, int height) {
    if (!g_window || !pixels || width <= 0 || height <= 0) {
        return;
    }
    if (!g_canvas_window_sized) {
        fit_window(0, 0, width, height);
        g_canvas_window_sized = true;
    }
    std::vector<uint8_t> image((size_t)width * (size_t)height * 4);
    cpu.read_mem(pixels, image.data(), (uint32_t)image.size());
    g_canvas_upload_info = {static_cast<uint32_t>(image.size()), false};
    static int canvas_frames = 0;
    ++canvas_frames;
    if (canvas_frames <= 12 || canvas_frames % 100 == 0) {
        size_t nonzero = 0;
        for (uint8_t byte : image) {
            nonzero += byte != 0;
        }
        size_t mid = image.size() / 2;
        size_t last = image.size() - 4;
        printf("[canvas frame=%d] %dx%d nonzero=%zu/%zu first=%02x%02x%02x%02x mid=%02x%02x%02x%02x last=%02x%02x%02x%02x\n", canvas_frames, width, height, nonzero, image.size(), image[0], image[1], image[2], image[3], image[mid], image[mid + 1], image[mid + 2], image[mid + 3], image[last], image[last + 1], image[last + 2], image[last + 3]);
    }
    int ww = 0, wh = 0;
    SDL_GL_GetDrawableSize(g_window, &ww, &wh);
    if (ww <= 0 || wh <= 0) {
        SDL_GetWindowSize(g_window, &ww, &wh);
    }
    glPushAttrib(GL_ENABLE_BIT | GL_COLOR_BUFFER_BIT | GL_TEXTURE_BIT |
                 GL_VIEWPORT_BIT | GL_CURRENT_BIT | GL_TRANSFORM_BIT);
    glPushClientAttrib(GL_CLIENT_PIXEL_STORE_BIT);
    glViewport(0, 0, ww, wh);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    glOrtho(0.0, (GLdouble)ww, (GLdouble)wh, 0.0, -1.0, 1.0);
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    if (!g_canvas_texture) {
        glGenTextures(1, &g_canvas_texture);
    }
    glBindTexture(GL_TEXTURE_2D, g_canvas_texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, display::smooth ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, display::smooth ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, width, height, 0, GL_BGRA, GL_UNSIGNED_BYTE, image.data());
    const auto area = display::content_rect(width, height, ww, wh);
    float dw = area.width, dh = area.height;
    float left = area.x, top = wh - area.y - area.height;
    glEnable(GL_TEXTURE_2D);
    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
    glBegin(GL_QUADS);
    glTexCoord2f(0.0f, 0.0f); glVertex2f(left, top);
    glTexCoord2f(1.0f, 0.0f); glVertex2f(left + dw, top);
    glTexCoord2f(1.0f, 1.0f); glVertex2f(left + dw, top + dh);
    glTexCoord2f(0.0f, 1.0f); glVertex2f(left, top + dh);
    glEnd();
    glDisable(GL_TEXTURE_2D);
    SDL_GL_SwapWindow(g_window);
    glPopMatrix();
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glPopClientAttrib();
    glPopAttrib();
}
#endif

void register_gl() {
    register_handler("glClearColor_2", gl_clearcolor);
    register_handler("glClear_2", gl_clear);
    register_handler("glViewport_2", gl_viewport);
    register_handler("glScissor_2", gl_scissor);
    register_handler("glEnable_2", gl_enable);
    register_handler("glDisable_2", gl_disable);
    register_handler("glFinish_2", gl_finish);
    register_handler("glGetError_2", g1_geterror);
    register_handler("glGetBooleanv_2", gl_getbooleanv);
    register_handler("glGetIntegerv_2", gl_getintegerv);
    register_handler("glCreateShader_2", gl_createshader);
    register_handler("glCreateProgram_2", gl_createprogram);
    register_handler("glAttachShader_2", gl_attachshader);
    register_handler("glCompileShader_2", gl_compileshader);
    register_handler("glLinkProgram_2", gl_linkprogram);
    register_handler("glUseProgram_2", gl_useprogram);
    register_handler("glUniform1i_2", gl_uniform1i);
    register_handler("glDeleteShader_2", gl_deleteshader);
    register_handler("glDeleteProgram_2", gl_deleteprogram);
    register_handler("glGetString_2", gl_getstring);
    register_handler("glShaderSource_2", gl_shadersource);
    register_handler("glGetShaderiv_2", gl_getshaderiv);
    register_handler("glGetProgramiv_2", gl_getprogramiv);
    register_handler("glGetAttribLocation_2", gl_getattriblocation);
    register_handler("glGetUniformLocation_2", gl_getuniformlocation);
    register_handler("glBindAttribLocation_2", gl_bindattriblocation);
    register_handler("glEnableVertexAttribArray_2", gl_enablevattrib);
    register_handler("glDisableVertexAttribArray_2", gl_disablevattrib);
    register_handler("glVertexAttribPointer_2", gl_vattribpointer);
    register_handler("glUniformMatrix4fv_2", gl_uniformmatrix4fv);
    register_handler("glDrawElements_2", gl_drawelements);
    register_handler("glDrawArrays_2", gl_drawarrays);

    // These ES 2 entry points have the same argument layout as ES 1.
    register_handler("glClearDepthf_2", g1_cleardepthf);
    register_handler("glDepthFunc_2", g1_depthfunc);
    register_handler("glDepthMask_2", g1_depthmask);
    register_handler("glCullFace_2", g1_cullface);
    register_handler("glBlendFunc_2", g1_blendfunc);
    register_handler("glColorMask_2", g1_colormask);
    register_handler("glFrontFace_2", g1_frontface);
    register_handler("glHint_2", g1_hint);
    register_handler("glLineWidth_2", g1_linewidth);
    register_handler("glIsEnabled_2", g1_isenabled);
    register_handler("glFlush_2", g1_flush);
    register_handler("glActiveTexture_2", g1_activetexture);
    register_handler("glBindTexture_2", g1_bindtexture);
    register_handler("glTexParameteri_2", g1_texparameteri);
    register_handler("glGenTextures_2", g1_gentextures);
    register_handler("glDeleteTextures_2", g1_deletetextures);
    register_handler("glTexImage2D_2", g1_teximage2d);
    register_handler("glTexSubImage2D_2", g1_texsubimage2d);
    register_handler("glCompressedTexImage2D_2", g1_compressedteximage2d);
    register_handler("glPixelStorei_2", g1_pixelstorei);
    register_handler("glBindBuffer_2", g1_bindbuffer);
    register_handler("glGenBuffers_2", g1_genbuffers);
    register_handler("glBufferData_2", g1_bufferdata);
    register_handler("glBindFramebuffer_2", g1_bindframebuffer);
    register_handler("glGenFramebuffers_2", g1_genframebuffers);
    register_handler("glDeleteFramebuffers_2", g1_deleteframebuffers);
    register_handler("glFramebufferTexture2D_2", g1_framebuffertexture2d);
    register_handler("glCheckFramebufferStatus_2", g1_checkframebufferstatus);

    // ---- GL ES 1.x fixed-function (FruitNinja) ----
    register_handler("glClear_1", g1_clear);
    register_handler("glClearColor_1", g1_clearcolor);
    register_handler("glClearDepthf_1", g1_cleardepthf);
    register_handler("glEnable_1", g1_enable);
    register_handler("glDisable_1", g1_disable);
    register_handler("glDepthFunc_1", g1_depthfunc);
    register_handler("glDepthMask_1", g1_depthmask);
    register_handler("glCullFace_1", g1_cullface);
    register_handler("glBlendFunc_1", g1_blendfunc);
    register_handler("glColorMask_1", g1_colormask);
    register_handler("glColor4f_1", g1_color4f);
    register_handler("glFrontFace_1", g1_frontface);
    register_handler("glHint_1", g1_hint);
    register_handler("glLineWidth_1", g1_linewidth);
    register_handler("glPointSize_1", g1_pointsize);
    register_handler("glAlphaFunc_1", g1_alphafunc);
    register_handler("glFogf_1", g1_fogf);
    register_handler("glFogfv_1", g1_fogfv);
    register_handler("glLightModelfv_1", g1_lightmodelfv);
    register_handler("glMaterialfv_1", g1_materialfv);
    register_handler("glIsEnabled_1", g1_isenabled);
    register_handler("glGetBooleanv_1", gl_getbooleanv);
    register_handler("glGetFloatv_1", g1_getfloatv);
    register_handler("glGetPointerv_1", g1_getpointerv);
    register_handler("glShadeModel_1", g1_shademodel);
    register_handler("glViewport_1", g1_viewport);
    register_handler("glScissor_1", gl_scissor);
    register_handler("glFinish_1", g1_finish);
    register_handler("glFlush_1", g1_flush);
    register_handler("glColor4ub_1", g1_color4ub);
    register_handler("glMatrixMode_1", g1_matrixmode);
    register_handler("glLoadIdentity_1", g1_loadidentity);
    register_handler("glPushMatrix_1", g1_pushmatrix);
    register_handler("glPopMatrix_1", g1_popmatrix);
    register_handler("glFrustumf_1", g1_frustumf);
    register_handler("glOrthof_1", g1_orthof);
    register_handler("glTranslatef_1", g1_translatef);
    register_handler("glRotatef_1", g1_rotatef);
    register_handler("glScalef_1", g1_scalef);
    register_handler("glLoadMatrixf_1", g1_loadmatrixf);
    register_handler("glMultMatrixf_1", g1_multmatrixf);
    register_handler("glActiveTexture_1", g1_activetexture);
    register_handler("glClientActiveTexture_1", g1_clientactivetexture);
    register_handler("glBindTexture_1", g1_bindtexture);
    register_handler("glTexParameteri_1", g1_texparameteri);
    register_handler("glTexEnvi_1", g1_texenvi);
    register_handler("glTexEnvf_1", g1_texenvf);
    register_handler("glGenTextures_1", g1_gentextures);
    register_handler("glDeleteTextures_1", g1_deletetextures);
    register_handler("glTexImage2D_1", g1_teximage2d);
    register_handler("glTexSubImage2D_1", g1_texsubimage2d);
    register_handler("glCompressedTexImage2D_1", g1_compressedteximage2d);
    register_handler("glPixelStorei_1", g1_pixelstorei);
    register_handler("glGetError_1", g1_geterror);
    register_handler("glBindFramebufferOES_1", g1_bindframebuffer);
    register_handler("glGenFramebuffersOES_1", g1_genframebuffers);
    register_handler("glDeleteFramebuffersOES_1", g1_deleteframebuffers);
    register_handler("glFramebufferTexture2DOES_1", g1_framebuffertexture2d);
    register_handler("glCheckFramebufferStatusOES_1", g1_checkframebufferstatus);
    register_handler("glBindBuffer_1", g1_bindbuffer);
    register_handler("glGenBuffers_1", g1_genbuffers);
    register_handler("glBufferData_1", g1_bufferdata);
    register_handler("glEnableClientState_1", g1_enableclientstate);
    register_handler("glDisableClientState_1", g1_disableclientstate);
    register_handler("glVertexPointer_1", g1_vertexpointer);
    register_handler("glColorPointer_1", g1_colorpointer);
    register_handler("glTexCoordPointer_1", g1_texcoordpointer);
    register_handler("glNormalPointer_1", g1_normalpointer);
    register_handler("glDrawArrays_1", g1_drawarrays);
    register_handler("glDrawElements_1", g1_drawelements);
    register_handler("glGetIntegerv_1", g1_getintegerv);
    register_handler("glGetString_1", g1_getstring);

    // EGL via Samsung sgl* wrapper
    register_handler("sglBindAPI", egl_true);
    register_handler("sglGetDisplay", egl_handle);
    register_handler("sglInitialize", egl_true);
    register_handler("sglGetError", egl_geterror);
    register_handler("sglChooseConfig", egl_chooseconfig);
    register_handler("sglCreateWindowSurface", egl_handle);
    register_handler("sglCreateContext", egl_handle);
    register_handler("sglMakeCurrent", egl_true);
    register_handler("sglSwapBuffers", egl_swapbuffers);
    register_handler("sglGetConfigAttrib", egl_getconfigattrib);
    register_handler("sglTerminate", egl_true);
    register_handler("sglDestroyContext", egl_true);
    register_handler("sglDestroySurface", egl_true);
}
} // namespace hle
