#define SDL_MAIN_HANDLED
#include <SDL2/SDL.h>
#include "cpu.h"
#include "touch_input.h"
#include "display.h"
#include <cstdio>
#include <set>
#include <stdexcept>
#include <vector>

namespace {
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

uint32_t call(Cpu& cpu, const char* name, std::initializer_list<uint32_t> args) {
    int index = 0;
    for (uint32_t arg : args) cpu.set_reg(UC_ARM_REG_R0 + index++, arg);
    hle::call(name, cpu);
    return cpu.reg(UC_ARM_REG_R0);
}

void finger(Uint32 kind, SDL_TouchID device, SDL_FingerID id, float x, float y) {
    SDL_Event event{};
    event.type = kind;
    event.tfinger.touchId = device;
    event.tfinger.fingerId = id;
    event.tfinger.x = x;
    event.tfinger.y = y;
    require(SDL_PushEvent(&event) == 1, "could not queue a finger event");
}

void mouse(Uint32 kind, Uint32 device, int x, int y) {
    SDL_Event event{};
    event.type = kind;
    if (kind == SDL_MOUSEMOTION) {
        event.motion.which = device;
        event.motion.x = x;
        event.motion.y = y;
    } else {
        event.button.which = device;
        event.button.button = SDL_BUTTON_LEFT;
        event.button.x = x;
        event.button.y = y;
    }
    require(SDL_PushEvent(&event) == 1, "could not queue a mouse event");
}

void touch_test(Cpu& cpu) {
    constexpr const char* set_multi = "_ZN3Osp2Ui5Touch20SetMultipointEnabledERKNS0_7ControlEb";
    constexpr const char* is_multi = "_ZNK3Osp2Ui5Touch19IsMultipointEnabledERKNS0_7ControlE";
    constexpr const char* point_id = "_ZNK3Osp2Ui14TouchEventInfo10GetPointIdEv";
    const uint32_t touch = cpu.alloc(16), form = cpu.alloc(16), other_form = cpu.alloc(16);
    const uint32_t info = cpu.make_stub_object(32);
    require(call(cpu, is_multi, {touch, form}) == 0, "multi-point input enabled before the app requested it");
    require(call(cpu, set_multi, {touch, form, 1}) == 0 &&
            call(cpu, is_multi, {touch, form}) == 1, "app could not enable multi-point input");
    require(!hle::touch_multipoint_enabled(other_form), "multi-point settings leaked to another control");

    input::TouchInput router;
    std::vector<input::TouchEvent> received;
    const input::TouchInput::Sink sink = [&](const input::TouchEvent& event) {
        hle::touch_event_set(info, event.point_id);
        require(call(cpu, point_id, {info}) == event.point_id, "Bada returned the wrong contact identity");
        received.push_back(event);
    };
    const auto pump = [&] {
        SDL_Event event;
        while (SDL_PollEvent(&event))
            router.handle(event, 800, 480, hle::touch_multipoint_enabled(form), sink);
    };

    // IDs with identical low 32 bits must remain separate, and releasing the
    // second finger must not stop motion from the first (Nova joystick/fire).
    constexpr SDL_FingerID first = 7, second = (SDL_FingerID{1} << 32) | first;
    finger(SDL_FINGERDOWN, 3, first, 0.125f, 0.5f);
    finger(SDL_FINGERDOWN, 3, second, 0.75f, 0.25f);
    finger(SDL_FINGERMOTION, 3, first, 0.25f, 0.5f);
    finger(SDL_FINGERUP, 3, second, 0.75f, 0.25f);
    finger(SDL_FINGERMOTION, 3, first, 0.375f, 0.5f);
    pump();
    require(received.size() == 5, "a second finger suppressed a valid event");
    const uint32_t a = received[0].point_id, b = received[1].point_id;
    require(a != b && received[2].point_id == a && received[3].point_id == b && received[4].point_id == a,
            "contact IDs collided or changed across press/move/release");
    require(received[0].x == 100 && received[0].y == 240 && received[4].x == 300 && received[4].y == 240,
            "normalized finger coordinates were not converted to window pixels");

    // Android pointer IDs repeat on different devices. A real mouse has its
    // own contact; SDL's synthetic mouse/finger copies must be discarded.
    finger(SDL_FINGERDOWN, 4, first, 0.5f, 0.5f);
    mouse(SDL_MOUSEBUTTONDOWN, 42, 50, 60);
    mouse(SDL_MOUSEBUTTONUP, SDL_TOUCH_MOUSEID, 50, 60);
    finger(SDL_FINGERDOWN, SDL_MOUSE_TOUCHID, 0, 0.5f, 0.5f);
    mouse(SDL_MOUSEMOTION, 42, 70, 80);
    pump();
    require(received.size() == 8 && received[5].point_id != a &&
            received[6].point_id != a && received[6].point_id != received[5].point_id &&
            received[7].point_id == received[6].point_id,
            "device IDs or synthetic mouse events corrupted active contacts");
    const size_t before_cancel = received.size();
    router.cancel(sink);
    require(received.size() == before_cancel + 3, "focus cancellation left an active contact pressed");
    std::set<uint32_t> released;
    for (size_t i = before_cancel; i < received.size(); ++i) {
        require(received[i].kind == input::TouchKind::released, "focus cancellation emitted a new press");
        released.insert(received[i].point_id);
    }
    require(released == std::set<uint32_t>{a, received[5].point_id, received[6].point_id},
            "focus cancellation changed the contacts being released");
    finger(SDL_FINGERMOTION, 3, first, 0.25f, 0.5f);
    finger(SDL_FINGERUP, 4, first, 0.5f, 0.5f);
    pump();
    require(received.size() == before_cancel + 3, "cancelled fingers restarted a stroke");

    call(cpu, set_multi, {touch, form, 0});
    received.clear();
    finger(SDL_FINGERDOWN, 3, first, 0.25f, 0.5f);
    finger(SDL_FINGERDOWN, 3, second, 0.75f, 0.5f);
    finger(SDL_FINGERUP, 3, second, 0.75f, 0.5f);
    finger(SDL_FINGERMOTION, 3, first, 0.5f, 0.5f);
    finger(SDL_FINGERUP, 3, first, 0.5f, 0.5f);
    pump();
    require(received.size() == 3 && received[0].point_id == received[1].point_id &&
            received[1].point_id == received[2].point_id && received[1].kind == input::TouchKind::moved,
            "single-point apps lost their primary contact when a second finger lifted");
}

void display_touch_test(Cpu& cpu) {
    constexpr const char* form_orientation = "_ZN3Osp2Ui8Controls4Form14SetOrientationENS0_11OrientationE";
    constexpr const char* frame_orientation = "_ZN3Osp2Ui8Controls5Frame14SetOrientationENS0_11OrientationE";
    display::fill_screen = true;
    call(cpu, form_orientation, {cpu.alloc(16), 2}); // N.O.V.A. requests landscape.
    int x = 0, y = 0;
    hle::gl_map_touch(960, 540, 1920, 1080, x, y);
    require(x == 400 && y == 240, "landscape center touch was rotated or scaled incorrectly");
    hle::gl_map_touch(0, 0, 1920, 1080, x, y);
    require(x == 0 && y == 15, "landscape crop was not included in touch coordinates");
    hle::gl_map_touch(200, 120, 400, 240, x, y);
    require(x == 400 && y == 240, "low-resolution landscape buffer changed guest coordinates");

    // Rotating the Form refreshes coordinates without restarting the engine.
    call(cpu, frame_orientation, {cpu.alloc(16), 0});
    hle::gl_map_touch(540, 960, 1080, 1920, x, y);
    require(x == 240 && y == 400, "portrait touch still used landscape dimensions");
    hle::gl_map_touch(0, 0, 1080, 1920, x, y);
    require(x == 15 && y == 0, "portrait crop was not included in touch coordinates");
    display::fill_screen = false;
}
} // namespace

int main() {
    try {
        SDL_SetMainReady();
        SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
        SDL_SetHint(SDL_HINT_MOUSE_TOUCH_EVENTS, "0");
        require(SDL_Init(SDL_INIT_TIMER | SDL_INIT_EVENTS) == 0, SDL_GetError());
        ElfImage image;
        const uint32_t instruction = 0xe12fff1e;
        image.segments.push_back({0x100000, 4, 4096, 5, reinterpret_cast<const uint8_t*>(&instruction)});
        Cpu cpu;
        std::string error;
        require(cpu.init(image, error), error.c_str());
        touch_test(cpu);
        display_touch_test(cpu);
        SDL_Quit();
        puts("Simultaneous SDL fingers, Bada IDs, mouse isolation and focus cancellation passed");
        return 0;
    } catch (const std::exception& error) {
        fprintf(stderr, "FAIL: %s\n", error.what());
        SDL_Quit();
        return 1;
    }
}
