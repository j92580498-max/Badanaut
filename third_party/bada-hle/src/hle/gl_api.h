#pragma once
#include <algorithm>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace hle {

struct SurfaceRect { int x, y, width, height; };

// Preserve the game's aspect ratio on displays with a different native size.
inline SurfaceRect fit_surface(int width, int height, int output_width, int output_height) {
    if (width <= 0 || height <= 0 || output_width <= 0 || output_height <= 0) return {0, 0, 0, 0};
    int w = output_width, h = output_height;
    if (static_cast<int64_t>(output_width) * height <= static_cast<int64_t>(output_height) * width)
        h = std::max(1, static_cast<int>(static_cast<int64_t>(output_width) * height / width));
    else
        w = std::max(1, static_cast<int>(static_cast<int64_t>(output_height) * width / height));
    return {(output_width - w) / 2, (output_height - h) / 2, w, h};
}

SurfaceRect gl_content_rect(int output_width, int output_height);
void gl_map_touch(int x, int y, int width, int height, int& guest_x, int& guest_y);
void gl_resize();
void gl_set_surface_size(int width, int height);
void gl_set_context_factory(std::function<bool(int)> factory);
struct CanvasUploadInfo { uint32_t bytes = 0; bool compact = false; };
CanvasUploadInfo gl_canvas_upload_info(); // payload of the last Canvas presentation

// Samsung exports glFoo_1 for the fixed pipeline and glFoo_2 for shaders.
// Canvas-only applications use the ES 2 presenter.
inline int gles_context_version(const std::vector<std::string>& imports) {
    bool fixed_pipeline = false;
    for (const auto& name : imports) {
        if (name.compare(0, 2, "gl") != 0 || name.size() < 4) continue;
        if (name.compare(name.size() - 2, 2, "_2") == 0) return 2;
        if (name.compare(name.size() - 2, 2, "_1") == 0) fixed_pipeline = true;
    }
    return fixed_pipeline ? 1 : 2;
}

} // namespace hle
