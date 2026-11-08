#pragma once
#include <cstdint>

// Guest (emulated ARM) address-space layout.
namespace guest {

// HLE trampoline page: each imported symbol is bound to one 4-byte slot here.
// Slot contents = `bx lr` (0xe12fff1e); a UC_HOOK_CODE over this range dispatches
// to the host handler BEFORE the bx lr runs, so the handler sets r0/VFP and the
// bx lr returns to the guest caller. Slot 0 is the "host return" sentinel: when
// PC reaches it, emulation of a host-initiated guest call has finished.
constexpr uint32_t HLE_BASE = 0x40000000;
constexpr uint32_t HLE_SIZE = 0x00100000; // 256K slots
constexpr uint32_t HLE_RETURN = HLE_BASE; // slot 0

// Data-import stubs (typeinfo/vtable/__dso_handle/__App_info) point here.
constexpr uint32_t HLE_DATA_BASE = 0x41000000;
constexpr uint32_t HLE_DATA_SIZE = 0x00100000;

// Tiny ARM runtime helpers execute in Unicorn's JIT, outside the host dispatch
// hook. Frequent integer arithmetic must not cross the C++ boundary per call.
constexpr uint32_t ABI_HELPER_BASE = 0x42000000;
constexpr uint32_t ABI_HELPER_SIZE = 0x00001000;
constexpr uint32_t ABI_HELPER_STRIDE = 32;

// Second image (the guest libstdc++.so). Its own vaddrs start at 0, so it loads
// at this bias, clear of the app image (which sits below ~0x400000).
constexpr uint32_t LIB_BASE = 0x10000000;

// Guest stack (grows down).
constexpr uint32_t STACK_TOP = 0x70000000;
constexpr uint32_t STACK_SIZE = 0x00800000; // 8 MB

// Guest heap for Osp_allocEx / malloc / new. TLSF reuses freed blocks inside
// this host-owned pool, which is shared with Unicorn through uc_mem_map_ptr.
constexpr uint32_t HEAP_BASE = 0x80000000;
constexpr uint32_t HEAP_SIZE = 0x20000000; // 512 MB

inline uint32_t tramp_addr(uint32_t import_index) {
    return HLE_BASE + (import_index + 1) * 4; // +1: slot 0 reserved
}
inline bool in_hle(uint32_t addr) {
    return addr >= HLE_BASE && addr < HLE_BASE + HLE_SIZE;
}
inline uint32_t tramp_index(uint32_t addr) {
    return (addr - HLE_BASE) / 4 - 1;
}

} // namespace guest
