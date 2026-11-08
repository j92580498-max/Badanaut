#pragma once
#include <cstddef>
#include <cstdint>

class Cpu;
struct ElfImage;

namespace pixel_accel {
// Replace verified pixel-copy kernels with an equivalent native operation.
// The executable and the resulting guest framebuffer remain unchanged.
size_t install(Cpu& cpu, const ElfImage& image);
struct Expansion {
    const uint16_t* source;   // RGB565 rows with a 256-pixel stride
    const uint32_t* expanded; // the exact 480-pixel guest image before HUD writes
    uint32_t rows;
};
bool snapshot(Cpu& cpu, uint32_t destination, int width, int height, Expansion& output);
}
