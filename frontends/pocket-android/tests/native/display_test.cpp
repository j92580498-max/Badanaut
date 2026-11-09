#undef NDEBUG
#define SDL_MAIN_HANDLED
#include "display.h"
#include <cassert>

int main() {
    display::fill_screen = false;
    auto fit = display::content_rect(480, 800, 1000, 1400);
    assert(fit.width == 840 && fit.height == 1400 && fit.x == 80);
    display::integer_scaling = true;
    auto sharp = display::content_rect(480, 800, 1000, 1700);
    assert(sharp.width == 960 && sharp.height == 1600 && sharp.x == 20 && sharp.y == 50);
    auto small = display::content_rect(480, 800, 240, 400);
    assert(small.width == 240 && small.height == 400);
    auto landscape = display::content_rect(800, 480, 1700, 1000);
    assert(landscape.width == 1600 && landscape.height == 960 && landscape.x == 50 && landscape.y == 20);
    // A preset selects a render buffer, never a small presentation rectangle.
    display::integer_scaling = false;
    display::output_width = 240; display::output_height = 400;
#ifdef __ANDROID__
    display::fill_screen = true;
    auto filled = display::content_rect(480, 800, 1080, 1920);
    assert(filled.width == 1152 && filled.height == 1920 && filled.x == -36 && filled.y == 0);
    auto wide = display::content_rect(800, 480, 1920, 1080);
    assert(wide.width == 1920 && wide.height == 1152 && wide.x == 0 && wide.y == -36);
    auto buffer = display::buffer_size(800, 480);
    assert(buffer.width == 400 && buffer.height == 240);
    display::output_width = display::output_height = 0;
    buffer = display::buffer_size(800, 480);
    assert(buffer.width == 800 && buffer.height == 480);
    display::integer_scaling = true;
    auto integer = display::content_rect(480, 800, 1080, 1920);
    assert(integer.width == 1440 && integer.height == 2400 && integer.x == -180 && integer.y == -240);
#endif
}
