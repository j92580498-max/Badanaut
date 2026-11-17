#pragma once
#include <cstdint>
#include <map>

class Cpu;

namespace hle {

struct GraphicsSurface {
    uint32_t pixels = 0;
    int width = 0, height = 0, pitch = 0;
    uint32_t format = 2, background = 0xff000000;
    bool masking = false;
    uint32_t mask = 0;
};

struct GraphicsCanvas {
    GraphicsSurface surface;
    bool window = false;
    int x = 0, y = 0;
};

// Pixel storage belongs to the guest process. Window Canvas objects share the
// frame buffer; constructed memory canvases and bitmaps have their own buffers.
struct GraphicsState {
    std::map<uint32_t, GraphicsCanvas> canvases;
    std::map<uint32_t, GraphicsSurface> bitmaps;
    GraphicsSurface frame{0, 480, 640, 1920};
    uint32_t canvas_vtable = 0, bitmap_vtable = 0, info_vtable = 0;
};

void register_graphics();
uint32_t make_frame_canvas(Cpu& cpu, int width, int height);
void resize_frame_canvas(Cpu& cpu, int width, int height);

} // namespace hle
