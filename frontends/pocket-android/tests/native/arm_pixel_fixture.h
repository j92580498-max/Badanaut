#pragma once
#include <cstdint>

// Generated ARM fixture: paired RGB565 loads, five-bit channel expansion,
// duplicated stores, source-row padding and saved registers. It is executed
// directly to verify native pixel-copy and GPU presentation independently.
inline constexpr uint32_t arm_pixel_fixture[] = {
    0xe92d5ff0, 0xe3a0c01f, 0xe3a04078, 0xe4905004,
    0xe2544001, 0xe00c6005, 0xe00c7325, 0xe00c85a5,
    0xe1a09186, 0xe0899587, 0xe0899988, 0xe00c6825,
    0xe00c7b25, 0xe00c8da5, 0xe1a0a186, 0xe08aa587,
    0xe08aa988, 0xe5819000, 0xe5819004, 0xe581a008,
    0xe581a00c, 0xe5819780, 0xe5819784, 0xe581a788,
    0xe581a78c, 0xe2811010, 0x1affffe7, 0xe2522001,
    0xe2800020, 0xe2811d1e, 0x1affffe2, 0xe8bd9ff0,
};
