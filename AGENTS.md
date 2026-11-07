# Badanaut — project map

Badanaut contains two emulation paths:

- Android bada launcher: `frontends/pocket-android/`; build with `build-android.sh`.
- Samsung bada HLE engine: `third_party/bada-hle/`; read its `README.md` and `UPSTREAM.md` before changing it.
- PocketHLE Windows CE / Windows Mobile core: Rust crates in `crates/`; read `docs/AGENTS.md` before changing that behavior.

`THIRD_PARTY_NOTICES.md` describes component ownership and license boundaries. The game ZIP, Bada firmware runtime, generated JNI libraries, and APKs are local build inputs/outputs; do not commit them. Keep the upstream Bada attribution intact. Do not commit unless requested, and never push directly to the target branch—use a pull request.