#include "internal_resolution.h"
#include "display.h"
#include "render_queue.h"
#include <SDL2/SDL.h>
#ifdef BADA_GLES_BACKEND
#include <GLES/gl.h>
#include <GLES2/gl2.h>
#else
#include <SDL2/SDL_opengl.h>
#endif
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>

namespace internal_resolution {
namespace {
int scale = 100, surface_width = 0, surface_height = 0;
GLuint target = 0, texture = 0, depth = 0, stencil = 0, vertices = 0, program = 0;
bool paused = false, unavailable = false;
template<class F> F proc(const char* core, const char* extension = nullptr) {
    auto function = reinterpret_cast<F>(SDL_GL_GetProcAddress(core));
    if (!function && extension) function = reinterpret_cast<F>(SDL_GL_GetProcAddress(extension));
    return function;
}
using Bind = void (*)(GLenum, GLuint);
using Gen = void (*)(GLsizei, GLuint*);
using Delete = void (*)(GLsizei, const GLuint*);
Bind bind_target = nullptr;
Gen gen_target = nullptr, gen_renderbuffer = nullptr;
Delete delete_target = nullptr, delete_renderbuffer = nullptr;
using AttachTexture = void (*)(GLenum, GLenum, GLenum, GLuint, GLint);
using Storage = void (*)(GLenum, GLenum, GLsizei, GLsizei);
using AttachBuffer = void (*)(GLenum, GLenum, GLenum, GLuint);
AttachTexture attach_texture = nullptr;
Storage storage = nullptr;
AttachBuffer attach_buffer = nullptr;
Bind bind_renderbuffer = nullptr;
using Status = GLenum (*)(GLenum);
Status status = nullptr;

bool resolve(int api) {
#ifdef BADA_GLES_BACKEND
    const bool oes = api == 1;
#else
    const bool oes = false;
#endif
    bind_target = proc<Bind>(oes ? "glBindFramebufferOES" : "glBindFramebuffer", "glBindFramebufferEXT");
    gen_target = proc<Gen>(oes ? "glGenFramebuffersOES" : "glGenFramebuffers", "glGenFramebuffersEXT");
    delete_target = proc<Delete>(oes ? "glDeleteFramebuffersOES" : "glDeleteFramebuffers", "glDeleteFramebuffersEXT");
    gen_renderbuffer = proc<Gen>(oes ? "glGenRenderbuffersOES" : "glGenRenderbuffers", "glGenRenderbuffersEXT");
    delete_renderbuffer = proc<Delete>(oes ? "glDeleteRenderbuffersOES" : "glDeleteRenderbuffers", "glDeleteRenderbuffersEXT");
    bind_renderbuffer = proc<Bind>(oes ? "glBindRenderbufferOES" : "glBindRenderbuffer", "glBindRenderbufferEXT");
    attach_texture = proc<AttachTexture>(oes ? "glFramebufferTexture2DOES" : "glFramebufferTexture2D", "glFramebufferTexture2DEXT");
    storage = proc<Storage>(oes ? "glRenderbufferStorageOES" : "glRenderbufferStorage", "glRenderbufferStorageEXT");
    attach_buffer = proc<AttachBuffer>(oes ? "glFramebufferRenderbufferOES" : "glFramebufferRenderbuffer", "glFramebufferRenderbufferEXT");
    status = proc<Status>(oes ? "glCheckFramebufferStatusOES" : "glCheckFramebufferStatus", "glCheckFramebufferStatusEXT");
    return bind_target && gen_target && delete_target && gen_renderbuffer && delete_renderbuffer &&
        bind_renderbuffer && attach_texture && storage && attach_buffer && status;
}

bool allocate(int width, int height, int api) {
    if (target && width == surface_width && height == surface_height) { bind_target(GL_FRAMEBUFFER, target); return true; }
    if (!resolve(api)) return false;
    GLint old_texture = 0, old_renderbuffer = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &old_texture);
    glGetIntegerv(GL_RENDERBUFFER_BINDING, &old_renderbuffer);
    if (target) {
        delete_target(1, &target);
        delete_renderbuffer(1, &depth);
        delete_renderbuffer(1, &stencil);
        glDeleteTextures(1, &texture);
    }
    gen_target(1, &target);
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    bind_target(GL_FRAMEBUFFER, target);
    attach_texture(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
    gen_renderbuffer(1, &depth);
    bind_renderbuffer(GL_RENDERBUFFER, depth);
    const char* extensions = reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS));
#ifdef BADA_GLES_BACKEND
    const bool packed = extensions && (std::strstr(extensions, "GL_OES_packed_depth_stencil") || std::strstr(extensions, "GL_EXT_packed_depth_stencil"));
#else
    const bool packed = true;
    (void)extensions;
#endif
    storage(GL_RENDERBUFFER, packed ? 0x88F0 : GL_DEPTH_COMPONENT16, width, height);
    attach_buffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depth);
    // Preserve stencil operations in games that use masks or shadows.
    stencil = 0;
    if (packed) attach_buffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, depth);
    else {
        gen_renderbuffer(1, &stencil);
        bind_renderbuffer(GL_RENDERBUFFER, stencil);
        storage(GL_RENDERBUFFER, GL_STENCIL_INDEX8, width, height);
        attach_buffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, stencil);
    }
    const bool complete = status(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    glBindTexture(GL_TEXTURE_2D, old_texture);
    bind_renderbuffer(GL_RENDERBUFFER, old_renderbuffer);
    if (!complete) {
        bind_target(GL_FRAMEBUFFER, 0);
        delete_target(1, &target); target = 0;
        delete_renderbuffer(1, &depth); depth = 0;
        delete_renderbuffer(1, &stencil); stencil = 0;
        glDeleteTextures(1, &texture); texture = 0;
        return false;
    }
    surface_width = width; surface_height = height;
    return true;
}

void ensure_vertices() {
    if (vertices) return;
    constexpr GLfloat quad[] = {-1,-1,0,0, 1,-1,1,0, -1,1,0,1, 1,1,1,1};
    proc<Gen>("glGenBuffers")(1, &vertices);
    proc<Bind>("glBindBuffer")(GL_ARRAY_BUFFER, vertices);
    proc<void (*)(GLenum, GLsizeiptr, const void*, GLenum)>("glBufferData")(
        GL_ARRAY_BUFFER, sizeof quad, quad, GL_STATIC_DRAW);
}
GLuint shader(GLenum type, const char* source) {
    const GLuint object = proc<GLuint (*)(GLenum)>("glCreateShader")(type);
    proc<void (*)(GLuint, GLsizei, const char* const*, const GLint*)>("glShaderSource")(object, 1, &source, nullptr);
    proc<void (*)(GLuint)>("glCompileShader")(object);
    return object;
}
bool ensure_program() {
    if (program) return true;
    const GLuint vertex = shader(GL_VERTEX_SHADER,
        "attribute vec2 a_position;attribute vec2 a_uv;varying mediump vec2 v_uv;"
        "void main(){gl_Position=vec4(a_position,0.0,1.0);v_uv=a_uv;}");
    const GLuint fragment = shader(GL_FRAGMENT_SHADER,
        "precision mediump float;varying mediump vec2 v_uv;uniform sampler2D u_texture;"
        "void main(){gl_FragColor=texture2D(u_texture,v_uv);}");
    program = proc<GLuint (*)()>("glCreateProgram")();
    proc<void (*)(GLuint, GLuint)>("glAttachShader")(program, vertex);
    proc<void (*)(GLuint, GLuint)>("glAttachShader")(program, fragment);
    auto bind_attribute = proc<void (*)(GLuint, GLuint, const char*)>("glBindAttribLocation");
    bind_attribute(program, 0, "a_position"); bind_attribute(program, 1, "a_uv");
    proc<void (*)(GLuint)>("glLinkProgram")(program);
    proc<void (*)(GLuint)>("glDeleteShader")(vertex);
    proc<void (*)(GLuint)>("glDeleteShader")(fragment);
    GLint linked = 0;
    proc<void (*)(GLuint, GLenum, GLint*)>("glGetProgramiv")(program, GL_LINK_STATUS, &linked);
    if (!linked) { proc<void (*)(GLuint)>("glDeleteProgram")(program); program = 0; }
    return linked != 0;
}

void draw(SDL_Window* window, int width, int height, int api) {
    performance::Scope timing(performance::Work::driver_present);
    GLint old_viewport[4], old_array = 0, old_active = GL_TEXTURE0, old_texture = 0;
    GLfloat old_clear[4];
    glGetIntegerv(GL_VIEWPORT, old_viewport); glGetFloatv(GL_COLOR_CLEAR_VALUE, old_clear);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &old_array);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &old_active);
    auto active_texture = proc<void (*)(GLenum)>("glActiveTexture");
    active_texture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &old_texture);
    constexpr GLenum caps[] = {GL_DEPTH_TEST, GL_STENCIL_TEST, GL_BLEND, GL_CULL_FACE, GL_SCISSOR_TEST};
    std::array<GLboolean, 5> old_caps{};
    for (size_t i = 0; i < old_caps.size(); ++i) { old_caps[i] = glIsEnabled(caps[i]); glDisable(caps[i]); }
    GLboolean old_mask[4]; glGetBooleanv(GL_COLOR_WRITEMASK, old_mask); glColorMask(1,1,1,1);
    bind_target(GL_FRAMEBUFFER, 0);
    glClearColor(0,0,0,1); glClear(GL_COLOR_BUFFER_BIT);
    int drawable_width = 0, drawable_height = 0;
    SDL_GL_GetDrawableSize(window, &drawable_width, &drawable_height);
    const auto area = display::content_rect(width, height, drawable_width, drawable_height);
    glViewport(area.x, area.y, area.width, area.height);
    glBindTexture(GL_TEXTURE_2D, texture);
    ensure_vertices(); proc<Bind>("glBindBuffer")(GL_ARRAY_BUFFER, vertices);
    if (api == 1) {
        constexpr GLenum fixed_caps[] = {GL_LIGHTING, GL_FOG, GL_ALPHA_TEST};
        std::array<GLboolean, 3> old_fixed{};
        for (size_t i = 0; i < old_fixed.size(); ++i) { old_fixed[i] = glIsEnabled(fixed_caps[i]); glDisable(fixed_caps[i]); }
        GLint units = 1, old_client = GL_TEXTURE0, old_mode = GL_MODELVIEW, old_env = GL_MODULATE;
        glGetIntegerv(GL_MAX_TEXTURE_UNITS, &units);
        glGetIntegerv(GL_CLIENT_ACTIVE_TEXTURE, &old_client); glGetIntegerv(GL_MATRIX_MODE, &old_mode);
        std::array<GLboolean, 32> texturing{};
        for (int unit = 0; unit < std::min(units, 32); ++unit) {
            active_texture(GL_TEXTURE0 + unit); texturing[unit] = glIsEnabled(GL_TEXTURE_2D);
            glDisable(GL_TEXTURE_2D);
        }
        active_texture(GL_TEXTURE0); glEnable(GL_TEXTURE_2D);
        glGetTexEnviv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, &old_env); glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
        for (GLenum mode : {GL_TEXTURE, GL_PROJECTION, GL_MODELVIEW}) { glMatrixMode(mode); glPushMatrix(); glLoadIdentity(); }
        proc<void (*)(GLenum)>("glClientActiveTexture")(GL_TEXTURE0);
        const GLboolean color_array = glIsEnabled(GL_COLOR_ARRAY), normal_array = glIsEnabled(GL_NORMAL_ARRAY);
        const GLboolean vertex_array = glIsEnabled(GL_VERTEX_ARRAY), uv_array = glIsEnabled(GL_TEXTURE_COORD_ARRAY);
        glDisableClientState(GL_COLOR_ARRAY); glDisableClientState(GL_NORMAL_ARRAY);
        glEnableClientState(GL_VERTEX_ARRAY); glEnableClientState(GL_TEXTURE_COORD_ARRAY);
        glVertexPointer(2, GL_FLOAT, 16, nullptr); glTexCoordPointer(2, GL_FLOAT, 16, reinterpret_cast<const void*>(8));
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        if (!vertex_array) glDisableClientState(GL_VERTEX_ARRAY);
        if (!uv_array) glDisableClientState(GL_TEXTURE_COORD_ARRAY);
        if (color_array) glEnableClientState(GL_COLOR_ARRAY);
        if (normal_array) glEnableClientState(GL_NORMAL_ARRAY);
        proc<void (*)(GLenum)>("glClientActiveTexture")(old_client);
        for (GLenum mode : {GL_MODELVIEW, GL_PROJECTION, GL_TEXTURE}) { glMatrixMode(mode); glPopMatrix(); }
        glMatrixMode(old_mode); glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, old_env);
        for (int unit = 0; unit < std::min(units, 32); ++unit) { active_texture(GL_TEXTURE0 + unit); if (texturing[unit]) glEnable(GL_TEXTURE_2D); }
        for (size_t i = 0; i < old_fixed.size(); ++i) if (old_fixed[i]) glEnable(fixed_caps[i]);
    } else if (ensure_program()) {
        auto get_attribute = proc<void (*)(GLuint, GLenum, GLint*)>("glGetVertexAttribiv");
        auto pointer = proc<void (*)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void*)>("glVertexAttribPointer");
        auto enable = proc<void (*)(GLuint)>("glEnableVertexAttribArray");
        auto disable = proc<void (*)(GLuint)>("glDisableVertexAttribArray");
        GLint old_program = 0; glGetIntegerv(GL_CURRENT_PROGRAM, &old_program);
        struct Attribute { GLint enabled, size, type, normalized, stride, buffer; void* pointer; } old[2]{};
        for (GLuint index = 0; index < 2; ++index) {
            get_attribute(index, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &old[index].enabled);
            get_attribute(index, GL_VERTEX_ATTRIB_ARRAY_SIZE, &old[index].size);
            get_attribute(index, GL_VERTEX_ATTRIB_ARRAY_TYPE, &old[index].type);
            get_attribute(index, GL_VERTEX_ATTRIB_ARRAY_NORMALIZED, &old[index].normalized);
            get_attribute(index, GL_VERTEX_ATTRIB_ARRAY_STRIDE, &old[index].stride);
            get_attribute(index, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING, &old[index].buffer);
            proc<void (*)(GLuint, GLenum, void**)>("glGetVertexAttribPointerv")(index, GL_VERTEX_ATTRIB_ARRAY_POINTER, &old[index].pointer);
            enable(index); pointer(index, 2, GL_FLOAT, 0, 16, reinterpret_cast<const void*>(index * 8));
        }
        proc<void (*)(GLuint)>("glUseProgram")(program);
        const GLint sampler = proc<GLint (*)(GLuint, const char*)>("glGetUniformLocation")(program, "u_texture");
        proc<void (*)(GLint, GLint)>("glUniform1i")(sampler, 0);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        proc<void (*)(GLuint)>("glUseProgram")(old_program);
        for (GLuint index = 0; index < 2; ++index) {
            proc<Bind>("glBindBuffer")(GL_ARRAY_BUFFER, old[index].buffer);
            pointer(index, old[index].size, old[index].type, old[index].normalized, old[index].stride, old[index].pointer);
            if (!old[index].enabled) disable(index);
        }
    }
    active_texture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, old_texture); active_texture(old_active);
    proc<Bind>("glBindBuffer")(GL_ARRAY_BUFFER, old_array);
    for (size_t i = 0; i < old_caps.size(); ++i) if (old_caps[i]) glEnable(caps[i]);
    glColorMask(old_mask[0], old_mask[1], old_mask[2], old_mask[3]);
    glClearColor(old_clear[0], old_clear[1], old_clear[2], old_clear[3]);
    glViewport(old_viewport[0], old_viewport[1], old_viewport[2], old_viewport[3]);
}
} // namespace

void configure(int percent) { scale = percent; }
int percent() { return scale; }
bool enabled() { return scale < 100 && target && !paused; }
uint32_t framebuffer() { return target; }
bool begin(int width, int height, int api) {
    if (scale == 100 || paused || unavailable) return false;
    const bool success = render::sync([=] { return allocate(std::max(1, width * scale / 100), std::max(1, height * scale / 100), api); });
    if (!success) { unavailable = true; puts("[gl] lower 3D resolution unavailable for this context; using native resolution"); }
    return success;
}
void pause() { if (enabled()) { render::sync([] { bind_target(GL_FRAMEBUFFER, 0); }); paused = true; } }
void resume() { if (target && paused) { render::sync([] { bind_target(GL_FRAMEBUFFER, target); }); paused = false; } }
void present(SDL_Window* window, int width, int height, int api) {
    if (!enabled()) return;
    render::sync([=] { draw(window, width, height, api); });
    paused = true; // keep the window bound until the host overlay and swap finish
}
void reset() { target = texture = depth = stencil = vertices = program = 0; surface_width = surface_height = 0; paused = unavailable = false; }
} // namespace internal_resolution
