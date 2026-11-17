#define SDL_MAIN_HANDLED
#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#include "cpu.h"
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <vector>

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
uint32_t call(Cpu& cpu, const char* name, std::initializer_list<uint32_t> args = {}) {
    unsigned index = 0;
    for (const auto value : args) {
        if (index < 4) cpu.set_reg(UC_ARM_REG_R0 + index, value);
        else cpu.write_u32(cpu.reg(UC_ARM_REG_SP) + (index - 4) * 4, value);
        ++index;
    }
    hle::call(name, cpu);
    return cpu.reg(UC_ARM_REG_R0);
}
uint32_t rect(Cpu& cpu, int x, int y, int width, int height) {
    const auto object = cpu.alloc(24);
    cpu.write_u32(object + 20, 0x12345678);
    call(cpu, "_ZN3Osp8Graphics9RectangleC1Eiiii", {object, uint32_t(x), uint32_t(y), uint32_t(width), uint32_t(height)});
    require(cpu.read_u32(object + 20) == 0x12345678, "Rectangle constructor overwrote adjacent storage");
    return object;
}
uint32_t point(Cpu& cpu, int x, int y) {
    const auto object = cpu.alloc(16);
    cpu.write_u32(object + 12, 0x87654321);
    call(cpu, "_ZN3Osp8Graphics5PointC1Eii", {object, uint32_t(x), uint32_t(y)});
    require(cpu.read_u32(object + 12) == 0x87654321, "Point constructor overwrote adjacent storage");
    return object;
}
uint32_t canvas(Cpu& cpu, int width, int height, int x = 0, int y = 0) {
    const auto object = cpu.alloc(12), bounds = rect(cpu, x, y, width, height);
    call(cpu, "_ZN3Osp8Graphics6CanvasC1Ev", {object});
    require(call(cpu, "_ZN3Osp8Graphics6Canvas9ConstructERKNS0_9RectangleE", {object, bounds}) == 0,
            "memory canvas could not be constructed");
    cpu.free(bounds);
    return object;
}
uint32_t bitmap(Cpu& cpu, int width, int height, uint32_t format) {
    const auto object = cpu.alloc(12), dimensions = point(cpu, width, height);
    call(cpu, "_ZN3Osp8Graphics6BitmapC1Ev", {object});
    require(call(cpu, "_ZN3Osp8Graphics6Bitmap9ConstructERKNS0_9DimensionENS0_17BitmapPixelFormatE",
                 {object, dimensions, format}) == 0, "bitmap could not be constructed");
    cpu.free(dimensions);
    return object;
}
struct Buffer { uint32_t pixels; int width, height, pitch; uint32_t format; };
Buffer lock(Cpu& cpu, uint32_t object, bool image = false) {
    const auto info = cpu.alloc(36);
    call(cpu, "_ZN3Osp8Graphics10BufferInfoC1Ev", {info});
    cpu.write_u32(info + 28, 0xdeadbeef);
    require(call(cpu, image ? "_ZN3Osp8Graphics6Bitmap4LockERNS0_10BufferInfoEl" :
                             "_ZN3Osp8Graphics6Canvas4LockERNS0_10BufferInfoEl", {object, info, UINT32_MAX}) == 0,
            "pixel buffer could not be locked");
    Buffer result{cpu.read_u32(info + 24), int(cpu.read_u32(info + 4)), int(cpu.read_u32(info + 8)),
                  int(cpu.read_u32(info + 12)), cpu.read_u32(info + 20)};
    require(cpu.read_u32(info + 16) == (result.format == 1 ? 16 : 32) &&
            cpu.read_u32(info + 28) == 0xdeadbeef && result.pixels,
            "lock descriptor has the wrong format or overwrote its public fields");
    call(cpu, image ? "_ZN3Osp8Graphics6Bitmap6UnlockEv" : "_ZN3Osp8Graphics6Canvas6UnlockEv", {object});
    cpu.free(info);
    return result;
}
void destroy(Cpu& cpu, uint32_t object) {
    const auto pixels = cpu.graphics->bitmaps.count(object) ? lock(cpu, object, true).pixels : 0;
    uint32_t result = 1;
    const auto deleting_destructor = cpu.read_u32(cpu.read_u32(object) + 4);
    require(cpu.run_guest(deleting_destructor, {object}, result) && result == 0 &&
            cpu.allocation_size(object) == 0 && (!pixels || cpu.allocation_size(pixels) == 0),
            "virtual destruction retained a bitmap object or its pixel buffer");
}

void frame_test(Cpu& cpu) {
    const auto app = cpu.make_stub_object(32);
    const auto frame = call(cpu, "_ZNK3Osp3App11Application11GetAppFrameEv", {app});
    const auto getter = cpu.read_u32(cpu.read_u32(frame) + 8);
    uint32_t first = 0, second = 0;
    require(cpu.run_guest(getter, {frame}, first) && first &&
            cpu.run_guest(getter, {frame}, second) && second && first != second,
            "IAppFrame::GetCanvasN did not return caller-owned screen canvases");
    const auto a = lock(cpu, first), b = lock(cpu, second);
    require(a.width == 480 && a.height == 800 && a.pitch == 1920 && a.pixels == b.pixels,
            "screen canvases do not share the portrait frame buffer");
    cpu.write_u32(a.pixels, 0xff13579b);
    destroy(cpu, first);
    require(cpu.read_u32(lock(cpu, second).pixels) == 0xff13579b,
            "destroying one screen canvas cleared another canvas's pixels");
    const auto memory = canvas(cpu, 2, 3, 7, 11);
    const auto storage = lock(cpu, memory);
    const auto bounds = cpu.alloc(24);
    cpu.write_u32(bounds + 20, 0xcafebabe);
    require(call(cpu, "_ZNK3Osp8Graphics6Canvas9GetBoundsEv", {bounds, memory}) == bounds &&
            cpu.read_u32(bounds + 4) == 7 && cpu.read_u32(bounds + 8) == 11 &&
            cpu.read_u32(bounds + 12) == 2 && cpu.read_u32(bounds + 16) == 3 &&
            cpu.read_u32(bounds + 20) == 0xcafebabe, "Canvas bounds lost the requested rectangle or the value-return ABI");
    cpu.write_u32(storage.pixels, 0xffabcdef);
    call(cpu, "_ZN3Osp2Ui8Controls5Frame14SetOrientationENS0_11OrientationE", {0, 2});
    const auto rotated = lock(cpu, second);
    require(rotated.width == 800 && rotated.height == 480 && rotated.pitch == 3200 &&
            cpu.read_u32(storage.pixels) == 0xffabcdef && lock(cpu, memory).pixels == storage.pixels,
            "frame orientation resized or cleared an independent memory canvas");
    {
        Cpu other;
        std::string error;
        require(other.init(*cpu.img, error), error.c_str());
        const auto independent = canvas(other, 2, 3);
        other.write_u32(lock(other, independent).pixels, 0xff112233);
        require(cpu.read_u32(storage.pixels) == 0xffabcdef,
                "another guest process changed a live canvas's state");
        destroy(other, independent);
    }
    cpu.free(bounds); destroy(cpu, memory); destroy(cpu, second); cpu.free(app);
}

void rotation_test(Cpu& cpu) {
    const auto image = bitmap(cpu, 3, 2, 1);
    const auto input = lock(cpu, image, true);
    const uint16_t colors[] = {0xf800, 0x07e0, 0x001f, 0xffff, 0xf81f, 0x07ff};
    cpu.write_mem(input.pixels, colors, 6);
    cpu.write_mem(input.pixels + input.pitch, colors + 3, 6);
    const auto rotated = canvas(cpu, 2, 3);
    const auto output = lock(cpu, rotated);
    const auto origin = point(cpu, 0, 2), pivot = point(cpu, 0, 0);
    require(call(cpu, "_ZN3Osp8Graphics6Canvas10DrawBitmapERKNS0_5PointERKNS0_6BitmapES4_i",
                 {rotated, origin, image, pivot, 90}) == 0, "rotated bitmap draw failed");
    const uint32_t expected[] = {0xff0000ff, 0xff00ffff, 0xff00ff00, 0xffff00ff, 0xffff0000, 0xffffffff};
    for (int y = 0; y < 3; ++y) for (int x = 0; x < 2; ++x)
        require(cpu.read_u32(output.pixels + y * output.pitch + x * 4) == expected[y * 2 + x],
                "RGB565 rotation has the wrong direction, row pitch, or color expansion");
    // Zenonia places UI bitmaps through a negative pivot in its unrotated
    // screen coordinates. The pivot must land on the destination point.
    const auto positioned = canvas(cpu, 3, 5), anchor = point(cpu, 0, 4), offset = point(cpu, -1, -1);
    require(call(cpu, "_ZN3Osp8Graphics6Canvas10DrawBitmapERKNS0_5PointERKNS0_6BitmapES4_i",
                 {positioned, anchor, image, offset, 90}) == 0, "positioned bitmap rotation failed");
    const auto placed = lock(cpu, positioned);
    for (int y = 0; y < 5; ++y) for (int x = 0; x < 3; ++x) {
        const uint32_t color = x >= 1 && y >= 1 && y <= 3 ? expected[(y - 1) * 2 + x - 1] : 0;
        require(cpu.read_u32(placed.pixels + y * placed.pitch + x * 4) == color,
                "a nonzero source pivot displaced or clipped a rotated UI bitmap");
    }
    const auto scaled = canvas(cpu, 4, 6), dest = rect(cpu, 0, 0, 4, 6), src = rect(cpu, 0, 0, 2, 3);
    require(call(cpu, "_ZN3Osp8Graphics6Canvas4CopyERKNS0_9RectangleERKS1_S4_", {scaled, dest, rotated, src}) == 0,
            "scaled canvas copy failed");
    const auto copied = lock(cpu, scaled);
    require(copied.pixels != output.pixels && output.pixels != input.pixels,
            "a bitmap or memory canvas reused another object's storage");
    for (int y = 0; y < 6; ++y) for (int x = 0; x < 4; ++x)
        require(cpu.read_u32(copied.pixels + y * copied.pitch + x * 4) == expected[(y / 2) * 2 + x / 2],
                "scaled copy changed the source rectangle's pixels");
    // Copy overlapping regions of the same canvas from a snapshot, including
    // vertical overlap that a row-at-a-time source read would overwrite.
    const auto lower = rect(cpu, 0, 1, 4, 5), upper = rect(cpu, 0, 0, 4, 5);
    require(call(cpu, "_ZN3Osp8Graphics6Canvas4CopyERKNS0_9RectangleERKS1_S4_", {scaled, lower, scaled, upper}) == 0,
            "overlapping canvas copy failed");
    for (int y = 1; y < 6; ++y) for (int x = 0; x < 4; ++x)
        require(cpu.read_u32(copied.pixels + y * copied.pitch + x * 4) == expected[((y - 1) / 2) * 2 + x / 2],
                "overlapping copy consumed a row it had already overwritten");
    for (auto object : {origin, pivot, anchor, offset, dest, src, lower, upper}) cpu.free(object);
    destroy(cpu, image); destroy(cpu, rotated); destroy(cpu, positioned); destroy(cpu, scaled);
}

void masking_test(Cpu& cpu) {
    const auto image = bitmap(cpu, 3, 1, 2);
    const auto source = lock(cpu, image, true);
    const uint32_t colors[] = {0xffff0000, 0x8000ff00, 0x00ffffff};
    cpu.write_mem(source.pixels, colors, sizeof colors);
    const auto destination = canvas(cpu, 3, 1);
    const auto target = lock(cpu, destination);
    const auto color = cpu.alloc(12), origin = point(cpu, 0, 0);
    cpu.write_u32(color + 8, 0xfeedface);
    call(cpu, "_ZN3Osp8Graphics5ColorC1Ejb", {color, 0x000000ff, 0});
    require(call(cpu, "_ZNK3Osp8Graphics5Color8GetRGB32Ev", {color}) == 0xff0000ff &&
            cpu.read_u32(color + 8) == 0xfeedface, "Color packing lost opaque RGB or wrote past the value");
    call(cpu, "_ZN3Osp8Graphics6Canvas18SetBackgroundColorERKNS0_5ColorE", {destination, color});
    call(cpu, "_ZN3Osp8Graphics6Canvas5ClearEv", {destination});
    call(cpu, "_ZN3Osp8Graphics5Color18SetColorComponentsEhhhh", {color, 255, 0, 0, 255});
    call(cpu, "_ZN3Osp8Graphics6Bitmap15SetMaskingColorEPKNS0_5ColorE", {image, color});
    require(call(cpu, "_ZN3Osp8Graphics6Canvas10DrawBitmapERKNS0_5PointERKNS0_6BitmapE", {destination, origin, image}) == 0 &&
            cpu.read_u32(target.pixels) == 0xff0000ff && cpu.read_u32(target.pixels + 4) == 0xff00807f &&
            cpu.read_u32(target.pixels + 8) == 0xff0000ff, "masked or transparent UI bitmap pixels overwrote the background");
    call(cpu, "_ZN3Osp8Graphics6Bitmap15SetMaskingColorEPKNS0_5ColorE", {image, 0});
    call(cpu, "_ZN3Osp8Graphics6Canvas5ClearEv", {destination});
    call(cpu, "_ZN3Osp8Graphics5Point11SetPositionEii", {origin, UINT32_MAX, 0});
    require(call(cpu, "_ZN3Osp8Graphics6Canvas10DrawBitmapERKNS0_5PointERKNS0_6BitmapE", {destination, origin, image}) == 0 &&
            cpu.read_u32(target.pixels) == 0xff00807f && cpu.read_u32(target.pixels + 8) == 0xff0000ff,
            "negative bitmap coordinates were not clipped at the canvas edge");
    cpu.free(color); cpu.free(origin); destroy(cpu, image); destroy(cpu, destination);
}

void image_test(Cpu& cpu, const std::filesystem::path& package) {
    SDL_Surface* image = SDL_CreateRGBSurfaceWithFormat(0, 2, 2, 32, SDL_PIXELFORMAT_ARGB8888);
    require(image, SDL_GetError());
    const uint32_t colors[] = {0xffff0000, 0x0000ff00, 0xff0000ff, 0x80ffffff};
    std::memcpy(image->pixels, colors, sizeof colors);
    const int saved = IMG_SavePNG(image, (package / "Res/Fixture.PNG").string().c_str());
    SDL_FreeSurface(image);
    require(saved == 0, IMG_GetError());
    const auto decoder = cpu.alloc(12), path = cpu.alloc(16);
    cpu.write_u32(decoder + 8, 0xfacefeed);
    call(cpu, "_ZN3Osp5Media5ImageC1Ev", {decoder});
    require(cpu.read_u32(decoder + 8) == 0xfacefeed, "Image constructor overwrote adjacent storage");
    hle::osp_string_set(cpu, path, "/res/fixture.png");
    constexpr const char* decode = "_ZNK3Osp5Media5Image7DecodeNERKNS_4Base6StringENS_8Graphics17BitmapPixelFormatE";
    const auto first = call(cpu, decode, {decoder, path, 2});
    require(first && call(cpu, "_ZNK3Osp8Graphics6Bitmap8GetWidthEv", {first}) == 2 &&
            call(cpu, "_ZNK3Osp8Graphics6Bitmap9GetHeightEv", {first}) == 2, "PNG decoding or case-insensitive package lookup failed");
    const auto pixels = lock(cpu, first, true);
    for (int y = 0; y < 2; ++y) for (int x = 0; x < 2; ++x)
        require(cpu.read_u32(pixels.pixels + y * pixels.pitch + x * 4) == colors[y * 2 + x],
                "decoded PNG lost its channels or alpha");
    const auto second = call(cpu, "_ZNK3Osp5Media5Image7DecodeNERKNS_4Base6StringENS_8Graphics17BitmapPixelFormatEii",
                             {decoder, path, 2, 4, 4});
    require(second && lock(cpu, second, true).width == 4 && lock(cpu, second, true).height == 4,
            "DecodeN did not honor its requested dimensions");
    const auto small = call(cpu, decode, {decoder, path, 1});
    require(small, "RGB565 PNG decoding failed");
    const auto raw = lock(cpu, small, true);
    uint16_t red = 0; cpu.read_mem(raw.pixels, &red, 2);
    require(raw.format == 1 && red == 0xf800 && raw.pixels != pixels.pixels, "decoded RGB565 bitmap has the wrong storage or channel order");
    hle::osp_string_set(cpu, path, "/Res/missing.png");
    require(call(cpu, decode, {decoder, path, 2}) == 0, "missing images did not return null");
    destroy(cpu, first); destroy(cpu, second); destroy(cpu, small);
    cpu.free(path);
    uint32_t result = 1;
    require(cpu.run_guest(cpu.read_u32(cpu.read_u32(decoder) + 4), {decoder}, result) &&
            cpu.allocation_size(decoder) == 0, "Image virtual destruction retained its wrapper");
}
} // namespace

int main() {
    const auto package = std::filesystem::temp_directory_path() /
        ("badanaut-graphics-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    try {
        SDL_SetMainReady();
        require(SDL_Init(0) == 0, SDL_GetError());
        std::filesystem::create_directories(package / "Res");
        ElfImage image;
        const uint32_t instruction = 0xe12fff1e;
        image.segments.push_back({0x100000, 4, 4096, 7, reinterpret_cast<const uint8_t*>(&instruction)});
        Cpu cpu;
        std::string error;
        require(cpu.init(image, error), error.c_str());
        cpu.set_reg(UC_ARM_REG_SP, cpu.reg(UC_ARM_REG_SP) - 256);
        hle::set_package_root(package.string());
        frame_test(cpu);
        hle::set_package_root(package.string());
        rotation_test(cpu);
        masking_test(cpu);
        image_test(cpu, package);
        std::filesystem::remove_all(package);
        SDL_Quit();
        puts("Screen canvases, isolated bitmaps, rotation, scaling, alpha, PNG decoding and cleanup passed");
        return 0;
    } catch (const std::exception& error) {
        std::error_code ignored;
        std::filesystem::remove_all(package, ignored);
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
