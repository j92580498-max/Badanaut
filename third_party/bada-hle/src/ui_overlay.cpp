#include "ui_overlay.h"
#include "cpu.h"
#include "hle/ui.h"
#include <SDL2/SDL_opengl.h>
#include <cstdio>
#include <cstring>

// Nuklear draws the stand-ins for the native bada controls (see hle/ui.h).
// The upstream GL2 backend matches the compatibility context
// we already ask SDL for.
#define NK_INCLUDE_FIXED_TYPES
#define NK_INCLUDE_STANDARD_IO
#define NK_INCLUDE_STANDARD_VARARGS
#define NK_INCLUDE_DEFAULT_ALLOCATOR
#define NK_INCLUDE_VERTEX_BUFFER_OUTPUT
#define NK_INCLUDE_FONT_BAKING
#define NK_INCLUDE_DEFAULT_FONT
#define NK_IMPLEMENTATION
#define NK_SDL_GL2_IMPLEMENTATION
#include <nuklear.h>
#include <nuklear_sdl_gl2.h>

namespace {
nk_context* g_ctx = nullptr;
SDL_Window* g_win = nullptr;
bool g_input_open = false; // between nk_input_begin/end
bool g_drawn_in_frame = false;

// What the user did, to be replayed into the guest from the frame loop. draw()
// runs inside sglSwapBuffers -- already inside uc_emu_start -- and re-entering
// the guest there would nest uc_emu_start and hang.
struct Pending {
    uint32_t object = 0;
    int generation = 0;
    bool set_text = false;
    std::string text;
    int pressed = -1;
};
std::vector<Pending> g_pending;

// The guest lays its controls out in device pixels (800x480); the window may be
// any size.
void device_to_window(SDL_Window* win, int dx, int dy, float* wx, float* wy) {
    int ww = 800, wh = 480;
    SDL_GetWindowSize(win, &ww, &wh);
    *wx = (float)dx * (float)ww / 800.0f;
    *wy = (float)dy * (float)wh / 480.0f;
}
} // namespace

namespace overlay {

void init(SDL_Window* win) {
    g_win = win;
    g_ctx = nk_sdl_init(win);
    nk_font_atlas* atlas = nullptr;
    nk_sdl_font_stash_begin(&atlas);
    nk_sdl_font_stash_end(); // built-in font: no external file to ship
}

void shutdown() {
    if (g_ctx) {
        nk_sdl_shutdown();
        g_ctx = nullptr;
    }
    g_input_open = false;
    g_pending.clear();
}

bool active() { return g_ctx && hle::ui_active(); }

bool handle_event(SDL_Event& e) {
    if (!active()) {
        return false;
    }
    if (!g_input_open) {
        nk_input_begin(g_ctx);
        g_input_open = true;
    }
    nk_sdl_handle_event(&e);
    // While a control is up it owns the keyboard and mouse -- the guest must not
    // also see those as touches.
    switch (e.type) {
    case SDL_KEYDOWN:
    case SDL_KEYUP:
    case SDL_TEXTINPUT:
    case SDL_MOUSEBUTTONDOWN:
    case SDL_MOUSEBUTTONUP:
    case SDL_MOUSEMOTION:
    case SDL_FINGERDOWN:
    case SDL_FINGERUP:
    case SDL_FINGERMOTION: return true;
    default: return false;
    }
}

void draw(SDL_Window* win) {
    if (!g_ctx) {
        return;
    }
    std::vector<hle::EditField*> fields = hle::ui_fields();
    bool any = false;
    for (hle::EditField* f : fields) {
        any = any || f->visible;
    }
    if (!any) {
        if (g_input_open) { // drain the events we buffered while it was up
            nk_input_end(g_ctx);
            g_input_open = false;
            nk_clear(g_ctx);
        }
        return;
    }
    g_drawn_in_frame = true;
    if (!g_input_open) {
        nk_input_begin(g_ctx);
    }
    nk_input_end(g_ctx);
    g_input_open = false;

    for (hle::EditField* f : fields) {
        if (!f->visible) {
            continue;
        }
        float wx = 0, wy = 0, ww = 0, wh = 0;
        device_to_window(win, f->x, f->y, &wx, &wy);
        device_to_window(win, f->w, f->h ? f->h : 40, &ww, &wh);
        if (ww < 200) {
            ww = 200;
        }
        char title[64];
        snprintf(title, sizeof title, "edit%u.%d", f->obj, f->generation); // identity; not shown
        // Give the frame room for the text row and the command buttons.
        if (nk_begin(g_ctx, title, nk_rect(wx, wy, ww, wh + 48),
                     NK_WINDOW_BORDER | NK_WINDOW_NO_SCROLLBAR)) {
            char buf[256];
            int len = (int)f->text.size();
            if (len > (int)sizeof(buf) - 1) {
                len = (int)sizeof(buf) - 1;
            }
            memcpy(buf, f->text.data(), (size_t)len);
            buf[len] = 0;

            nk_layout_row_dynamic(g_ctx, 28, 1);
            // bada's overlay keypad owns the focus while it is up.
            nk_edit_focus(g_ctx, 0);
            int max = f->limit > 0 && f->limit < (int)sizeof(buf) ? f->limit + 1 : (int)sizeof(buf);
            nk_flags ev = nk_edit_string(g_ctx, NK_EDIT_FIELD | NK_EDIT_SIG_ENTER, buf, &len, max,
                                         nk_filter_default);
            buf[len] = 0;
            std::string typed(buf, (size_t)len);
            if (typed != f->text) {
                f->text = typed; // echo now; the guest hears about it next frame
                g_pending.push_back({f->obj, f->generation, true, typed, -1});
            }

            // Whatever command buttons the app put on the overlay keypad, in the
            // app's own wording ("Cancel" / "Done"). Enter confirms.
            int n = (f->has_command[0] ? 1 : 0) + (f->has_command[1] ? 1 : 0);
            int confirm = f->has_command[0] ? 0 : 1; // bada puts the confirming action left
            nk_layout_row_dynamic(g_ctx, 26, n ? n : 1);
            int pressed = -1;
            for (int i = 0; i < 2; i++) {
                if (f->has_command[i] && nk_button_label(g_ctx, f->command[i].c_str())) {
                    pressed = i;
                }
            }
            if (!n && nk_button_label(g_ctx, "Done")) {
                pressed = confirm;
            }
            if (ev & NK_EDIT_COMMITED) {
                pressed = confirm;
            }
            if (pressed >= 0) {
                f->visible = false; // close now so the overlay stops drawing it
                g_pending.push_back({f->obj, f->generation, false, std::string(), pressed});
            }
        }
        nk_end(g_ctx);
    }
    // nk_sdl_render draws from client-side arrays. If the guest left a buffer or
    // a VAO bound, GL reads those pointers as offsets into it and the text comes
    // out as garbage -- so clear the bindings and put them back.
    typedef void(APIENTRY * BindFn)(GLenum, GLuint);
    typedef void(APIENTRY * BindVaoFn)(GLuint);
    static auto bind_buffer = (BindFn)SDL_GL_GetProcAddress("glBindBuffer");
    static auto bind_vao = (BindVaoFn)SDL_GL_GetProcAddress("glBindVertexArray");
    GLint abuf = 0, ebuf = 0, vao = 0;
    if (bind_buffer) {
        glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &abuf);
        glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &ebuf);
        bind_buffer(GL_ARRAY_BUFFER, 0);
        bind_buffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    }
    if (bind_vao) {
        glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
        bind_vao(0);
    }
    // Nuklear draws fixed-function through texture unit 0 and assumes an
    // identity texture matrix. The guest leaves whatever it was last using, and
    // a stale unit or texture matrix makes every glyph sample one texel -- the
    // text comes out as solid blocks.
    typedef void(APIENTRY * ActiveTexFn)(GLenum);
    typedef void(APIENTRY * UseProgFn)(GLuint);
    static auto active_tex = (ActiveTexFn)SDL_GL_GetProcAddress("glActiveTexture");
    static auto client_active_tex = (ActiveTexFn)SDL_GL_GetProcAddress("glClientActiveTexture");
    static auto use_program = (UseProgFn)SDL_GL_GetProcAddress("glUseProgram");
    if (active_tex) {
        active_tex(GL_TEXTURE0);
    }
    if (client_active_tex) {
        client_active_tex(GL_TEXTURE0);
    }
    if (use_program) {
        use_program(0); // a program still bound would override all of this
    }
    glMatrixMode(GL_TEXTURE);
    glPushMatrix();
    glLoadIdentity();
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    // nk_sdl_render pushes/pops the GL state it touches, so the guest's
    // fixed-function setup survives the overlay.
    nk_sdl_render(NK_ANTI_ALIASING_ON);
    glMatrixMode(GL_TEXTURE);
    glPopMatrix();
    glMatrixMode(GL_MODELVIEW);
    if (bind_buffer) {
        bind_buffer(GL_ARRAY_BUFFER, (GLuint)abuf);
        bind_buffer(GL_ELEMENT_ARRAY_BUFFER, (GLuint)ebuf);
    }
    if (bind_vao) {
        bind_vao((GLuint)vao);
    }
}

void begin_frame() { g_drawn_in_frame = false; }
void present_idle(SDL_Window* win) {
    if (!g_drawn_in_frame && active()) {
        draw(win);
        SDL_GL_SwapWindow(win);
    }
}

void pump(Cpu& cpu) {
    std::vector<Pending> todo;
    todo.swap(g_pending); // a listener may open another control
    for (const Pending& p : todo) {
        hle::EditField* field = nullptr;
        for (auto* current : hle::ui_fields()) {
            if (current->obj == p.object && current->generation == p.generation) { field = current; break; }
        }
        if (!field) continue;
        if (p.set_text) {
            field->text.clear(); // ui_set_text ignores a no-op change
            hle::ui_set_text(cpu, *field, p.text);
        } else {
            hle::ui_commit(cpu, *field, p.pressed);
        }
    }
}

} // namespace overlay
