#include "cpu.h"
#include "hle/dispatch.h"
#include <GLES2/gl2.h>
#include <SDL2/SDL.h>
#include <algorithm>
#include <cstdint>
#include <vector>

namespace {
SDL_Window* window = nullptr;
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

void register_gl() {}

void gl_set_window(void* sdl_window) {
    window = static_cast<SDL_Window*>(sdl_window);
}

void gl_set_preswap(std::function<void(Cpu&)>) {}

void gl_present_canvas(Cpu& cpu, uint32_t pixels, int width, int height) {
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
    if (drawable_width <= 0 || drawable_height <= 0 || !ensure_program()) return;

    if (!texture_id || !glIsTexture(texture_id)) glGenTextures(1, &texture_id);
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
    glUseProgram(program_id);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture_id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, image.data());
    glUniform1i(sampler_location, 0);

    const GLfloat vertices[] = {
        -1.0f,  1.0f, 0.0f, 0.0f,
         1.0f,  1.0f, 1.0f, 0.0f,
         1.0f, -1.0f, 1.0f, 1.0f,
        -1.0f, -1.0f, 0.0f, 1.0f,
    };
    glVertexAttribPointer(position_location, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), vertices);
    glVertexAttribPointer(uv_location, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), vertices + 2);
    glEnableVertexAttribArray(position_location);
    glEnableVertexAttribArray(uv_location);
    glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
    glDisableVertexAttribArray(position_location);
    glDisableVertexAttribArray(uv_location);
    SDL_GL_SwapWindow(window);
}

}
