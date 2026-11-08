# Unicorn for Badanaut

CMake builds Unicorn 2.1.4 directly from upstream commit
`8028ec436f2d9376525352dd38ed9ed6b9f6be10`. This is the CPU release formerly
provided by `unicorn-engine-sys` 2.1.5; Rust is no longer part of the build.
Only ARM32 guest emulation is enabled. Android host ABIs remain ARM64 and ARMv7.

The source archive is downloaded on first configuration and checked against
SHA-256 `9d2a2003f0307d0fc239af439654564fe6ef17b67f7c5a39b45fad4c69d6aec2`.
CMake's standard `FETCHCONTENT_SOURCE_DIR_UNICORN_SOURCE` override can point to
an already unpacked matching source tree for offline builds.

Upstream: https://github.com/unicorn-engine/unicorn

Keep the upstream license notices: `COPYING`, `COPYING.LGPL2` and `COPYING_GLIB`.
