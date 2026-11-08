#include "cpu.h"
#include "hle/dispatch.h"
#include "hle/gl_api.h"
#include "pixel_accel.h"
#include <GLES/gl.h>
#include <GLES2/gl2.h>
#include <SDL2/SDL.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <new>
#include <vector>

namespace {
GLuint program_id = 0;
GLuint texture_id = 0;
GLint position_location = -1;
GLint uv_location = -1;
GLint sampler_location = -1;
GLint rgb565_location = -1;
GLint surface_location = -1, guest_size_location = -1, crop_location = -1;
bool exact_coordinates = false;
int texture_width = 0, texture_height = 0;
std::vector<uint8_t> canvas_image;
GLuint source_texture = 0, overlay_texture = 0;
int source_width = 0, source_height = 0, overlay_width = 0, overlay_height = 0;
std::vector<uint16_t> packed_source;
std::vector<uint8_t> overlay_image;
hle::CanvasUploadInfo upload_info;

GLuint compile_shader(GLenum type, const char* source) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint compiled = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (!compiled) {
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

bool ensure_program() {
    if (program_id && glIsProgram(program_id)) return true;
    GLint range[2], precision = 0;
    glGetShaderPrecisionFormat(GL_FRAGMENT_SHADER, GL_HIGH_FLOAT, range, &precision);
    exact_coordinates = precision > 0;
    const char* vertex_source = exact_coordinates ?
        "attribute vec2 a_position;void main(){gl_Position=vec4(a_position,0.0,1.0);}" :
        "attribute vec2 a_position;attribute vec2 a_uv;varying mediump vec2 v_uv;"
        "void main(){gl_Position=vec4(a_position,0.0,1.0);v_uv=a_uv;}";
    const char* fragment_source = exact_coordinates ?
        "precision highp float;"
        "uniform sampler2D u_texture;"
        "uniform bool u_rgb565;"
        "uniform vec4 u_surface;uniform vec2 u_guest_size;uniform vec4 u_crop;"
        // bada Canvas is 0xAARRGGBB (BGRA bytes on the little-endian guest).
        // Sky Force leaves alpha zero on its opaque software framebuffer.
        // Decode RGB565 with the same five-bit green and <<3 channel values
        // as the verified ARM kernel; ordinary RGBA uploads need BGRA swizzle.
        // Snap both layers to the same guest pixel. Local UV interpolation of
        // a cropped HUD rectangle can otherwise pick a neighbouring texel.
        "void main(){vec2 p=floor(vec2((gl_FragCoord.x-u_surface.x)*u_guest_size.x/u_surface.z,"
        "(u_surface.y+u_surface.w-gl_FragCoord.y)*u_guest_size.y/u_surface.w));"
        "if(any(lessThan(p,u_crop.xy))||any(greaterThanEqual(p,u_crop.xy+u_crop.zw)))discard;"
        "vec2 uv=(p-u_crop.xy+0.5)/u_crop.zw;vec3 c=texture2D(u_texture,uv).rgb;"
        "if(u_rgb565){c=floor(c*vec3(31.0,63.0,31.0)+0.5);c.g=floor(c.g*0.5);c*=8.0/255.0;}"
        "else{c=c.bgr;}gl_FragColor=vec4(c,1.0);}" :
        "precision mediump float;varying mediump vec2 v_uv;uniform sampler2D u_texture;"
        "void main(){gl_FragColor=vec4(texture2D(u_texture,v_uv).bgr,1.0);}";
    GLuint vertex = compile_shader(GL_VERTEX_SHADER, vertex_source);
    GLuint fragment = compile_shader(GL_FRAGMENT_SHADER, fragment_source);
    if (!vertex || !fragment) {
        if (vertex) glDeleteShader(vertex);
        if (fragment) glDeleteShader(fragment);
        return false;
    }
    program_id = glCreateProgram();
    glAttachShader(program_id, vertex);
    glAttachShader(program_id, fragment);
    glLinkProgram(program_id);
    glDeleteShader(vertex);
    glDeleteShader(fragment);
    GLint linked = 0;
    glGetProgramiv(program_id, GL_LINK_STATUS, &linked);
    if (!linked) {
        glDeleteProgram(program_id);
        program_id = 0;
        return false;
    }
    position_location = glGetAttribLocation(program_id, "a_position");
    uv_location = glGetAttribLocation(program_id, "a_uv");
    sampler_location = glGetUniformLocation(program_id, "u_texture");
    rgb565_location = glGetUniformLocation(program_id, "u_rgb565");
    surface_location = glGetUniformLocation(program_id, "u_surface");
    guest_size_location = glGetUniformLocation(program_id, "u_guest_size");
    crop_location = glGetUniformLocation(program_id, "u_crop");
    return position_location >= 0 && sampler_location >= 0 && (exact_coordinates ?
        rgb565_location >= 0 && surface_location >= 0 && guest_size_location >= 0 && crop_location >= 0 : uv_location >= 0);
}

void upload_texture(GLuint& id, int& stored_width, int& stored_height, int width, int height,
                    GLenum format, GLenum type, const void* data) {
    if (!id || !glIsTexture(id)) {
        glGenTextures(1, &id);
        stored_width = stored_height = 0;
        glBindTexture(GL_TEXTURE_2D, id);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    } else {
        glBindTexture(GL_TEXTURE_2D, id);
    }
    if (stored_width != width || stored_height != height) {
        glTexImage2D(GL_TEXTURE_2D, 0, format, width, height, 0, format, type, data);
        stored_width = width;
        stored_height = height;
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, format, type, data);
    }
}

bool compact_frame(Cpu& cpu, uint32_t pixels, int width, int height, hle::SurfaceRect& overlay) {
    pixel_accel::Expansion expansion;
    if (!pixel_accel::snapshot(cpu, pixels, width, height, expansion)) return false;
    int left = width, top = height, right = -1, bottom = -1;
    for (int y = 0; y < height; ++y) {
        const uint8_t* actual = canvas_image.data() + static_cast<size_t>(y) * width * 4;
        const uint32_t* original = expansion.expanded + static_cast<size_t>(y) * width;
        if (std::memcmp(actual, original, width * 4) == 0) continue;
        for (int x = 0; x < width; ++x) {
            if (std::memcmp(actual + x * 4, original + x, 4) == 0) continue;
            left = std::min(left, x); top = std::min(top, y);
            right = std::max(right, x); bottom = std::max(bottom, y);
        }
    }
    overlay = {0, 0, 0, 0};
    if (right >= 0) {
        // Include unchanged neighbours so rectangle edges never clip a changed
        // pixel when the guest image is scaled to a different drawable size.
        left = std::max(0, left - 1); top = std::max(0, top - 1);
        right = std::min(width - 1, right + 1); bottom = std::min(height - 1, bottom + 1);
        overlay = {left, top, right - left + 1, bottom - top + 1};
    }
    const size_t source_bytes = static_cast<size_t>(240) * expansion.rows * 2;
    const size_t overlay_bytes = static_cast<size_t>(overlay.width) * overlay.height * 4;
    if (source_bytes + overlay_bytes >= canvas_image.size()) return false;
    try {
        packed_source.resize(240 * expansion.rows);
        overlay_image.resize(overlay_bytes);
    } catch (const std::bad_alloc&) {
        return false;
    }
    for (uint32_t row = 0; row < expansion.rows; ++row)
        std::memcpy(packed_source.data() + row * 240, expansion.source + row * 256, 240 * sizeof(uint16_t));
    for (int row = 0; row < overlay.height; ++row)
        std::memcpy(overlay_image.data() + static_cast<size_t>(row) * overlay.width * 4,
                    canvas_image.data() + (static_cast<size_t>(overlay.y + row) * width + overlay.x) * 4, overlay.width * 4);
    return true;
}

void draw_quad(const GLfloat* vertices, bool rgb565, const hle::SurfaceRect& crop) {
    if (exact_coordinates) {
        glUniform1i(rgb565_location, rgb565);
        glUniform4f(crop_location, crop.x, crop.y, crop.width, crop.height);
    }
    glVertexAttribPointer(position_location, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), vertices);
    if (!exact_coordinates) glVertexAttribPointer(uv_location, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), vertices + 2);
    glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
}
}

namespace hle {
CanvasUploadInfo gles_canvas_upload_info() { return upload_info; }

void gles_present_canvas(Cpu& cpu, void* sdl_window, uint32_t pixels, int width, int height) {
    upload_info = {};
    auto* window = static_cast<SDL_Window*>(sdl_window);
    if (!window || !pixels || width <= 0 || height <= 0) return;
    const size_t byte_count = static_cast<size_t>(width) * static_cast<size_t>(height) * 4;
    if (byte_count > 64 * 1024 * 1024) return;
    canvas_image.resize(byte_count);
    cpu.read_mem(pixels, canvas_image.data(), static_cast<uint32_t>(canvas_image.size()));

    int drawable_width = 0;
    int drawable_height = 0;
    SDL_GL_GetDrawableSize(window, &drawable_width, &drawable_height);
    if (drawable_width <= 0 || drawable_height <= 0) {
        SDL_GetWindowSize(window, &drawable_width, &drawable_height);
    }
    if (drawable_width <= 0 || drawable_height <= 0) return;

    int major = 0;
    SDL_GL_GetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, &major);
    if (major >= 2 && !ensure_program()) return;
    if (major == 1) {
        // BGRA textures are optional in ES 1. Convert on the host instead.
        for (size_t pixel = 0; pixel < byte_count; pixel += 4) {
            std::swap(canvas_image[pixel], canvas_image[pixel + 2]);
            canvas_image[pixel + 3] = 255;
        }
    }
    SurfaceRect overlay;
    const bool compact = major >= 2 && exact_coordinates && compact_frame(cpu, pixels, width, height, overlay);

    GLint old_viewport[4], old_buffer = 0, old_active_texture = 0, old_texture = 0, old_program = 0, old_unpack = 4;
    GLfloat old_clear_color[4];
    glGetFloatv(GL_COLOR_CLEAR_VALUE, old_clear_color);
    glGetIntegerv(GL_VIEWPORT, old_viewport);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &old_buffer);
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &old_unpack);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &old_active_texture);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &old_texture);
    if (major >= 2) glGetIntegerv(GL_CURRENT_PROGRAM, &old_program);
    const GLboolean depth = glIsEnabled(GL_DEPTH_TEST), blend = glIsEnabled(GL_BLEND);
    const GLboolean cull = glIsEnabled(GL_CULL_FACE), scissor = glIsEnabled(GL_SCISSOR_TEST);

    glDisable(GL_SCISSOR_TEST);
    glViewport(0, 0, drawable_width, drawable_height);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    const auto area = fit_surface(width, height, drawable_width, drawable_height);
    glViewport(area.x, area.y, area.width, area.height);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glActiveTexture(GL_TEXTURE0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    if (compact) {
        upload_texture(source_texture, source_width, source_height, 240, height / 2,
                       GL_RGB, GL_UNSIGNED_SHORT_5_6_5, packed_source.data());
        upload_info = {static_cast<uint32_t>(packed_source.size() * 2 + overlay_image.size()), true};
        static bool logged = false;
        if (!logged) {
            puts("[gl] Canvas GPU upload: RGB565 source plus changed RGBA rectangle");
            logged = true;
        }
    } else {
        upload_texture(texture_id, texture_width, texture_height, width, height,
                       GL_RGBA, GL_UNSIGNED_BYTE, canvas_image.data());
        upload_info = {static_cast<uint32_t>(byte_count), false};
    }

    const GLfloat vertices[] = {
        -1.0f,  1.0f, 0.0f, 0.0f,
         1.0f,  1.0f, 1.0f, 0.0f,
         1.0f, -1.0f, 1.0f, 1.0f,
        -1.0f, -1.0f, 0.0f, 1.0f,
    };
    if (major == 1) {
        // ES 1 games can also display Osp Canvas content. Immediate mode and
        // shaders are unavailable here; use the native fixed-function arrays.
        GLint matrix_mode = GL_MODELVIEW;
        GLint client_texture = GL_TEXTURE0, texture_mode = GL_MODULATE;
        GLfloat color[4];
        const GLboolean texturing = glIsEnabled(GL_TEXTURE_2D);
        glGetIntegerv(GL_MATRIX_MODE, &matrix_mode);
        glGetIntegerv(GL_CLIENT_ACTIVE_TEXTURE, &client_texture);
        glGetFloatv(GL_CURRENT_COLOR, color);
        glGetTexEnviv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, &texture_mode);
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
        glMatrixMode(GL_PROJECTION);
        glPushMatrix();
        glLoadIdentity();
        glMatrixMode(GL_MODELVIEW);
        glPushMatrix();
        glLoadIdentity();
        glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
        glEnable(GL_TEXTURE_2D);
        glClientActiveTexture(GL_TEXTURE0);
        glDisableClientState(GL_COLOR_ARRAY);
        glDisableClientState(GL_NORMAL_ARRAY);
        glEnableClientState(GL_VERTEX_ARRAY);
        glEnableClientState(GL_TEXTURE_COORD_ARRAY);
        glVertexPointer(2, GL_FLOAT, 4 * sizeof(GLfloat), vertices);
        glTexCoordPointer(2, GL_FLOAT, 4 * sizeof(GLfloat), vertices + 2);
        glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
        glDisableClientState(GL_VERTEX_ARRAY);
        glDisableClientState(GL_TEXTURE_COORD_ARRAY);
        glPopMatrix();
        glMatrixMode(GL_PROJECTION);
        glPopMatrix();
        glMatrixMode(matrix_mode);
        glClientActiveTexture(client_texture);
        glColor4f(color[0], color[1], color[2], color[3]);
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, texture_mode);
        if (!texturing) glDisable(GL_TEXTURE_2D);
    } else {
        glUseProgram(program_id);
        glUniform1i(sampler_location, 0);
        if (exact_coordinates) {
            glUniform4f(surface_location, area.x, area.y, area.width, area.height);
            glUniform2f(guest_size_location, width, height);
        }
        glEnableVertexAttribArray(position_location);
        if (!exact_coordinates) glEnableVertexAttribArray(uv_location);
        draw_quad(vertices, compact, {0, 0, width, height});
        if (compact && overlay.width) {
            upload_texture(overlay_texture, overlay_width, overlay_height, overlay.width, overlay.height,
                           GL_RGBA, GL_UNSIGNED_BYTE, overlay_image.data());
            const GLfloat left = -1 + 2.0f * overlay.x / width, right = -1 + 2.0f * (overlay.x + overlay.width) / width;
            const GLfloat top = 1 - 2.0f * overlay.y / height, bottom = 1 - 2.0f * (overlay.y + overlay.height) / height;
            const GLfloat rectangle[] = {left,top,0,0, right,top,1,0, right,bottom,1,1, left,bottom,0,1};
            draw_quad(rectangle, false, overlay);
        }
        glDisableVertexAttribArray(position_location);
        if (!exact_coordinates) glDisableVertexAttribArray(uv_location);
        glUseProgram(old_program);
    }
    SDL_GL_SwapWindow(window);
    glBindBuffer(GL_ARRAY_BUFFER, old_buffer);
    glBindTexture(GL_TEXTURE_2D, old_texture);
    glActiveTexture(old_active_texture);
    glPixelStorei(GL_UNPACK_ALIGNMENT, old_unpack);
    glClearColor(old_clear_color[0], old_clear_color[1], old_clear_color[2], old_clear_color[3]);
    if (depth) glEnable(GL_DEPTH_TEST);
    if (blend) glEnable(GL_BLEND);
    if (cull) glEnable(GL_CULL_FACE);
    if (scissor) glEnable(GL_SCISSOR_TEST);
    glViewport(old_viewport[0], old_viewport[1], old_viewport[2], old_viewport[3]);
}

}
