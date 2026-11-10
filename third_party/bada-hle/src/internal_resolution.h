#pragma once
#include <cstdint>
struct SDL_Window;

namespace internal_resolution {
void configure(int percent); // 100, 75 or 50; guest dimensions stay unchanged
int percent();
bool begin(int width, int height, int guest_api);
bool enabled();
uint32_t framebuffer();
void pause(); // full-resolution Canvas/host UI presentation
void resume();
void present(SDL_Window* window, int width, int height, int guest_api);
void reset(); // the old context has already been destroyed
} // namespace internal_resolution
