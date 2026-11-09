#define SDL_MAIN_HANDLED
#include <SDL2/SDL.h>
#ifdef BADA_TEST_DESKTOP
#include <SDL2/SDL_opengl.h>
#else
#include <GLES/gl.h>
#include <GLES2/gl2.h>
#endif
#include "cpu.h"
#include "hle/gl_api.h"
#include "display.h"
#include "pixel_accel.h"
#include "arm_pixel_fixture.h"
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <vector>

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

void compressed_texture_test(Cpu& cpu, int api) {
    const std::string suffix = "_" + std::to_string(api);
    const std::string fbo_suffix = (api == 1 ? "OES" : "") + suffix;
    GLint previous_texture = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &previous_texture);
    const uint32_t out = cpu.alloc(4);
    call(cpu, "glGenTextures" + suffix, {1, out});
    const uint32_t texture = cpu.read_u32(out);
    call(cpu, "glBindTexture" + suffix, {GL_TEXTURE_2D, texture});
    call(cpu, "glTexParameteri" + suffix, {GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST});
    call(cpu, "glGenFramebuffers" + fbo_suffix, {1, out});
    const uint32_t framebuffer = cpu.read_u32(out);
    call(cpu, "glBindFramebuffer" + fbo_suffix, {GL_FRAMEBUFFER, framebuffer});

    // Four PVRTC words with identical white endpoints, using the upstream
    // PowerVR decoder's opaque color encoding. Both 2/4 bpp occupy at least
    // 32 bytes, even for the tiny mip levels Nova uploads.
    const uint32_t white[] = {0,0xffffffff, 0,0xffffffff, 0,0xffffffff, 0,0xffffffff};
    const uint32_t empty[8] = {};
    const uint32_t white_data = copy(cpu, white, sizeof white);
    const uint32_t empty_data = copy(cpu, empty, sizeof empty);
    struct Probe { GLenum format; uint32_t width, height, data; bool opaque; };
    const Probe probes[] = {
        {0x8C01, 16, 8, white_data, true}, // RGB PVRTC2
        {0x8C02, 8, 8, white_data, true}, // RGBA PVRTC4
        {0x8C03, 1, 1, empty_data, false}, // RGBA PVRTC2 tiny mip
        {0x8C00, 1, 1, empty_data, true}, // RGB ignores encoded alpha
    };
    for (const auto& probe : probes) {
        call(cpu, "glCompressedTexImage2D" + suffix,
             {GL_TEXTURE_2D, 0, probe.format, probe.width, probe.height, 0, 32, probe.data});
        require(glGetError() == GL_NO_ERROR, "PVRTC upload generated a GL error");
        call(cpu, "glFramebufferTexture2D" + fbo_suffix,
             {GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0});
        call(cpu, "glCheckFramebufferStatus" + fbo_suffix, {GL_FRAMEBUFFER});
        require(cpu.reg(UC_ARM_REG_R0) == GL_FRAMEBUFFER_COMPLETE,
                "PVRTC texture did not produce a decoded image");
        const uint8_t expected = probe.data == white_data ? 255 : 0;
        pixel(probe.width - 1, probe.height - 1, {expected, expected, expected});
        uint8_t rgba[4] = {};
        glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
        require(rgba[3] == (probe.opaque ? 255 : 0), "PVRTC RGB/RGBA alpha semantics changed");
    }
    call(cpu, "glBindFramebuffer" + fbo_suffix, {GL_FRAMEBUFFER, 0});
    cpu.write_u32(out, framebuffer);
    call(cpu, "glDeleteFramebuffers" + fbo_suffix, {1, out});
    call(cpu, "glBindTexture" + suffix, {GL_TEXTURE_2D, static_cast<uint32_t>(previous_texture)});
    cpu.write_u32(out, texture);
    call(cpu, "glDeleteTextures" + suffix, {1, out});
    require(glGetError() == GL_NO_ERROR, "compressed texture probe left a GL error");
}

void render(Cpu& cpu, int api) {
    const std::string suffix = "_" + std::to_string(api);
    call(cpu, "glViewport" + suffix, {0, 0, 64, 64});
    floats(cpu, "glClearColor" + suffix, {1, 0, 0, 1});
    call(cpu, "glClear" + suffix, {GL_COLOR_BUFFER_BIT});

    uint32_t out = cpu.alloc(4);
    // Nova changes color write masks while clearing/drawing individual passes.
    // The query returns four bytes, not four guest integers.
    call(cpu, "glColorMask" + suffix, {0, 1, 0, 0});
    floats(cpu, "glClearColor" + suffix, {0, 1, 1, 0});
    call(cpu, "glClear" + suffix, {GL_COLOR_BUFFER_BIT});
    pixel(2, 2, {255, 255, 0});
    call(cpu, "glGetBooleanv" + suffix, {GL_COLOR_WRITEMASK, out});
    require(cpu.read_u32(out) == 0x00000100, "guest color-mask query used the wrong byte layout");
    call(cpu, "glColorMask" + suffix, {1, 1, 1, 1});
    floats(cpu, "glClearColor" + suffix, {1, 0, 0, 1});
    call(cpu, "glClear" + suffix, {GL_COLOR_BUFFER_BIT});
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

    compressed_texture_test(cpu, api);

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
        const float replace = GL_REPLACE;
        uint32_t replace_bits;
        memcpy(&replace_bits, &replace, sizeof replace_bits);
        if (cpu.hard_float) uc_reg_write(cpu.uc, UC_ARM_REG_S0, &replace);
        call(cpu, "glTexEnvf_1", {GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, replace_bits});
        GLint texture_mode = 0;
        glGetTexEnviv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, &texture_mode);
        require(texture_mode == GL_REPLACE, "mixed integer/float glTexEnvf ABI failed");
        call(cpu, "glTexEnvi_1", {GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE});
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

    // Nova's Irrlicht renderer uploads BGRA8888 images. All four channels and
    // both rows must reach the host; treating BGRA as one byte per pixel leaves
    // the driver reading past the staging buffer and corrupts fonts/sprites.
    constexpr GLenum bgra = 0x80E1; // GL_BGRA_EXT (also supported by desktop GL)
    const uint8_t bgra_image[] = {0,0,255,255, 0,255,255,255,
                                  255,0,255,255, 255,0,0,255};
    call(cpu, "glTexImage2D" + suffix,
         {GL_TEXTURE_2D, 0, GL_RGBA, 2, 2, 0, bgra, GL_UNSIGNED_BYTE,
          copy(cpu, bgra_image, sizeof bgra_image)});
    require(glGetError() == GL_NO_ERROR, "BGRA image upload generated a GL error");
    call(cpu, "glDrawArrays" + suffix, {GL_TRIANGLE_STRIP, 0, 4});
    pixel(20, 20, {255, 0, 0});
    pixel(44, 44, {0, 0, 255});
    const uint8_t bgra_update[] = {0,255,0,255, 255,0,0,255,
                                   0,255,255,255, 255,0,255,255};
    call(cpu, "glTexSubImage2D" + suffix,
         {GL_TEXTURE_2D, 0, 0, 0, 2, 2, bgra, GL_UNSIGNED_BYTE,
          copy(cpu, bgra_update, sizeof bgra_update)});
    require(glGetError() == GL_NO_ERROR, "BGRA subimage upload generated a GL error");
    call(cpu, "glDrawArrays" + suffix, {GL_TRIANGLE_STRIP, 0, 4});
    pixel(20, 20, {0, 255, 0});
    pixel(44, 44, {255, 0, 255});
    // BGRA is texture-filterable in ES 1; renderability is only guaranteed
    // from ES 2. Test it on textured quads, then restore the RGBA fixture.
    call(cpu, "glTexImage2D" + suffix,
         {GL_TEXTURE_2D, 0, GL_RGBA, 2, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, copy(cpu, texels, sizeof texels)});
    call(cpu, "glClear" + suffix, {GL_COLOR_BUFFER_BIT});

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
    std::vector<uint32_t> canvas(64 * 64, 0x0000ffff); // opaque bada cyan in 0x00RRGGBB
    hle::gl_present_canvas(cpu, copy(cpu, canvas.data(), canvas.size() * 4), 64, 64);
    pixel(32, 32, {0, 255, 255});
    std::fill(canvas.begin(), canvas.end(), 0x00ff0000);
    hle::gl_present_canvas(cpu, copy(cpu, canvas.data(), canvas.size() * 4), 64, 64);
    pixel(32, 32, {255, 0, 0});
    std::fill(canvas.begin(), canvas.end(), 0x000000ff);
    hle::gl_present_canvas(cpu, copy(cpu, canvas.data(), canvas.size() * 4), 64, 64);
    pixel(32, 32, {0, 0, 255}); // also exercises reuse of the Canvas texture
    require(glGetError() == GL_NO_ERROR, "Canvas generated a GL error");
    call(cpu, "glClear" + suffix, {GL_COLOR_BUFFER_BIT});
    call(cpu, "glDrawArrays" + suffix, {GL_TRIANGLE_STRIP, 0, 4});
    pixel(2, 2, {255, 0, 0});
    pixel(20, 20, {0, 255, 0});
    pixel(44, 44, {255, 0, 255});
    require(glGetError() == GL_NO_ERROR, "guest rendering after Canvas generated a GL error");

    // Dynamic sprites interleave positions, UVs and byte colors. Their next
    // draw must see guest updates even when upload staging/storage is reused.
    const uint32_t interleaved = cpu.alloc(80);
    for (int vertex = 0; vertex < 4; ++vertex) {
        cpu.write_mem(interleaved + vertex * 20, vertices + vertex * 4, 16);
        cpu.write_u32(interleaved + vertex * 20 + 16, 0xffffffff);
    }
    if (api == 1) {
        call(cpu, "glVertexPointer_1", {2, GL_FLOAT, 20, interleaved});
        call(cpu, "glTexCoordPointer_1", {2, GL_FLOAT, 20, interleaved + 8});
        call(cpu, "glColorPointer_1", {4, GL_UNSIGNED_BYTE, 20, interleaved + 16});
        call(cpu, "glEnableClientState_1", {GL_COLOR_ARRAY});
    } else {
        call(cpu, "glVertexAttribPointer_2", {0, 2, GL_FLOAT, GL_FALSE, 20, interleaved});
        call(cpu, "glVertexAttribPointer_2", {1, 2, GL_FLOAT, GL_FALSE, 20, interleaved + 8});
    }
    call(cpu, "glClear" + suffix, {GL_COLOR_BUFFER_BIT});
    call(cpu, "glDrawArrays" + suffix, {GL_TRIANGLE_STRIP, 0, 4});
    pixel(20, 20, {0, 255, 0});
    pixel(44, 44, {255, 0, 255});
    for (int vertex = 0; vertex < 4; ++vertex) {
        const float uv[] = {1 - vertices[vertex * 4 + 2], 1 - vertices[vertex * 4 + 3]};
        cpu.write_mem(interleaved + vertex * 20 + 8, uv, sizeof uv);
    }
    call(cpu, "glClear" + suffix, {GL_COLOR_BUFFER_BIT});
    call(cpu, "glDrawArrays" + suffix, {GL_TRIANGLE_STRIP, 0, 4});
    pixel(20, 20, {255, 0, 255});
    pixel(44, 44, {0, 255, 0});
    if (api == 1) {
        call(cpu, "glDisableClientState_1", {GL_COLOR_ARRAY});
        call(cpu, "glVertexPointer_1", {2, GL_FLOAT, 16, points});
        call(cpu, "glTexCoordPointer_1", {2, GL_FLOAT, 16, points + 8});
    } else {
        call(cpu, "glVertexAttribPointer_2", {0, 2, GL_FLOAT, GL_FALSE, 16, points});
        call(cpu, "glVertexAttribPointer_2", {1, 2, GL_FLOAT, GL_FALSE, 16, points + 8});
    }
    require(glGetError() == GL_NO_ERROR, "dynamic interleaved uploads generated a GL error");

    if (api == 1) {
        // Nova saves/restores array pointers around texture uploads. Queries
        // must retain guest addresses and bindings after host VBO staging.
        call(cpu, "glGetPointerv_1", {GL_VERTEX_ARRAY_POINTER, out});
        require(cpu.read_u32(out) == points, "vertex pointer query leaked a host staging offset");
        call(cpu, "glGetIntegerv_1", {GL_VERTEX_ARRAY_BUFFER_BINDING, out});
        require(cpu.read_u32(out) == 0, "client array query leaked an internal host VBO");
        call(cpu, "glIsEnabled_1", {GL_VERTEX_ARRAY});
        require(cpu.reg(UC_ARM_REG_R0) == GL_TRUE, "deferred vertex array appears disabled to the guest");

        // Each client texture unit owns its UV pointer. Using opposite corners
        // of the same texture on two modulated units makes the quad black.
        const float reversed_uvs[] = {1,1, 0,1, 1,0, 0,0};
        const uint32_t second_uvs = copy(cpu, reversed_uvs, sizeof reversed_uvs);
        call(cpu, "glActiveTexture_1", {GL_TEXTURE1});
        call(cpu, "glBindTexture_1", {GL_TEXTURE_2D, texture_id});
        call(cpu, "glTexEnvi_1", {GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE});
        call(cpu, "glEnable_1", {GL_TEXTURE_2D});
        call(cpu, "glClientActiveTexture_1", {GL_TEXTURE1});
        call(cpu, "glTexCoordPointer_1", {2, GL_FLOAT, 0, second_uvs});
        call(cpu, "glEnableClientState_1", {GL_TEXTURE_COORD_ARRAY});
        call(cpu, "glClientActiveTexture_1", {GL_TEXTURE0});
        call(cpu, "glGetPointerv_1", {GL_TEXTURE_COORD_ARRAY_POINTER, out});
        require(cpu.read_u32(out) == points + 8, "second texture unit overwrote the first UV pointer");
        floats(cpu, "glColor4f_1", {1, 1, 1, 1});
        call(cpu, "glClear_1", {GL_COLOR_BUFFER_BIT});
        call(cpu, "glDrawArrays_1", {GL_TRIANGLE_STRIP, 0, 4});
        pixel(20, 20, {0, 0, 0});
        pixel(44, 44, {0, 0, 0});
        call(cpu, "glGetIntegerv_1", {GL_CLIENT_ACTIVE_TEXTURE, out});
        require(cpu.read_u32(out) == GL_TEXTURE0, "array staging changed the guest client texture unit");
        call(cpu, "glClientActiveTexture_1", {GL_TEXTURE1});
        call(cpu, "glGetPointerv_1", {GL_TEXTURE_COORD_ARRAY_POINTER, out});
        require(cpu.read_u32(out) == second_uvs, "second UV pointer was not retained after drawing");
        call(cpu, "glDisableClientState_1", {GL_TEXTURE_COORD_ARRAY});
        call(cpu, "glDisable_1", {GL_TEXTURE_2D});
        call(cpu, "glActiveTexture_1", {GL_TEXTURE0});
        call(cpu, "glClientActiveTexture_1", {GL_TEXTURE0});

        // The mixed GLenum/float alpha-test ABI must discard transparent font
        // texels rather than covering previously rendered pixels.
        const float reference = 0.5f;
        uint32_t reference_bits;
        memcpy(&reference_bits, &reference, sizeof reference_bits);
        if (cpu.hard_float) uc_reg_write(cpu.uc, UC_ARM_REG_S0, &reference);
        call(cpu, "glAlphaFunc_1", {GL_GREATER, reference_bits});
        const uint8_t transparent[] = {0, 255, 0, 0};
        call(cpu, "glTexSubImage2D_1",
             {GL_TEXTURE_2D, 0, 0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, copy(cpu, transparent, 4)});
        call(cpu, "glEnable_1", {GL_ALPHA_TEST});
        call(cpu, "glClear_1", {GL_COLOR_BUFFER_BIT});
        call(cpu, "glDrawArrays_1", {GL_TRIANGLE_STRIP, 0, 4});
        pixel(20, 20, {255, 0, 0});
        pixel(44, 44, {255, 0, 255});
        call(cpu, "glDisable_1", {GL_ALPHA_TEST});
        call(cpu, "glTexSubImage2D_1",
             {GL_TEXTURE_2D, 0, 0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, copy(cpu, texels, 4)});

        const uint32_t matrix = cpu.alloc(80);
        cpu.write_u32(matrix + 64, 0xdeadbeef);
        call(cpu, "glGetFloatv_1", {GL_PROJECTION_MATRIX, matrix});
        for (int i = 0; i < 16; ++i)
            require(cpu.read_u32(matrix + i * 4) == (i % 5 == 0 ? 0x3f800000u : 0u),
                    "matrix query omitted projection values");
        require(cpu.read_u32(matrix + 64) == 0xdeadbeef, "matrix query overran the guest buffer");

        const float density = 0.375f;
        uint32_t density_bits;
        memcpy(&density_bits, &density, sizeof density_bits);
        if (cpu.hard_float) uc_reg_write(cpu.uc, UC_ARM_REG_S0, &density);
        call(cpu, "glFogf_1", {GL_FOG_DENSITY, density_bits});
        GLfloat actual_density = 0;
        glGetFloatv(GL_FOG_DENSITY, &actual_density);
        require(std::fabs(actual_density - density) < 0.001f, "mixed GLenum/float fog ABI failed");
        call(cpu, "glClear_1", {GL_COLOR_BUFFER_BIT});
        call(cpu, "glDrawArrays_1", {GL_TRIANGLE_STRIP, 0, 4});
        pixel(20, 20, {0, 255, 0});
        pixel(44, 44, {255, 0, 255});
        require(glGetError() == GL_NO_ERROR, "Nova graphics-state regression generated a GL error");
    }
}

void format_query_test(Cpu& cpu, int api) {
    GLint count = 0;
    glGetIntegerv(GL_NUM_COMPRESSED_TEXTURE_FORMATS, &count);
    require(count >= 0, "host reported a negative texture-format count");
    std::vector<GLint> expected(count);
    glGetIntegerv(GL_COMPRESSED_TEXTURE_FORMATS, expected.data());
    const uint32_t output = cpu.alloc(count * 4 + 8) + 4;
    cpu.write_u32(output - 4, 0xdeadbeef);
    cpu.write_u32(output + count * 4, 0xdeadbeef);
    call(cpu, "glGetIntegerv_" + std::to_string(api), {GL_COMPRESSED_TEXTURE_FORMATS, output});
    for (int i = 0; i < count; ++i)
        require(cpu.read_u32(output + i * 4) == static_cast<uint32_t>(expected[i]),
                "compressed-format integer query omitted list entries");
    require(cpu.read_u32(output - 4) == 0xdeadbeef && cpu.read_u32(output + count * 4) == 0xdeadbeef,
            "compressed-format query overran its guest buffer");
    cpu.write_u32(output + count, 0xdeadbeef);
    call(cpu, "glGetBooleanv_" + std::to_string(api), {GL_COMPRESSED_TEXTURE_FORMATS, output});
    std::vector<GLboolean> booleans(count);
    cpu.read_mem(output, booleans.data(), count);
    for (int i = 0; i < count; ++i)
        require(booleans[i] == (expected[i] != 0), "boolean texture-format query truncated its list");
    require(cpu.read_u32(output + count) == 0xdeadbeef, "boolean texture-format query overran its guest buffer");
    if (api == 1) {
        cpu.write_u32(output + count * 4, 0xdeadbeef);
        call(cpu, "glGetFloatv_1", {GL_COMPRESSED_TEXTURE_FORMATS, output});
        std::vector<GLfloat> values(count);
        cpu.read_mem(output, values.data(), count * sizeof(GLfloat));
        for (int i = 0; i < count; ++i)
            require(values[i] == static_cast<GLfloat>(expected[i]), "float texture-format query truncated its list");
        require(cpu.read_u32(output + count * 4) == 0xdeadbeef, "float texture-format query overran its guest buffer");
    }
    require(glGetError() == GL_NO_ERROR, "texture-format queries generated a GL error");
}

void resize_test(Cpu& cpu, int api, SDL_Window* window) {
    const std::string suffix = "_" + std::to_string(api);
    // Rebind the same context to a larger drawable. This also works with SDL's
    // offscreen driver, whose pbuffer cannot be resized in place.
    const auto context = SDL_GL_GetCurrentContext();
    auto* resized = SDL_CreateWindow("Resized Bada surface", 0, 0, 160, 96, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    require(resized != nullptr, SDL_GetError());
    require(SDL_GL_MakeCurrent(resized, context) == 0, SDL_GetError());
    hle::gl_set_window(resized);
    hle::gl_resize();
    call(cpu, "glViewport" + suffix, {0, 0, 64, 64});
    floats(cpu, "glClearColor" + suffix, {0, 0, 0, 1});
    call(cpu, "glClear" + suffix, {GL_COLOR_BUFFER_BIT});
    call(cpu, "glDrawArrays" + suffix, {GL_TRIANGLE_STRIP, 0, 4});
    pixel(10, 48, {0, 0, 0}); // letterbox
    pixel(62, 30, {0, 255, 0});
    pixel(102, 66, {255, 0, 255});
    const uint32_t state = cpu.alloc(16);
    call(cpu, "glGetIntegerv" + suffix, {GL_VIEWPORT, state});
    require(cpu.read_u32(state + 8) == 64 && cpu.read_u32(state + 12) == 64,
            "guest viewport query leaked native display dimensions");
    if (api == 1) {
        call(cpu, "glGetFloatv_1", {GL_VIEWPORT, state});
        require(cpu.read_u32(state) == 0 && cpu.read_u32(state + 4) == 0 &&
                cpu.read_u32(state + 8) == 0x42800000u && cpu.read_u32(state + 12) == 0x42800000u,
                "float viewport query leaked the scaled host rectangle");
        call(cpu, "glGetBooleanv_1", {GL_VIEWPORT, state});
        require(cpu.read_u32(state) == 0x01010000u,
                "boolean viewport query exposed the host letterbox origin");
    }
    int x = -1, y = -1;
    hle::gl_map_touch(80, 48, 160, 96, x, y);
    require(x == 32 && y == 32, "fullscreen input missed the center");
    hle::gl_map_touch(10, 48, 160, 96, x, y);
    require(x == 0 && y == 32, "letterbox input was not clamped to the guest");
    call(cpu, "glScissor" + suffix, {0, 0, 32, 64});
    call(cpu, "glEnable" + suffix, {GL_SCISSOR_TEST});
    floats(cpu, "glClearColor" + suffix, {0, 0, 1, 1});
    call(cpu, "glClear" + suffix, {GL_COLOR_BUFFER_BIT});
    pixel(48, 48, {0, 0, 255});
    pixel(120, 48, {0, 0, 0});
    call(cpu, "glGetIntegerv" + suffix, {GL_SCISSOR_BOX, state});
    require(cpu.read_u32(state + 8) == 32 && cpu.read_u32(state + 12) == 64,
            "guest scissor query leaked native display dimensions");
    call(cpu, "glDisable" + suffix, {GL_SCISSOR_TEST});

    // Offscreen textures keep guest pixels even when the Android surface is
    // larger; scaling these viewports/scissors breaks Fruit Ninja's FBOs.
    const std::string fbo_suffix = (api == 1 ? "OES" : "") + suffix;
    call(cpu, "glGenTextures" + suffix, {1, state});
    call(cpu, "glBindTexture" + suffix, {GL_TEXTURE_2D, cpu.read_u32(state)});
    call(cpu, "glTexImage2D" + suffix, {GL_TEXTURE_2D, 0, GL_RGBA, 2, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0});
    const uint32_t texture_id = cpu.read_u32(state);
    call(cpu, "glGenFramebuffers" + fbo_suffix, {1, state});
    call(cpu, "glBindFramebuffer" + fbo_suffix, {GL_FRAMEBUFFER, cpu.read_u32(state)});
    call(cpu, "glFramebufferTexture2D" + fbo_suffix, {GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture_id, 0});
    call(cpu, "glViewport" + suffix, {0, 0, 2, 2});
    GLint viewport[4] = {};
    glGetIntegerv(GL_VIEWPORT, viewport);
    require(viewport[0] == 0 && viewport[1] == 0 && viewport[2] == 2 && viewport[3] == 2,
            "offscreen viewport was scaled to the native surface");
    floats(cpu, "glClearColor" + suffix, {0, 0, 0, 1});
    call(cpu, "glClear" + suffix, {GL_COLOR_BUFFER_BIT});
    call(cpu, "glScissor" + suffix, {0, 0, 1, 2});
    call(cpu, "glEnable" + suffix, {GL_SCISSOR_TEST});
    floats(cpu, "glClearColor" + suffix, {1, 0, 0, 1});
    call(cpu, "glClear" + suffix, {GL_COLOR_BUFFER_BIT});
    pixel(0, 0, {255, 0, 0});
    pixel(1, 0, {0, 0, 0});
    call(cpu, "glDisable" + suffix, {GL_SCISSOR_TEST});
    call(cpu, "glDeleteFramebuffers" + fbo_suffix, {1, state});
    GLint binding = -1;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &binding);
    glGetIntegerv(GL_VIEWPORT, viewport);
    require(binding == 0 && viewport[0] == 32 && viewport[1] == 0 &&
            viewport[2] == 3 && viewport[3] == 3,
            "deleting the bound framebuffer did not restore surface scaling");
    require(glGetError() == GL_NO_ERROR, "fullscreen/FBO rendering generated a GL error");
    require(SDL_GL_MakeCurrent(window, context) == 0, SDL_GetError());
    hle::gl_set_window(window);
    hle::gl_resize();
    SDL_DestroyWindow(resized);
}

void rotated_window_test(Cpu& cpu, SDL_Window* window) {
    const auto context = SDL_GL_GetCurrentContext();
    auto* target = SDL_CreateWindow("Landscape SDK window", 0, 0, 128, 64, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    require(target != nullptr, SDL_GetError());
    require(SDL_GL_MakeCurrent(target, context) == 0, SDL_GetError());
    hle::gl_set_window(target);
    hle::gl_set_surface_size(64, 128, "test portrait framebuffer");
    hle::gl_set_window_orientation(true);
    call(cpu, "glViewport_1", {0, 0, 64, 128});
    call(cpu, "glMatrixMode_1", {GL_PROJECTION});
    call(cpu, "glLoadIdentity_1");
    call(cpu, "glPushMatrix_1");
    floats(cpu, "glScalef_1", {0.5f, 0.5f, 1});
    call(cpu, "glPopMatrix_1");
    call(cpu, "glMatrixMode_1", {GL_MODELVIEW});
    call(cpu, "glLoadIdentity_1");
    const uint32_t state = cpu.alloc(64);
    call(cpu, "glGenTextures_1", {1, state});
    const uint32_t texture = cpu.read_u32(state);
    call(cpu, "glBindTexture_1", {GL_TEXTURE_2D, texture});
    call(cpu, "glTexParameteri_1", {GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST});
    call(cpu, "glTexParameteri_1", {GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST});
    const uint8_t texels[] = {0,255,0,255, 0,0,255,255, 255,255,0,255, 255,0,255,255};
    call(cpu, "glTexImage2D_1", {GL_TEXTURE_2D, 0, GL_RGBA, 2, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                                copy(cpu, texels, sizeof texels)});
    const float vertices[] = {-1,-1,0,0, 1,-1,1,0, -1,1,0,1, 1,1,1,1};
    const uint32_t points = copy(cpu, vertices, sizeof vertices);
    call(cpu, "glVertexPointer_1", {2, GL_FLOAT, 16, points});
    call(cpu, "glTexCoordPointer_1", {2, GL_FLOAT, 16, points + 8});
    floats(cpu, "glColor4f_1", {1, 1, 1, 1});
    call(cpu, "glDrawArrays_1", {GL_TRIANGLE_STRIP, 0, 4});
    pixel(16, 8, {255, 255, 0}); pixel(112, 8, {0, 255, 0});
    pixel(16, 56, {255, 0, 255}); pixel(112, 56, {0, 0, 255});
    // Indexed draws must receive the same correction.
    const uint8_t indices[] = {0, 1, 2, 3};
    call(cpu, "glDrawElements_1", {GL_TRIANGLE_STRIP, 4, GL_UNSIGNED_BYTE, copy(cpu, indices, sizeof indices)});
    pixel(16, 56, {255, 0, 255});
    call(cpu, "glGetFloatv_1", {GL_PROJECTION_MATRIX, state});
    for (int i = 0; i < 16; ++i)
        require(cpu.read_u32(state + i * 4) == (i % 5 == 0 ? 0x3f800000u : 0u),
                "presentation rotation changed the guest projection or matrix stack");
    GLint mode = 0; glGetIntegerv(GL_MATRIX_MODE, &mode);
    require(mode == GL_MODELVIEW, "presentation rotation changed the matrix mode");
    int x = 0, y = 0;
    hle::gl_map_touch(0, 0, 128, 64, x, y);
    require(x == 63 && y == 0, "rotated top-left touch missed the portrait framebuffer");
    hle::gl_map_touch(127, 63, 128, 64, x, y);
    require(x == 0 && y == 127, "rotated bottom-right touch missed the portrait framebuffer");
    call(cpu, "glScissor_1", {0, 0, 32, 128});
    call(cpu, "glEnable_1", {GL_SCISSOR_TEST});
    floats(cpu, "glClearColor_1", {1, 0, 0, 1});
    call(cpu, "glClear_1", {GL_COLOR_BUFFER_BIT});
    pixel(112, 8, {255, 0, 0}); pixel(112, 56, {0, 0, 255});
    call(cpu, "glDisable_1", {GL_SCISSOR_TEST});
    call(cpu, "glViewport_1", {0, 0, 32, 128});
    GLint viewport[4]{}; glGetIntegerv(GL_VIEWPORT, viewport);
    require(viewport[0] == 0 && viewport[1] == 0 && viewport[2] == 128 && viewport[3] == 32,
            "sub-viewport did not follow the presentation rotation");
    // Drawing to a guest FBO retains the original projection and pixel order.
    call(cpu, "glGenTextures_1", {1, state});
    const uint32_t offscreen_texture = cpu.read_u32(state);
    call(cpu, "glBindTexture_1", {GL_TEXTURE_2D, offscreen_texture});
    call(cpu, "glTexImage2D_1", {GL_TEXTURE_2D, 0, GL_RGBA, 2, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0});
    call(cpu, "glGenFramebuffersOES_1", {1, state});
    const uint32_t framebuffer = cpu.read_u32(state);
    call(cpu, "glBindFramebufferOES_1", {GL_FRAMEBUFFER, framebuffer});
    call(cpu, "glFramebufferTexture2DOES_1", {GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, offscreen_texture, 0});
    call(cpu, "glBindTexture_1", {GL_TEXTURE_2D, texture});
    call(cpu, "glViewport_1", {0, 0, 2, 2});
    call(cpu, "glDrawArrays_1", {GL_TRIANGLE_STRIP, 0, 4});
    pixel(0, 0, {0, 255, 0}); pixel(1, 1, {255, 0, 255});
    cpu.write_u32(state, framebuffer);
    call(cpu, "glDeleteFramebuffersOES_1", {1, state});
    require(glGetError() == GL_NO_ERROR, "rotated window/FBO rendering generated a GL error");
    hle::gl_set_window_orientation(false);
    hle::gl_set_surface_size(64, 64, "test reset");
    require(SDL_GL_MakeCurrent(window, context) == 0, SDL_GetError());
    hle::gl_set_window(window);
    SDL_DestroyWindow(target);
}

#ifndef BADA_TEST_DESKTOP
void compact_canvas_test(Cpu& cpu, SDL_Window* window) {
    const uint32_t kernel = 0x100100;
    cpu.write_mem(kernel, arm_pixel_fixture, sizeof arm_pixel_fixture);
    ElfImage fixture_image;
    fixture_image.segments.push_back({kernel, sizeof arm_pixel_fixture, sizeof arm_pixel_fixture, 5,
                                      reinterpret_cast<const uint8_t*>(arm_pixel_fixture)});
    require(pixel_accel::install(cpu, fixture_image) == 1, "generated pixel kernel was not recognized");
    std::vector<uint16_t> source(320 * 256, 0xdead);
    for (int y = 0; y < 320; ++y)
        for (int x = 0; x < 240; ++x) source[y * 256 + x] = (y * 240 + x) & 0xffff;
    const uint32_t input = copy(cpu, source.data(), source.size() * 2), output = cpu.alloc(480 * 640 * 4);
    const auto context = SDL_GL_GetCurrentContext();
    for (const auto size : {hle::SurfaceRect{0,0,480,640}, hle::SurfaceRect{0,0,160,96}}) {
        auto* target = SDL_CreateWindow("Compact Canvas", 0, 0, size.width, size.height, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
        require(target != nullptr, SDL_GetError());
        require(SDL_GL_MakeCurrent(target, context) == 0, SDL_GetError());
        hle::gl_set_window(target);
        for (int mode = 0; mode < 3; ++mode) {
            uint32_t result = 0;
            require(cpu.run_guest(kernel, {input, output, 320}, result), "pixel kernel call failed");
            std::vector<uint32_t> expected(480 * 640);
            cpu.read_mem(output, expected.data(), expected.size() * 4);
            if (mode == 1) { // arbitrary 8-bit HUD colors over the RGB565 background
                for (int y = 9; y < 46; ++y)
                    for (int x = 7; x < 107; ++x) expected[y * 480 + x] = 0x00123456;
                cpu.write_mem(output, expected.data(), expected.size() * 4);
            } else if (mode == 2) { // extensive guest changes must use full RGBA upload
                std::fill(expected.begin(), expected.end(), 0x0000ff00);
                cpu.write_mem(output, expected.data(), expected.size() * 4);
            }
            hle::gl_present_canvas(cpu, output, 480, 640);
            const auto upload = hle::gl_canvas_upload_info();
            require(mode == 2 ? !upload.compact && upload.bytes == expected.size() * 4
                              : upload.compact && upload.bytes < expected.size(),
                    "compact Canvas did not reduce texture upload bytes or retain its fallback");
            std::vector<uint8_t> rendered(size.width * size.height * 4);
            glReadPixels(0, 0, size.width, size.height, GL_RGBA, GL_UNSIGNED_BYTE, rendered.data());
            const auto area = hle::fit_surface(480, 640, size.width, size.height);
            for (int y = 0; y < size.height; ++y) {
                for (int x = 0; x < size.width; ++x) {
                    uint32_t rgb = 0;
                    if (x >= area.x && x < area.x + area.width && y >= area.y && y < area.y + area.height) {
                        const int gx = (2 * (x - area.x) + 1) * 480 / (2 * area.width);
                        const int gy = (2 * (area.height - (y - area.y)) - 1) * 640 / (2 * area.height);
                        rgb = expected[gy * 480 + gx];
                    }
                    const size_t index = (y * size.width + x) * 4;
                    for (int channel = 0; channel < 3; ++channel)
                        require(abs(int(rendered[index + channel]) - int((rgb >> (16 - channel * 8)) & 255)) <= 1,
                                "RGB565/HUD composition differs from the full guest framebuffer");
                    require(rendered[index + 3] == 255, "compact Canvas lost opaque alpha");
                }
            }
            require(glGetError() == GL_NO_ERROR, "compact Canvas generated a GL error");
        }
        require(SDL_GL_MakeCurrent(window, context) == 0, SDL_GetError());
        hle::gl_set_window(window);
        SDL_DestroyWindow(target);
    }
    puts("RGB565 background, HUD rectangle, color precision, scaling and upload-byte reduction passed");
}
#endif
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
#ifdef BADA_TEST_DESKTOP
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_COMPATIBILITY);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
#else
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, api);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
#endif
        SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
        SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
        SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
        SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);
        SDL_Window* window = SDL_CreateWindow("Bada GL test", 0, 0, 64, 64, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
        require(window != nullptr, SDL_GetError());
        auto context = SDL_GL_CreateContext(window);
#ifndef BADA_TEST_DESKTOP
        if (!context && api == 1 && std::strstr(SDL_GetError(), "EGL_BAD_ALLOC")) {
            // Some Mesa software drivers advertise ES 1 configs but cannot
            // create an ES 1 context, including on the Linux CI runner. This
            // happens before any guest handler runs; ES 2 remains mandatory.
            fprintf(stderr, "SKIP: host cannot create an ES 1 context: %s\n", SDL_GetError());
            SDL_DestroyWindow(window);
            SDL_Quit();
            return 77;
        }
#endif
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
        format_query_test(cpu, api);
        resize_test(cpu, api, window);
        if (api == 1) rotated_window_test(cpu, window);
#ifndef BADA_TEST_DESKTOP
        if (api == 2) compact_canvas_test(cpu, window);
#endif
        SDL_GL_DeleteContext(context);
        SDL_DestroyWindow(window);
        SDL_Quit();
#ifdef BADA_TEST_DESKTOP
        printf("Desktop compatibility GL: ES %d %s guest textures, arrays, indices, swap and Canvas passed\n", api, argv[2]);
#else
        printf("Native ES %d %s: guest textures, arrays, indices, swap and Canvas passed\n", api, argv[2]);
#endif
        return 0;
    } catch (const std::exception& error) {
        fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
