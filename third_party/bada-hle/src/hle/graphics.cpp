#include "graphics.h"
#include "cpu.h"
#include "dispatch.h"
#include "gl_api.h"
#include "guest.h"
#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <vector>

namespace {
constexpr uint32_t invalid_arg = 0xa0000000u + 1909;
constexpr uint32_t invalid_state = 0xa0000000u + 1926;
constexpr uint32_t out_of_memory = 0xa0000000u + 2505;
constexpr uint32_t out_of_range = 0xa0000000u + 2506;

hle::GraphicsState& state(Cpu& cpu) {
    if (!cpu.graphics) cpu.graphics = std::make_unique<hle::GraphicsState>();
    return *cpu.graphics;
}

struct Rect { int x, y, width, height; };
bool rectangle(Cpu& cpu, uint32_t object, Rect& rect) {
    if (!object || !cpu.memory_mapped(object, 20)) return false;
    rect = {static_cast<int32_t>(cpu.read_u32(object + 4)),
            static_cast<int32_t>(cpu.read_u32(object + 8)),
            static_cast<int32_t>(cpu.read_u32(object + 12)),
            static_cast<int32_t>(cpu.read_u32(object + 16))};
    return true;
}
bool point(Cpu& cpu, uint32_t object, int& x, int& y) {
    if (!object || !cpu.memory_mapped(object, 12)) return false;
    x = static_cast<int32_t>(cpu.read_u32(object + 4));
    y = static_cast<int32_t>(cpu.read_u32(object + 8));
    return true;
}

uint32_t allocate(Cpu& cpu, hle::GraphicsSurface& surface, int width, int height, uint32_t format) {
    if (format < 1 || format > 3) return invalid_arg;
    if (width <= 0 || height <= 0) return out_of_range;
    const uint64_t pitch = (uint64_t(width) * (format == 1 ? 2 : 4) + 3) & ~uint64_t{3};
    if (pitch > std::numeric_limits<int>::max() || pitch * height > guest::HEAP_SIZE)
        return out_of_memory;
    const uint32_t pixels = cpu.alloc(static_cast<uint32_t>(pitch * height), 4);
    if (!pixels) return out_of_memory;
    cpu.free(surface.pixels);
    surface.pixels = pixels;
    surface.width = width; surface.height = height;
    surface.pitch = static_cast<int>(pitch); surface.format = format;
    return 0;
}
uint32_t ensure_pixels(Cpu& cpu, hle::GraphicsSurface& surface) {
    if (surface.pixels) return 0;
    return allocate(cpu, surface, surface.width, surface.height, surface.format);
}

hle::GraphicsSurface* canvas(Cpu& cpu, uint32_t object, bool legacy = false) {
    auto& graphics = state(cpu);
    auto found = graphics.canvases.find(object);
    if (found == graphics.canvases.end()) {
        if (!legacy) return nullptr;
        // Earlier wrappers use an opaque Canvas object without constructing it.
        // Preserve their shared frame buffer, including the null diagnostic handle.
        found = graphics.canvases.emplace(object, hle::GraphicsCanvas{{}, true}).first;
    }
    return found->second.window ? &graphics.frame : &found->second.surface;
}
hle::GraphicsSurface* bitmap(Cpu& cpu, uint32_t object) {
    auto& bitmaps = state(cpu).bitmaps;
    const auto found = bitmaps.find(object);
    return found == bitmaps.end() ? nullptr : &found->second;
}

void ok(Cpu& cpu) { cpu.set_ret32(0); }
void delete_object(Cpu& cpu) { cpu.free(cpu.arg32(0)); cpu.set_ret32(0); }
void canvas_destroy(Cpu& cpu) {
    auto& canvases = state(cpu).canvases;
    const auto found = canvases.find(cpu.arg32(0));
    if (found != canvases.end()) {
        if (!found->second.window) cpu.free(found->second.surface.pixels);
        canvases.erase(found);
    }
    cpu.set_ret32(0);
}
void canvas_delete(Cpu& cpu) {
    const uint32_t object = cpu.arg32(0);
    canvas_destroy(cpu); cpu.free(object);
}
void bitmap_destroy(Cpu& cpu) {
    auto& bitmaps = state(cpu).bitmaps;
    const auto found = bitmaps.find(cpu.arg32(0));
    if (found != bitmaps.end()) { cpu.free(found->second.pixels); bitmaps.erase(found); }
    cpu.set_ret32(0);
}
void bitmap_delete(Cpu& cpu) {
    const uint32_t object = cpu.arg32(0);
    bitmap_destroy(cpu); cpu.free(object);
}
void canvas_ctor(Cpu& cpu) {
    const uint32_t object = cpu.arg32(0);
    canvas_destroy(cpu);
    auto& graphics = state(cpu);
    if (!graphics.canvas_vtable)
        graphics.canvas_vtable = cpu.make_vtable(4, {{0, canvas_destroy}, {1, canvas_delete}});
    cpu.write_u32(object, graphics.canvas_vtable);
    graphics.canvases[object] = {};
    cpu.set_ret32(object);
}
void bitmap_ctor(Cpu& cpu) {
    const uint32_t object = cpu.arg32(0);
    bitmap_destroy(cpu);
    auto& graphics = state(cpu);
    if (!graphics.bitmap_vtable)
        graphics.bitmap_vtable = cpu.make_vtable(4, {{0, bitmap_destroy}, {1, bitmap_delete}});
    cpu.write_u32(object, graphics.bitmap_vtable);
    graphics.bitmaps[object] = {};
    cpu.set_ret32(object);
}
void info_ctor(Cpu& cpu) {
    auto& graphics = state(cpu);
    if (!graphics.info_vtable)
        graphics.info_vtable = cpu.make_vtable(4, {{0, ok}, {1, delete_object}});
    const uint32_t object = cpu.arg32(0), fields[6] = {};
    cpu.write_u32(object, graphics.info_vtable);
    cpu.write_mem(object + 4, fields, sizeof fields);
    cpu.set_ret32(object);
}
void canvas_construct(Cpu& cpu) {
    Rect rect{};
    auto& canvases = state(cpu).canvases;
    auto found = canvases.find(cpu.arg32(0));
    if (found == canvases.end() || !rectangle(cpu, cpu.arg32(1), rect)) { cpu.set_ret32(invalid_arg); return; }
    const uint32_t error = allocate(cpu, found->second.surface, rect.width, rect.height, 2);
    if (!error) { found->second.window = false; found->second.x = rect.x; found->second.y = rect.y; }
    cpu.set_ret32(error);
}
void canvas_construct_screen(Cpu& cpu) {
    auto& graphics = state(cpu);
    auto found = graphics.canvases.find(cpu.arg32(0));
    if (found == graphics.canvases.end()) { cpu.set_ret32(invalid_state); return; }
    cpu.free(found->second.surface.pixels);
    found->second = {{}, true};
    cpu.set_ret32(ensure_pixels(cpu, graphics.frame));
}
void bitmap_construct(Cpu& cpu) {
    auto* surface = bitmap(cpu, cpu.arg32(0));
    int width = 0, height = 0;
    if (!surface || !point(cpu, cpu.arg32(1), width, height)) { cpu.set_ret32(invalid_arg); return; }
    cpu.set_ret32(allocate(cpu, *surface, width, height, cpu.arg32(2)));
}
void lock(Cpu& cpu, hle::GraphicsSurface* surface) {
    const uint32_t info = cpu.arg32(1);
    if (!surface || !info || !cpu.memory_mapped(info, 28)) { cpu.set_ret32(invalid_arg); return; }
    const uint32_t error = ensure_pixels(cpu, *surface);
    if (error) { cpu.set_ret32(error); return; }
    const uint32_t fields[] = {static_cast<uint32_t>(surface->width),
        static_cast<uint32_t>(surface->height), static_cast<uint32_t>(surface->pitch),
        surface->format == 1 ? 16u : 32u, surface->format, surface->pixels};
    cpu.write_mem(info + 4, fields, sizeof fields);
    cpu.set_ret32(0);
}
void canvas_lock(Cpu& cpu) { lock(cpu, canvas(cpu, cpu.arg32(0), true)); }
void bitmap_lock(Cpu& cpu) { lock(cpu, bitmap(cpu, cpu.arg32(0))); }
void canvas_show(Cpu& cpu) {
    auto* surface = canvas(cpu, cpu.arg32(0), true);
    const uint32_t error = ensure_pixels(cpu, *surface);
    if (!error) hle::gl_present_canvas(cpu, surface->pixels, surface->width, surface->height);
    cpu.set_ret32(error);
}
void canvas_bounds(Cpu& cpu) {
    const uint32_t output = cpu.arg32(0);
    auto* surface = canvas(cpu, cpu.arg32(1));
    const auto found = state(cpu).canvases.find(cpu.arg32(1));
    const uint32_t fields[] = {found != state(cpu).canvases.end() && !found->second.window ? static_cast<uint32_t>(found->second.x) : 0,
                              found != state(cpu).canvases.end() && !found->second.window ? static_cast<uint32_t>(found->second.y) : 0,
                              surface ? static_cast<uint32_t>(surface->width) : 0,
                                    surface ? static_cast<uint32_t>(surface->height) : 0};
    cpu.write_u32(output, cpu.noop_vtable());
    cpu.write_mem(output + 4, fields, sizeof fields);
    cpu.set_ret32(output);
}
void bitmap_width(Cpu& cpu) { const auto* s = bitmap(cpu, cpu.arg32(0)); cpu.set_ret32(s ? s->width : 0); }
void bitmap_height(Cpu& cpu) { const auto* s = bitmap(cpu, cpu.arg32(0)); cpu.set_ret32(s ? s->height : 0); }

uint32_t rgb565(uint16_t color) {
    const uint32_t r = (color >> 11) & 31, g = (color >> 5) & 63, b = color & 31;
    return 0xff000000 | (((r << 3) | (r >> 2)) << 16) |
           (((g << 2) | (g >> 4)) << 8) | (b << 3) | (b >> 2);
}
uint32_t read_pixel(const uint8_t* pixels, const hle::GraphicsSurface& surface, int x, int y) {
    const uint8_t* pixel = pixels + size_t(y) * surface.pitch + size_t(x) * (surface.format == 1 ? 2 : 4);
    if (surface.format == 1) { uint16_t color; std::memcpy(&color, pixel, 2); return rgb565(color); }
    if (surface.format == 3)
        return (uint32_t(pixel[3]) << 24) | (uint32_t(pixel[0]) << 16) | (uint32_t(pixel[1]) << 8) | pixel[2];
    uint32_t color; std::memcpy(&color, pixel, 4); return color;
}
uint32_t mask_rgb(const hle::GraphicsSurface& surface) {
    if (surface.format != 1) return surface.mask & 0xffffff;
    return rgb565(static_cast<uint16_t>(((surface.mask >> 8) & 0xf800) |
        ((surface.mask >> 5) & 0x7e0) | ((surface.mask >> 3) & 31))) & 0xffffff;
}
uint32_t over(uint32_t source, uint32_t destination) {
    const uint32_t alpha = source >> 24;
    if (!alpha) return destination;
    if (alpha == 255) return source;
    uint32_t result = (alpha + ((destination >> 24) * (255 - alpha) + 127) / 255) << 24;
    for (unsigned shift : {0u, 8u, 16u})
        result |= ((((source >> shift) & 255) * alpha +
                    ((destination >> shift) & 255) * (255 - alpha) + 127) / 255) << shift;
    return result;
}

uint32_t blit(Cpu& cpu, hle::GraphicsSurface& destination, const Rect& dest,
              hle::GraphicsSurface& source, const Rect& src, bool compose) {
    if (dest.width < 0 || dest.height < 0 || src.width < 0 || src.height < 0) return out_of_range;
    if (!dest.width || !dest.height || !src.width || !src.height) return 0;
    const int left = static_cast<int>(std::max<int64_t>(0, dest.x));
    const int top = static_cast<int>(std::max<int64_t>(0, dest.y));
    const int right = static_cast<int>(std::min<int64_t>(destination.width, int64_t(dest.x) + dest.width));
    const int bottom = static_cast<int>(std::min<int64_t>(destination.height, int64_t(dest.y) + dest.height));
    if (left >= right || top >= bottom || src.x >= source.width || src.y >= source.height ||
        int64_t(src.x) + src.width <= 0 || int64_t(src.y) + src.height <= 0) return out_of_range;
    uint32_t error = ensure_pixels(cpu, source);
    if (!error) error = ensure_pixels(cpu, destination);
    if (error) return error;
    const uint8_t* pixels = cpu.memory_ptr(source.pixels, source.pitch * source.height);
    if (!pixels) return invalid_state;
    std::vector<uint8_t> snapshot;
    if (source.pixels == destination.pixels) {
        snapshot.assign(pixels, pixels + size_t(source.pitch) * source.height);
        pixels = snapshot.data();
    }
    const uint32_t mask = mask_rgb(source);
    std::vector<uint32_t> line(right - left);
    for (int y = top; y < bottom; ++y) {
        const int64_t sy = int64_t(src.y) + (int64_t(y) - dest.y) * src.height / dest.height;
        if (sy < 0 || sy >= source.height) continue;
        const uint32_t address = destination.pixels + y * destination.pitch + left * 4;
        cpu.read_mem(address, line.data(), static_cast<uint32_t>(line.size() * 4));
        for (int x = left; x < right; ++x) {
            const int64_t sx = int64_t(src.x) + (int64_t(x) - dest.x) * src.width / dest.width;
            if (sx < 0 || sx >= source.width) continue;
            const uint32_t color = read_pixel(pixels, source, static_cast<int>(sx), static_cast<int>(sy));
            if (compose && source.masking && (color & 0xffffff) == mask) continue;
            line[x - left] = compose ? over(color, line[x - left]) : color;
        }
        cpu.write_mem(address, line.data(), static_cast<uint32_t>(line.size() * 4));
    }
    return 0;
}
void canvas_copy(Cpu& cpu) {
    auto* destination = canvas(cpu, cpu.arg32(0));
    auto* source = canvas(cpu, cpu.arg32(2));
    Rect dest{}, src{};
    if (!source || !destination || !rectangle(cpu, cpu.arg32(1), dest) ||
        !rectangle(cpu, cpu.arg32(3), src)) { cpu.set_ret32(invalid_arg); return; }
    cpu.set_ret32(blit(cpu, *destination, dest, *source, src, false));
}
void canvas_copy_point(Cpu& cpu) {
    auto* destination = canvas(cpu, cpu.arg32(0));
    auto* source = canvas(cpu, cpu.arg32(2));
    Rect src{}; int x = 0, y = 0;
    if (!source || !destination || !point(cpu, cpu.arg32(1), x, y) ||
        !rectangle(cpu, cpu.arg32(3), src)) { cpu.set_ret32(invalid_arg); return; }
    cpu.set_ret32(blit(cpu, *destination, {x, y, src.width, src.height}, *source, src, false));
}
void draw_bitmap(Cpu& cpu) {
    auto* destination = canvas(cpu, cpu.arg32(0));
    auto* source = bitmap(cpu, cpu.arg32(2));
    int x = 0, y = 0;
    if (!source || !destination || !point(cpu, cpu.arg32(1), x, y)) { cpu.set_ret32(invalid_arg); return; }
    cpu.set_ret32(blit(cpu, *destination, {x, y, source->width, source->height},
                      *source, {0, 0, source->width, source->height}, true));
}
void draw_bitmap_rect(Cpu& cpu) {
    auto* destination = canvas(cpu, cpu.arg32(0));
    auto* source = bitmap(cpu, cpu.arg32(2));
    Rect dest{};
    if (!source || !destination || !rectangle(cpu, cpu.arg32(1), dest)) { cpu.set_ret32(invalid_arg); return; }
    cpu.set_ret32(blit(cpu, *destination, dest, *source, {0, 0, source->width, source->height}, true));
}
void draw_bitmap_region(Cpu& cpu) {
    auto* destination = canvas(cpu, cpu.arg32(0));
    auto* source = bitmap(cpu, cpu.arg32(2));
    Rect dest{}, src{};
    if (!source || !destination || !rectangle(cpu, cpu.arg32(1), dest) ||
        !rectangle(cpu, cpu.arg32(3), src)) { cpu.set_ret32(invalid_arg); return; }
    cpu.set_ret32(blit(cpu, *destination, dest, *source, src, true));
}
void draw_bitmap_rotated(Cpu& cpu) {
    auto* destination = canvas(cpu, cpu.arg32(0));
    auto* source = bitmap(cpu, cpu.arg32(2));
    int x = 0, y = 0, px = 0, py = 0;
    if (!source || !destination || !point(cpu, cpu.arg32(1), x, y) ||
        !point(cpu, cpu.arg32(3), px, py)) { cpu.set_ret32(invalid_arg); return; }
    uint32_t error = ensure_pixels(cpu, *source);
    if (!error) error = ensure_pixels(cpu, *destination);
    if (error) { cpu.set_ret32(error); return; }
    const int degree = (static_cast<int32_t>(cpu.arg32(4)) % 360 + 360) % 360;
    double cosine = 0, sine = 0;
    switch (degree) {
    case 0: cosine = 1; break;
    case 90: sine = 1; break;
    case 180: cosine = -1; break;
    case 270: sine = -1; break;
    default: cosine = std::cos(degree * 3.14159265358979323846 / 180);
             sine = std::sin(degree * 3.14159265358979323846 / 180); break;
    }
    // Bada's positive angle is counterclockwise in screen coordinates. The
    // 90-degree draw at (0,319) maps a 320x240 framebuffer into a 240x320 canvas.
    // The destination point locates the source pivot after rotation. Negative
    // pivots let apps place UI bitmaps in a shared rotated coordinate system.
    const double anchor_x = x, anchor_y = y;
    double xmin = destination->width, ymin = destination->height, xmax = -1, ymax = -1;
    for (int sx : {0, source->width - 1}) for (int sy : {0, source->height - 1}) {
        const double dx = anchor_x + (double(sx) - px) * cosine + (double(sy) - py) * sine;
        const double dy = anchor_y - (double(sx) - px) * sine + (double(sy) - py) * cosine;
        xmin = std::min(xmin, dx); xmax = std::max(xmax, dx);
        ymin = std::min(ymin, dy); ymax = std::max(ymax, dy);
    }
    const int left = static_cast<int>(std::clamp(std::floor(xmin), 0.0, double(destination->width)));
    const int top = static_cast<int>(std::clamp(std::floor(ymin), 0.0, double(destination->height)));
    const int right = static_cast<int>(std::clamp(std::ceil(xmax) + 1, 0.0, double(destination->width)));
    const int bottom = static_cast<int>(std::clamp(std::ceil(ymax) + 1, 0.0, double(destination->height)));
    const uint8_t* pixels = cpu.memory_ptr(source->pixels, source->pitch * source->height);
    if (!pixels) { cpu.set_ret32(invalid_state); return; }
    const uint32_t mask = mask_rgb(*source);
    std::vector<uint32_t> line(std::max(0, right - left));
    for (int dy = top; dy < bottom; ++dy) {
        const uint32_t address = destination->pixels + dy * destination->pitch + left * 4;
        cpu.read_mem(address, line.data(), static_cast<uint32_t>(line.size() * 4));
        for (int dx = left; dx < right; ++dx) {
            const auto sx = std::lround((dx - anchor_x) * cosine - (dy - anchor_y) * sine + px);
            const auto sy = std::lround((dx - anchor_x) * sine + (dy - anchor_y) * cosine + py);
            if (sx < 0 || sy < 0 || sx >= source->width || sy >= source->height) continue;
            const uint32_t color = read_pixel(pixels, *source, static_cast<int>(sx), static_cast<int>(sy));
            if (source->masking && (color & 0xffffff) == mask) continue;
            line[dx - left] = over(color, line[dx - left]);
        }
        cpu.write_mem(address, line.data(), static_cast<uint32_t>(line.size() * 4));
    }
    cpu.set_ret32(0);
}
void canvas_clear(Cpu& cpu) {
    auto* surface = canvas(cpu, cpu.arg32(0));
    if (!surface) { cpu.set_ret32(invalid_state); return; }
    const uint32_t error = ensure_pixels(cpu, *surface);
    if (error) { cpu.set_ret32(error); return; }
    const std::vector<uint32_t> line(surface->width, surface->background);
    for (int y = 0; y < surface->height; ++y)
        cpu.write_mem(surface->pixels + y * surface->pitch, line.data(), static_cast<uint32_t>(line.size() * 4));
    cpu.set_ret32(0);
}
void canvas_background(Cpu& cpu) {
    auto* surface = canvas(cpu, cpu.arg32(0));
    if (surface) surface->background = cpu.read_u32(cpu.arg32(1) + 4);
    cpu.set_ret32(0);
}
void bitmap_mask(Cpu& cpu) {
    auto* surface = bitmap(cpu, cpu.arg32(0));
    if (!surface) { cpu.set_ret32(invalid_state); return; }
    const uint32_t color = cpu.arg32(1);
    surface->masking = color != 0;
    surface->mask = color ? cpu.read_u32(color + 4) : 0;
    cpu.set_ret32(0);
}

void color_store(Cpu& cpu, uint32_t object, uint32_t value) {
    cpu.write_u32(object, cpu.noop_vtable()); cpu.write_u32(object + 4, value);
}
void color_ctor(Cpu& cpu) { const auto o = cpu.arg32(0); color_store(cpu, o, 0xff000000); cpu.set_ret32(o); }
uint32_t color_components(Cpu& cpu) {
    return ((cpu.arg32(4) & 255) << 24) | ((cpu.arg32(1) & 255) << 16) |
           ((cpu.arg32(2) & 255) << 8) | (cpu.arg32(3) & 255);
}
void color_ctor_components(Cpu& cpu) { const auto o = cpu.arg32(0); color_store(cpu, o, color_components(cpu)); cpu.set_ret32(o); }
void color_ctor_rgb(Cpu& cpu) {
    const auto o = cpu.arg32(0);
    color_store(cpu, o, cpu.arg32(1) | (cpu.arg32(2) ? 0 : 0xff000000)); cpu.set_ret32(o);
}
void color_set_components(Cpu& cpu) { cpu.write_u32(cpu.arg32(0) + 4, color_components(cpu)); cpu.set_ret32(0); }
void color_set_rgb(Cpu& cpu) {
    cpu.write_u32(cpu.arg32(0) + 4, cpu.arg32(1) | (cpu.arg32(2) ? 0 : 0xff000000)); cpu.set_ret32(0);
}
void color_get_rgb(Cpu& cpu) { cpu.set_ret32(cpu.read_u32(cpu.arg32(0) + 4)); }
void point_ctor(Cpu& cpu) {
    const auto object = cpu.arg32(0);
    cpu.write_u32(object, cpu.noop_vtable());
    cpu.write_u32(object + 4, cpu.arg32(1)); cpu.write_u32(object + 8, cpu.arg32(2)); cpu.set_ret32(object);
}
void point_ctor_zero(Cpu& cpu) {
    const auto object = cpu.arg32(0);
    cpu.write_u32(object, cpu.noop_vtable()); cpu.write_u32(object + 4, 0); cpu.write_u32(object + 8, 0);
    cpu.set_ret32(object);
}
void point_position(Cpu& cpu) {
    const auto object = cpu.arg32(0);
    cpu.write_u32(object + 4, cpu.arg32(1)); cpu.write_u32(object + 8, cpu.arg32(2)); cpu.set_ret32(0);
}
void rectangle_size(Cpu& cpu) {
    const auto object = cpu.arg32(0);
    cpu.write_u32(object + 12, cpu.arg32(1)); cpu.write_u32(object + 16, cpu.arg32(2)); cpu.set_ret32(0);
}
void rectangle_contains(Cpu& cpu) {
    Rect rect{}; int x = 0, y = 0;
    cpu.set_ret32(rectangle(cpu, cpu.arg32(0), rect) && point(cpu, cpu.arg32(1), x, y) &&
        x >= rect.x && y >= rect.y && int64_t(x) < int64_t(rect.x) + rect.width &&
        int64_t(y) < int64_t(rect.y) + rect.height);
}

void decode(Cpu& cpu, bool scaled) {
    const auto format = cpu.arg32(2);
    if (format < 1 || format > 3) { cpu.set_ret32(0); return; }
    const std::string path = hle::resolve_package_path(hle::osp_string_get(cpu, cpu.arg32(1)));
    using SurfacePtr = std::unique_ptr<SDL_Surface, decltype(&SDL_FreeSurface)>;
    SurfacePtr image(IMG_Load(path.c_str()), SDL_FreeSurface);
    if (!image) { cpu.set_ret32(0); return; }
    const int width = scaled ? static_cast<int32_t>(cpu.arg32(3)) : image->w;
    const int height = scaled ? static_cast<int32_t>(cpu.arg32(4)) : image->h;
    const uint32_t object = cpu.alloc(12);
    if (!object) { cpu.set_ret32(0); return; }
    cpu.set_reg(UC_ARM_REG_R0, object); bitmap_ctor(cpu);
    auto& target = state(cpu).bitmaps[object];
    if (allocate(cpu, target, width, height, format)) { bitmap_delete(cpu); cpu.set_ret32(0); return; }
    const uint32_t sdl_format = format == 1 ? SDL_PIXELFORMAT_RGB565 :
                                format == 2 ? SDL_PIXELFORMAT_ARGB8888 : SDL_PIXELFORMAT_RGBA32;
    SurfacePtr converted(SDL_ConvertSurfaceFormat(image.get(), sdl_format, 0), SDL_FreeSurface);
    if (!converted) { cpu.set_reg(UC_ARM_REG_R0, object); bitmap_delete(cpu); cpu.set_ret32(0); return; }
    if (converted->w != width || converted->h != height) {
        SurfacePtr resized(SDL_CreateRGBSurfaceWithFormat(0, width, height, format == 1 ? 16 : 32, sdl_format), SDL_FreeSurface);
        SDL_SetSurfaceBlendMode(converted.get(), SDL_BLENDMODE_NONE);
        if (!resized || SDL_BlitScaled(converted.get(), nullptr, resized.get(), nullptr) != 0) {
            cpu.set_reg(UC_ARM_REG_R0, object); bitmap_delete(cpu); cpu.set_ret32(0); return;
        }
        converted = std::move(resized);
    }
    const uint32_t row_bytes = width * (format == 1 ? 2 : 4);
    for (int y = 0; y < height; ++y)
        cpu.write_mem(target.pixels + y * target.pitch,
            static_cast<const uint8_t*>(converted->pixels) + y * converted->pitch, row_bytes);
    cpu.set_ret32(object);
}
void image_decode(Cpu& cpu) { decode(cpu, false); }
void image_decode_scaled(Cpu& cpu) { decode(cpu, true); }
void image_ctor(Cpu& cpu) {
    const auto object = cpu.arg32(0);
    cpu.write_u32(object, cpu.make_vtable(4, {{0, ok}, {1, delete_object}}));
    cpu.set_ret32(object);
}

} // namespace

namespace hle {
void resize_frame_canvas(Cpu& cpu, int width, int height) {
    auto& frame = state(cpu).frame;
    if (frame.width == width && frame.height == height) return;
    cpu.free(frame.pixels); frame.pixels = 0;
    frame.width = width; frame.height = height; frame.pitch = width * 4;
}
uint32_t make_frame_canvas(Cpu& cpu, int width, int height) {
    resize_frame_canvas(cpu, width, height);
    const uint32_t object = cpu.alloc(12);
    if (!object) return 0;
    // GetCanvasN returns a caller-owned wrapper over the same screen storage.
    cpu.set_reg(UC_ARM_REG_R0, object); canvas_ctor(cpu);
    state(cpu).canvases[object].window = true;
    return object;
}

void register_graphics() {
    for (const char* name : {"_ZN3Osp8Graphics6CanvasC1Ev", "_ZN3Osp8Graphics6CanvasC2Ev"}) register_handler(name, canvas_ctor);
    register_handler("_ZN3Osp8Graphics6CanvasD0Ev", canvas_delete);
    for (const char* name : {"_ZN3Osp8Graphics6CanvasD1Ev", "_ZN3Osp8Graphics6CanvasD2Ev"}) register_handler(name, canvas_destroy);
    register_handler("_ZN3Osp8Graphics6Canvas9ConstructERKNS0_9RectangleE", canvas_construct);
    register_handler("_ZN3Osp8Graphics6Canvas9ConstructEv", canvas_construct_screen);
    register_handler("_ZN3Osp8Graphics6Canvas4LockERNS0_10BufferInfoEl", canvas_lock);
    register_handler("_ZN3Osp8Graphics6Canvas6UnlockEv", ok);
    register_handler("_ZN3Osp8Graphics6Canvas4ShowEv", canvas_show);
    register_handler("_ZNK3Osp8Graphics6Canvas9GetBoundsEv", canvas_bounds);
    register_handler("_ZN3Osp8Graphics6Canvas5ClearEv", canvas_clear);
    register_handler("_ZN3Osp8Graphics6Canvas18SetBackgroundColorERKNS0_5ColorE", canvas_background);
    register_handler("_ZN3Osp8Graphics6Canvas4CopyERKNS0_9RectangleERKS1_S4_", canvas_copy);
    register_handler("_ZN3Osp8Graphics6Canvas4CopyERKNS0_5PointERKS1_RKNS0_9RectangleE", canvas_copy_point);
    register_handler("_ZN3Osp8Graphics6Canvas10DrawBitmapERKNS0_5PointERKNS0_6BitmapE", draw_bitmap);
    register_handler("_ZN3Osp8Graphics6Canvas10DrawBitmapERKNS0_9RectangleERKNS0_6BitmapE", draw_bitmap_rect);
    register_handler("_ZN3Osp8Graphics6Canvas10DrawBitmapERKNS0_9RectangleERKNS0_6BitmapES4_", draw_bitmap_region);
    register_handler("_ZN3Osp8Graphics6Canvas10DrawBitmapERKNS0_5PointERKNS0_6BitmapES4_i", draw_bitmap_rotated);
    for (const char* name : {"_ZN3Osp8Graphics6BitmapC1Ev", "_ZN3Osp8Graphics6BitmapC2Ev"}) register_handler(name, bitmap_ctor);
    register_handler("_ZN3Osp8Graphics6BitmapD0Ev", bitmap_delete);
    for (const char* name : {"_ZN3Osp8Graphics6BitmapD1Ev", "_ZN3Osp8Graphics6BitmapD2Ev"}) register_handler(name, bitmap_destroy);
    register_handler("_ZN3Osp8Graphics6Bitmap9ConstructERKNS0_9DimensionENS0_17BitmapPixelFormatE", bitmap_construct);
    register_handler("_ZN3Osp8Graphics6Bitmap4LockERNS0_10BufferInfoEl", bitmap_lock);
    register_handler("_ZN3Osp8Graphics6Bitmap6UnlockEv", ok);
    register_handler("_ZNK3Osp8Graphics6Bitmap8GetWidthEv", bitmap_width);
    register_handler("_ZNK3Osp8Graphics6Bitmap9GetHeightEv", bitmap_height);
    register_handler("_ZN3Osp8Graphics6Bitmap15SetMaskingColorEPKNS0_5ColorE", bitmap_mask);
    for (const char* name : {"_ZN3Osp8Graphics10BufferInfoC1Ev", "_ZN3Osp8Graphics10BufferInfoC2Ev"}) register_handler(name, info_ctor);
    register_handler("_ZN3Osp8Graphics10BufferInfoD1Ev", ok);
    register_handler("_ZN3Osp8Graphics10BufferInfoD2Ev", ok);
    register_handler("_ZN3Osp8Graphics5ColorC1Ev", color_ctor);
    register_handler("_ZN3Osp8Graphics5ColorC2Ev", color_ctor);
    register_handler("_ZN3Osp8Graphics5ColorC1Ehhhh", color_ctor_components);
    register_handler("_ZN3Osp8Graphics5ColorC2Ehhhh", color_ctor_components);
    register_handler("_ZN3Osp8Graphics5ColorC1Ejb", color_ctor_rgb);
    register_handler("_ZN3Osp8Graphics5ColorC2Ejb", color_ctor_rgb);
    register_handler("_ZN3Osp8Graphics5Color18SetColorComponentsEhhhh", color_set_components);
    register_handler("_ZN3Osp8Graphics5Color8SetRGB32Ejb", color_set_rgb);
    register_handler("_ZNK3Osp8Graphics5Color8GetRGB32Ev", color_get_rgb);
    register_handler("_ZN3Osp8Graphics5ColorD1Ev", ok);
    register_handler("_ZN3Osp8Graphics5ColorD2Ev", ok);
    for (const char* name : {"_ZN3Osp8Graphics5PointC1Eii", "_ZN3Osp8Graphics5PointC2Eii",
                             "_ZN3Osp8Graphics9DimensionC1Eii", "_ZN3Osp8Graphics9DimensionC2Eii"}) register_handler(name, point_ctor);
    for (const char* name : {"_ZN3Osp8Graphics5PointC1Ev", "_ZN3Osp8Graphics5PointC2Ev"}) register_handler(name, point_ctor_zero);
    register_handler("_ZN3Osp8Graphics5Point11SetPositionEii", point_position);
    register_handler("_ZN3Osp8Graphics9Rectangle11SetPositionEii", point_position);
    register_handler("_ZN3Osp8Graphics9Rectangle7SetSizeEii", rectangle_size);
    register_handler("_ZNK3Osp8Graphics9Rectangle8ContainsERKNS0_5PointE", rectangle_contains);
    for (const char* name : {"_ZN3Osp8Graphics5PointD1Ev", "_ZN3Osp8Graphics5PointD2Ev",
                             "_ZN3Osp8Graphics9DimensionD1Ev", "_ZN3Osp8Graphics9DimensionD2Ev"}) register_handler(name, ok);
    for (const char* name : {"_ZN3Osp5Media5ImageC1Ev", "_ZN3Osp5Media5ImageC2Ev"}) register_handler(name, image_ctor);
    register_handler("_ZN3Osp5Media5Image9ConstructEv", ok);
    register_handler("_ZNK3Osp5Media5Image7DecodeNERKNS_4Base6StringENS_8Graphics17BitmapPixelFormatE", image_decode);
    register_handler("_ZNK3Osp5Media5Image7DecodeNERKNS_4Base6StringENS_8Graphics17BitmapPixelFormatEii", image_decode_scaled);
}
} // namespace hle
