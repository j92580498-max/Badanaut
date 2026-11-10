#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

class Cpu;

// Accelerates a recognized ARM soft-float dot product. Candidates and code
// snapshots belong to the CPU; neither game addresses nor game files are used.
class MathAccelerator {
  public:
    explicit MathAccelerator(Cpu& cpu);
    void allocated(uint32_t address, uint32_t bytes);
    void freed(uint32_t address);
    void written(uint32_t address, uint32_t bytes);
    void scan(); // only between Unicorn runs, with a bounded scan budget
    size_t installed() const { return kernels_.size(); }
    bool install(uint32_t address); // already loaded region, also used by probes
    static bool evaluate(const float vector[4], const float matrix[13], float& result);

  private:
    struct Candidate { uint32_t address, bytes, offset = 0; unsigned pass = 0; uint64_t ready; };
    struct Kernel {
        uint32_t address, add, multiply, subtract;
        bool valid = true;
        std::array<uint8_t, 100> code;
        std::array<uint8_t, 196> add_code;
        std::array<uint8_t, 208> multiply_code;
        std::array<uint8_t, 280> subtract_code;
    };
    Cpu& cpu_;
    bool enabled_;
    std::vector<Candidate> candidates_;
    std::vector<Kernel> kernels_;
    static bool run(Cpu& cpu);
    bool execute(uint32_t address);
};
