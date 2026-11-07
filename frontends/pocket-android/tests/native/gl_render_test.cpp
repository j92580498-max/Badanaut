#define SDL_MAIN_HANDLED
#include <SDL2/SDL.h>
#include <GLES/gl.h>
#include <GLES2/gl2.h>
#include "cpu.h"
#include "hle/gl_api.h"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

namespace {
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

void call(Cpu& cpu, const std::string& symbol, std::initializer_list<uint32_t> args = {}) {
    int index = 0;
    for (uint32_t arg : args) {
        if (index < 4) cpu.set_reg(UC_ARM_REG_R0 + index, arg);
        else cpu.write_u32(cpu.reg(UC_ARM_REG_SP) + (index - 4) * 4, arg);
        ++index;
    }
    hle::call(symbol, cpu);
}

void floats(Cpu& cpu, const std::string& symbol, std::initializer_list<float> args) {
    int index = 0;
    for (float arg : args) {
        if (cpu.hard_float) uc_reg_write(cpu.uc, UC_ARM_REG_S0 + index, &arg);
        else {
            uint32_t bits;
            memcpy(&bits, &arg, sizeof bits);
            if (index < 4) cpu.set_reg(UC_ARM_REG_R0 + index, bits);
            else cpu.write_u32(cpu.reg(UC_ARM_REG_SP) + (index - 4) * 4, bits);
        }
        ++index;
    }
    hle::call(symbol, cpu);
}

uint32_t copy(Cpu& cpu, const void* data, uint32_t bytes) {
    uint32_t out = cpu.alloc(bytes);
    cpu.write_mem(out, data, bytes);
    return out;
}

uint32_t text(Cpu& cpu, const char* value) {
    return copy(cpu, value, strlen(value) + 1);
}

uint32_t shader(Cpu& cpu, GLenum kind, const char* source) {
    call(cpu, "glCreateShader_2", {kind});
    uint32_t object = cpu.reg(UC_ARM_REG_R0);
    require(object != 0, "guest shader creation failed");
    uint32_t string = text(cpu, source);
    call(cpu, "glShaderSource_2", {object, 1, copy(cpu, &string, 4), 0});
    call(cpu, "glCompileShader_2", {object});
    uint32_t status = cpu.alloc(4);
    call(cpu, "glGetShaderiv_2", {object, GL_COMPILE_STATUS, status});
    require(cpu.read_u32(status) == GL_TRUE, "guest shader compilation failed");
    return object;
}

void pixel(int x, int y, std::array<uint8_t, 3> expected) {
    uint8_t value[4] = {};
    glReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, value);
    for (int i = 0; i < 3; ++i) {
        if (abs(int(value[i]) - int(expected[i])) > 4) {
            char message[160];
            snprintf(message, sizeof message, "pixel (%d,%d) is %u,%u,%u; expected %u,%u,%u",
                     x, y, value[0], value[1], value[2], expected[0], expected[1], expected[2]);
            throw std::runtime_error(message);
        }
    }
}

void render(Cpu& cpu, int api) {
    const std::string suffix = "_" + std::to_string(api);
    call(cpu, "glViewport" + suffix, {0, 0, 64, 64});
    floats(cpu, "glClearColor" + suffix, {1, 0, 0, 1});
    call(cpu, "glClear" + suffix, {GL_COLOR_BUFFER_BIT});

    uint32_t out = cpu.alloc(4);
    call(cpu, "glGenTextures" + suffix, {1, out});
    require(cpu.read_u32(out) != 0, "guest texture creation failed");
    call(cpu, "glBindTexture" + suffix, {GL_TEXTURE_2D, cpu.read_u32(out)});
    call(cpu, "glTexParameteri" + suffix, {GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST});
    call(cpu, "glTexParameteri" + suffix, {GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST});
    const uint8_t texels[] = {0,255,0,255, 0,0,255,255, 255,255,0,255, 255,0,255,255};
    call(cpu, "glTexImage2D" + suffix,
         {GL_TEXTURE_2D, 0, GL_RGBA, 2, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, copy(cpu, texels, sizeof texels)});
    require(glGetError() == GL_NO_ERROR, "guest texture upload generated a GL error");
    const uint32_t texture_id = cpu.read_u32(out);
    const std::string fbo_suffix = (api == 1 ? "OES" : "") + suffix;
    call(cpu, "glGenFramebuffers" + fbo_suffix, {1, out});
    const uint32_t framebuffer_id = cpu.read_u32(out);
    call(cpu, "glBindFramebuffer" + fbo_suffix, {GL_FRAMEBUFFER, framebuffer_id});
    call(cpu, "glFramebufferTexture2D" + fbo_suffix,
         {GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture_id, 0});
    call(cpu, "glCheckFramebufferStatus" + fbo_suffix, {GL_FRAMEBUFFER});
    require(cpu.reg(UC_ARM_REG_R0) == GL_FRAMEBUFFER_COMPLETE, "guest framebuffer setup failed");
    pixel(0, 0, {0, 255, 0});
    pixel(1, 1, {255, 0, 255});
    call(cpu, "glBindFramebuffer" + fbo_suffix, {GL_FRAMEBUFFER, 0});

    const float vertices[] = {-0.75f,-0.75f,0,0, 0.75f,-0.75f,1,0,
                              -0.75f,0.75f,0,1, 0.75f,0.75f,1,1};
    uint32_t points = copy(cpu, vertices, sizeof vertices);
    if (api == 1) {
        call(cpu, "glMatrixMode_1", {GL_PROJECTION});
        call(cpu, "glLoadIdentity_1");
        call(cpu, "glMatrixMode_1", {GL_MODELVIEW});
        call(cpu, "glLoadIdentity_1");
        call(cpu, "glEnable_1", {GL_TEXTURE_2D});
        call(cpu, "glColor4ub_1", {255, 255, 255, 255});
        call(cpu, "glEnableClientState_1", {GL_VERTEX_ARRAY});
        call(cpu, "glEnableClientState_1", {GL_TEXTURE_COORD_ARRAY});
        call(cpu, "glVertexPointer_1", {2, GL_FLOAT, 16, points});
        call(cpu, "glTexCoordPointer_1", {2, GL_FLOAT, 16, points + 8});
    } else {
        uint32_t vertex = shader(cpu, GL_VERTEX_SHADER,
            "attribute vec2 position; attribute vec2 uv; varying vec2 v_uv;"
            "void main(){gl_Position=vec4(position,0.,1.);v_uv=uv;}");
        uint32_t fragment = shader(cpu, GL_FRAGMENT_SHADER,
            "precision mediump float; varying vec2 v_uv; uniform sampler2D image;"
            "void main(){gl_FragColor=texture2D(image,v_uv);}");
        call(cpu, "glCreateProgram_2");
        uint32_t program = cpu.reg(UC_ARM_REG_R0);
        call(cpu, "glAttachShader_2", {program, vertex});
        call(cpu, "glAttachShader_2", {program, fragment});
        call(cpu, "glBindAttribLocation_2", {program, 0, text(cpu, "position")});
        call(cpu, "glBindAttribLocation_2", {program, 1, text(cpu, "uv")});
        call(cpu, "glLinkProgram_2", {program});
        call(cpu, "glGetProgramiv_2", {program, GL_LINK_STATUS, out});
        require(cpu.read_u32(out) == GL_TRUE, "guest shader linking failed");
        call(cpu, "glUseProgram_2", {program});
        call(cpu, "glGetUniformLocation_2", {program, text(cpu, "image")});
        call(cpu, "glUniform1i_2", {cpu.reg(UC_ARM_REG_R0), 0});
        call(cpu, "glEnableVertexAttribArray_2", {0});
        call(cpu, "glEnableVertexAttribArray_2", {1});
        call(cpu, "glVertexAttribPointer_2", {0, 2, GL_FLOAT, GL_FALSE, 16, points});
        call(cpu, "glVertexAttribPointer_2", {1, 2, GL_FLOAT, GL_FALSE, 16, points + 8});
    }

    // Check both array and indexed draws, and the real guest->host swap path.
    bool swapped = false;
    hle::gl_set_preswap([&](Cpu&) {
        pixel(2, 2, {255, 0, 0});
        pixel(20, 20, {0, 255, 0});
        pixel(44, 44, {255, 0, 255});
        swapped = true;
    });
    call(cpu, "glDrawArrays" + suffix, {GL_TRIANGLE_STRIP, 0, 4});
    require(glGetError() == GL_NO_ERROR, "guest array draw generated a GL error");
    call(cpu, "sglSwapBuffers", {1, 1});
    require(swapped && cpu.reg(UC_ARM_REG_R0) == 1, "guest swap was not presented");
    const uint16_t indices[] = {0, 1, 2, 2, 1, 3};
    call(cpu, "glClear" + suffix, {GL_COLOR_BUFFER_BIT});
    call(cpu, "glDrawElements" + suffix,
         {GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, copy(cpu, indices, sizeof indices)});
    swapped = false;
    call(cpu, "sglSwapBuffers", {1, 1});
    require(swapped, "indexed draw was not presented");
    hle::gl_set_preswap({});
    require(glGetError() == GL_NO_ERROR, "guest draw generated a GL error");

    // The Canvas path must also work when the game uses either ES context.
    const uint8_t canvas[] = {0,255,255,255};
    hle::gl_present_canvas(cpu, copy(cpu, canvas, sizeof canvas), 1, 1);
    pixel(32, 32, {0, 255, 255});
    require(glGetError() == GL_NO_ERROR, "Canvas generated a GL error");
    call(cpu, "glClear" + suffix, {GL_COLOR_BUFFER_BIT});
    call(cpu, "glDrawArrays" + suffix, {GL_TRIANGLE_STRIP, 0, 4});
    pixel(2, 2, {255, 0, 0});
    pixel(20, 20, {0, 255, 0});
    pixel(44, 44, {255, 0, 255});
    require(glGetError() == GL_NO_ERROR, "guest rendering after Canvas generated a GL error");
}
} // namespace

int main(int argc, char** argv) {
    try {
        require(argc == 3, "expected API version and float ABI");
        int api = std::stoi(argv[1]);
        require(hle::gles_context_version({"glDrawArrays_1"}) == 1, "ES 1 context selection failed");
        require(hle::gles_context_version({"glCreateShader_2"}) == 2, "ES 2 context selection failed");
        require(hle::gles_context_version({}) == 2, "Canvas context selection failed");
        SDL_SetMainReady();
        require(SDL_Init(SDL_INIT_VIDEO) == 0, SDL_GetError());
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, api);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
        SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
        SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
        SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
        SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);
        SDL_Window* window = SDL_CreateWindow("Bada GL test", 0, 0, 64, 64, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
        require(window != nullptr, SDL_GetError());
        auto context = SDL_GL_CreateContext(window);
        require(context != nullptr, SDL_GetError());
        printf("Host GL: %s, %s\n", glGetString(GL_VERSION), glGetString(GL_RENDERER));
        hle::gl_set_window(window);
        ElfImage image;
        const uint32_t instruction = 0xe12fff1e; // bx lr; no game or firmware input
        image.segments.push_back({0x100000, 4, 4096, 5, (const uint8_t*)&instruction});
        Cpu cpu;
        std::string error;
        const bool initialized = cpu.init(image, error);
        require(initialized, error.c_str());
        // Cpu's idle SP leaves 16 bytes; texture calls pass five stack args.
        cpu.set_reg(UC_ARM_REG_SP, cpu.reg(UC_ARM_REG_SP) - 256);
        cpu.hard_float = std::string(argv[2]) == "hard";
        render(cpu, api);
        uc_close(cpu.uc);
        SDL_GL_DeleteContext(context);
        SDL_DestroyWindow(window);
        SDL_Quit();
        printf("ES %d %s: guest textures, arrays, indices, swap and Canvas passed\n", api, argv[2]);
        return 0;
    } catch (const std::exception& error) {
        fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
