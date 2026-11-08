#undef NDEBUG
#define SDL_MAIN_HANDLED
#include "display.h"
#include <cassert>

int main() {
    auto fit = display::content_rect(480, 800, 1000, 1400);
    assert(fit.width == 840 && fit.height == 1400 && fit.x == 80);
    display::integer_scaling = true;
    auto sharp = display::content_rect(480, 800, 1000, 1700);
    assert(sharp.width == 960 && sharp.height == 1600 && sharp.x == 20 && sharp.y == 50);
    auto small = display::content_rect(480, 800, 240, 400);
    assert(small.width == 240 && small.height == 400);
    auto landscape = display::content_rect(800, 480, 1700, 1000);
    assert(landscape.width == 1600 && landscape.height == 960 && landscape.x == 50 && landscape.y == 20);
    // Android output presets cap physical presentation without resizing a device.
    display::integer_scaling = false;
    display::output_width = 240; display::output_height = 400;
#ifdef __ANDROID__
    auto capped = display::content_rect(480, 800, 1080, 1920);
    assert(capped.width == 240 && capped.height == 400 && capped.x == 420 && capped.y == 760);
#endif
}
