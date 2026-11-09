# TLSF

Source: https://github.com/mattconte/tlsf

Pinned commit: `deff9ab509341f264addbd3c8ada533678591905`.

`tlsf.c` and `tlsf.h` are unmodified upstream files. They provide the existing
constant-time pool allocator used by the Bada guest heap, including aligned
allocation, splitting, freeing and coalescing. The pool is shared with Unicorn
through its documented `uc_mem_map_ptr` API; guest addresses remain ARM32 values.

License: BSD 3-Clause, retained in `tlsf.h` and reproduced in `LICENSE.md`.
