#pragma once
#include "hle/gl_api.h"
#include <SDL2/SDL.h>
#include <cmath>

// The guest keeps its native coordinates while the render buffer can be resized.
namespace display {
inline int output_width = 0, output_height = 0;
inline std::string selected_resolution = "native";
inline int device_width = 480, device_height = 800;
inline int rotation = 0; // counterclockwise quarter-turns from the guest framebuffer
inline bool integer_scaling = false, smooth = false;
#ifdef __ANDROID__
inline bool fill_screen = true;
void android_set_display_size(int width, int height);
#else
inline bool fill_screen = false;
#endif

inline hle::SurfaceRect buffer_size(int native_width, int native_height) {
    if (!output_width || !output_height) return {0, 0, native_width, native_height};
    const bool landscape = native_width > native_height;
    const int short_side = std::min(output_width, output_height), long_side = std::max(output_width, output_height);
    return {0, 0, landscape ? long_side : short_side, landscape ? short_side : long_side};
}

inline hle::SurfaceRect content_rect(int width, int height, int ow, int oh) {
    auto area = fill_screen ? hle::fill_surface(width, height, ow, oh) : hle::fit_surface(width, height, ow, oh);
    if (integer_scaling && width > 0 && height > 0) {
        int scale = fill_screen ? std::max((ow + width - 1) / width, (oh + height - 1) / height)
                                : std::min(area.width / width, area.height / height);
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
#ifdef __ANDROID__
    android_set_display_size(guest_width, guest_height);
#else
    if (!window || SDL_GetWindowFlags(window) & (SDL_WINDOW_FULLSCREEN | SDL_WINDOW_FULLSCREEN_DESKTOP)) return;
    const auto buffer = buffer_size(guest_width, guest_height);
    int w = buffer.width, h = buffer.height;
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
