#include "cpu.h"
#include "hle/dispatch.h"
#include <GLES/gl.h>
#include <GLES2/gl2.h>
#include <SDL2/SDL.h>
#include <algorithm>
#include <cstdint>
#include <vector>

namespace {
GLuint program_id = 0;
GLuint texture_id = 0;
GLint position_location = -1;
GLint uv_location = -1;
GLint sampler_location = -1;

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
    const char* vertex_source =
        "attribute vec2 a_position;"
        "attribute vec2 a_uv;"
        "varying vec2 v_uv;"
        "void main(){gl_Position=vec4(a_position,0.0,1.0);v_uv=a_uv;}";
    const char* fragment_source =
        "precision mediump float;"
        "varying vec2 v_uv;"
        "uniform sampler2D u_texture;"
        "void main(){gl_FragColor=texture2D(u_texture,v_uv);}";
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
    return position_location >= 0 && uv_location >= 0 && sampler_location >= 0;
}
}

namespace hle {

void gles_present_canvas(Cpu& cpu, void* sdl_window, uint32_t pixels, int width, int height) {
    auto* window = static_cast<SDL_Window*>(sdl_window);
    if (!window || !pixels || width <= 0 || height <= 0) return;
    const size_t byte_count = static_cast<size_t>(width) * static_cast<size_t>(height) * 4;
    if (byte_count > 64 * 1024 * 1024) return;
    std::vector<uint8_t> image(byte_count);
    cpu.read_mem(pixels, image.data(), static_cast<uint32_t>(image.size()));

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

    GLint old_viewport[4], old_buffer = 0, old_active_texture = 0, old_texture = 0, old_program = 0;
    GLfloat old_clear_color[4];
    glGetFloatv(GL_COLOR_CLEAR_VALUE, old_clear_color);
    glGetIntegerv(GL_VIEWPORT, old_viewport);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &old_buffer);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &old_active_texture);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &old_texture);
    if (major >= 2) glGetIntegerv(GL_CURRENT_PROGRAM, &old_program);
    const GLboolean depth = glIsEnabled(GL_DEPTH_TEST), blend = glIsEnabled(GL_BLEND);
    const GLboolean cull = glIsEnabled(GL_CULL_FACE), scissor = glIsEnabled(GL_SCISSOR_TEST);

    if (!texture_id || !glIsTexture(texture_id)) glGenTextures(1, &texture_id);
    glDisable(GL_SCISSOR_TEST);
    glViewport(0, 0, drawable_width, drawable_height);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    const float scale = std::min(
        static_cast<float>(drawable_width) / static_cast<float>(width),
        static_cast<float>(drawable_height) / static_cast<float>(height));
    const int output_width = static_cast<int>(static_cast<float>(width) * scale);
    const int output_height = static_cast<int>(static_cast<float>(height) * scale);
    glViewport((drawable_width - output_width) / 2,
               (drawable_height - output_height) / 2,
               output_width,
               output_height);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture_id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, image.data());

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
        glVertexAttribPointer(position_location, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), vertices);
        glVertexAttribPointer(uv_location, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), vertices + 2);
        glEnableVertexAttribArray(position_location);
        glEnableVertexAttribArray(uv_location);
        glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
        glDisableVertexAttribArray(position_location);
        glDisableVertexAttribArray(uv_location);
        glUseProgram(old_program);
    }
    SDL_GL_SwapWindow(window);
    glBindBuffer(GL_ARRAY_BUFFER, old_buffer);
    glBindTexture(GL_TEXTURE_2D, old_texture);
    glActiveTexture(old_active_texture);
    glClearColor(old_clear_color[0], old_clear_color[1], old_clear_color[2], old_clear_color[3]);
    if (depth) glEnable(GL_DEPTH_TEST);
    if (blend) glEnable(GL_BLEND);
    if (cull) glEnable(GL_CULL_FACE);
    if (scissor) glEnable(GL_SCISSOR_TEST);
    glViewport(old_viewport[0], old_viewport[1], old_viewport[2], old_viewport[3]);
}

}
