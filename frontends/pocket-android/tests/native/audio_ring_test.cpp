#include "audio_ring.h"
#include <cstdio>
#include <cstdlib>

static void require(bool condition, const char* message) {
    if (!condition) { std::fprintf(stderr, "%s\n", message); std::exit(1); }
}
static bool write(PcmRing& ring, const int16_t* samples, size_t frames) {
    return ring.append(frames, [&](int16_t* target, size_t offset, size_t count) {
        std::memcpy(target, samples + offset, count * sizeof(int16_t));
    });
}
int main() {
    PcmRing ring;
    ring.prepare(3, 2);
    const int16_t a[] = {1, 2, 3, 4}, b[] = {5, 6, 7, 8, 9, 10};
    require(write(ring, a, 2), "initial stereo write failed");
    require(!write(ring, a, 2) && ring.sample(0) == 1, "backpressure corrupted queued samples");
    require(!ring.consume() && ring.consume() && ring.empty(), "buffer completion count changed");
    require(write(ring, b, 3), "wrapped write failed");
    require(ring.sample(0) == 5 && ring.sample(1, true) == 8, "wrapped interpolation samples changed");
    require(!ring.consume() && ring.sample(0) == 7, "first wrapped frame changed");
    require(!ring.consume() && ring.sample(1, true) == 10, "end interpolation read stale storage");
    require(ring.consume() && ring.empty(), "wrapped buffer completion was lost");
    ring.prepare(4, 1);
    const int16_t c[] = {11}, d[] = {12, 13};
    require(write(ring, c, 1) && write(ring, d, 2), "adjacent buffers failed");
    require(ring.sample(1, true) == 12 && ring.consume(), "mono next-buffer interpolation changed");
    ring.clear();
    require(write(ring, d, 2) && !ring.consume() && ring.consume(), "reset retained stale completion markers");
    puts("PCM wrap, interpolation, completions and backpressure passed");
}
