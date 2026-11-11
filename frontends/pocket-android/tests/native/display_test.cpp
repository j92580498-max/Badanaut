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
    // Presentation fills the available area while preserving the guest aspect ratio.
    display::integer_scaling = false;
    auto portrait = display::content_rect(480, 800, 1080, 1920);
    assert(portrait.width == 1080 && portrait.height == 1800 && portrait.x == 0 && portrait.y == 60);
}
