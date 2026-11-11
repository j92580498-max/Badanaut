#pragma once
#include "hle/gl_api.h"
#include <SDL2/SDL.h>
#include <cmath>

// Presentation preferences never change the guest's screen or GL state.
namespace display {
inline bool integer_scaling = false, smooth = false;

inline hle::SurfaceRect content_rect(int width, int height, int ow, int oh) {
    auto area = hle::fit_surface(width, height, ow, oh);
    if (integer_scaling && width > 0 && height > 0) {
        int scale = std::min(area.width / width, area.height / height);
        if (scale >= 1) {
            area.width = width * scale;
            area.height = height * scale;
        }
    }
    area.x = (ow - area.width) / 2;
    area.y = (oh - area.height) / 2;
    return area;
}

inline void size_window(SDL_Window* window, int guest_width, int guest_height) {
#ifndef __ANDROID__
    if (!window || SDL_GetWindowFlags(window) & (SDL_WINDOW_FULLSCREEN | SDL_WINDOW_FULLSCREEN_DESKTOP)) return;
    int w = guest_width, h = guest_height;
    SDL_Rect available;
    if (SDL_GetDisplayUsableBounds(SDL_GetWindowDisplayIndex(window), &available) == 0) {
        double scale = std::min({1.0, std::max(1, available.w - 64) / static_cast<double>(w),
                                std::max(1, available.h - 64) / static_cast<double>(h)});
        w = std::max(1, static_cast<int>(w * scale));
        h = std::max(1, static_cast<int>(h * scale));
    }
    SDL_SetWindowSize(window, w, h);
    SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
#endif
}
} // namespace display
