#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>

enum class ArmFloatAbi { unknown, soft, hard, compatible };

inline const char* arm_float_abi_name(ArmFloatAbi abi) {
    switch (abi) {
    case ArmFloatAbi::soft: return "soft";
    case ArmFloatAbi::hard: return "hard (VFP)";
    case ArmFloatAbi::compatible: return "compatible";
    default: return "unknown";
    }
}

// AAELF32 file attributes describe the calling convention even when Samsung's
// ELF header omits EF_ARM_ABI_FLOAT_*. An absent Tag_ABI_VFP_args in an aeabi
// file scope has value zero (base AAPCS), independently of the bada API version.
inline ArmFloatAbi arm_attributes_float_abi(const uint8_t* data, size_t size) {
    auto invalid = []() { throw std::runtime_error("invalid ARM attributes"); };
    if (!size || data[0] != 'A') invalid();
    auto word = [&](size_t offset, size_t end) {
        if (offset > end || end - offset < 4) invalid();
        return uint32_t(data[offset]) | (uint32_t(data[offset + 1]) << 8) |
               (uint32_t(data[offset + 2]) << 16) | (uint32_t(data[offset + 3]) << 24);
    };
    auto uleb = [&](size_t& offset, size_t end) {
        uint64_t value = 0;
        for (unsigned shift = 0; shift < 35; shift += 7) {
            if (offset >= end) break;
            const uint8_t byte = data[offset++];
            value |= uint64_t(byte & 127) << shift;
            if (!(byte & 128) && value <= UINT32_MAX) return uint32_t(value);
        }
        invalid();
        return uint32_t(0);
    };
    auto string_end = [&](size_t offset, size_t end) {
        while (offset < end) if (!data[offset++]) return offset;
        invalid();
        return end;
    };
    ArmFloatAbi abi = ArmFloatAbi::unknown;
    size_t offset = 1;
    while (offset < size) {
        const size_t length = word(offset, size);
        if (length <= 4 || length > size - offset) invalid();
        const size_t end = offset + length;
        const size_t vendor = offset + 4, vendor_end = string_end(vendor, end);
        if (vendor_end - vendor != 6 || std::memcmp(data + vendor, "aeabi", 6)) {
            offset = end;
            continue;
        }
        offset = vendor_end;
        while (offset < end) {
            const size_t start = offset;
            const uint32_t scope = uleb(offset, end);
            const size_t scope_length = word(offset, end);
            offset += 4;
            if (scope_length < offset - start || scope_length > end - start) invalid();
            const size_t scope_end = start + scope_length;
            if (scope == 1) {
                ArmFloatAbi current = ArmFloatAbi::soft;
                while (offset < scope_end) {
                    const uint32_t tag = uleb(offset, scope_end);
                    if (tag == 4 || tag == 5 || (tag >= 32 && (tag & 1))) {
                        offset = string_end(offset, scope_end);
                    } else {
                        const uint32_t value = uleb(offset, scope_end);
                        if (tag == 28) {
                            current = value == 0 ? ArmFloatAbi::soft : value == 1 ? ArmFloatAbi::hard :
                                      value == 3 ? ArmFloatAbi::compatible : ArmFloatAbi::unknown;
                        }
                        if (tag == 32) offset = string_end(offset, scope_end);
                    }
                }
                if (abi != ArmFloatAbi::unknown && abi != current) invalid();
                abi = current;
            }
            offset = scope_end;
        }
        offset = end;
    }
    return abi;
}
