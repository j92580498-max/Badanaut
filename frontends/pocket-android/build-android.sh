#!/usr/bin/env bash
set -euo pipefail

if [[ -f "$HOME/.cargo/env" ]]; then
    source "$HOME/.cargo/env"
fi

FRONTEND_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$FRONTEND_DIR/../.." && pwd)"
SDK_ROOT="${ANDROID_SDK_ROOT:-${ANDROID_HOME:-$HOME/Android/Sdk}}"
NDK_VERSION="26.1.10909125"
NDK_ROOT="${ANDROID_NDK_HOME:-$SDK_ROOT/ndk/$NDK_VERSION}"
CARGO_TARGET_DIR="${CARGO_TARGET_DIR:-$HOME/.cache/pockethle-bada/cargo-target}"
JNI_LIBS="$FRONTEND_DIR/app/src/main/jniLibs"

export ANDROID_SDK_ROOT="$SDK_ROOT"
export ANDROID_HOME="$SDK_ROOT"
export ANDROID_NDK_HOME="$NDK_ROOT"
export CARGO_TARGET_DIR
export CARGO_BUILD_JOBS="${CARGO_BUILD_JOBS:-2}"
export CMAKE_BUILD_PARALLEL_LEVEL="${CMAKE_BUILD_PARALLEL_LEVEL:-2}"

if ! command -v cargo >/dev/null 2>&1; then
    echo "Rust is required. Install the stable Rust toolchain first." >&2
    exit 1
fi
if ! cargo ndk --version >/dev/null 2>&1; then
    echo "cargo-ndk is required. Install it with: cargo install --locked cargo-ndk" >&2
    exit 1
fi
if [[ ! -x "$NDK_ROOT/toolchains/llvm/prebuilt/linux-x86_64/bin/clang" ]]; then
    echo "Android NDK $NDK_VERSION was not found at $NDK_ROOT" >&2
    exit 1
fi

mkdir -p "$JNI_LIBS"
cd "$REPO_ROOT"
cargo ndk \
    --target arm64-v8a \
    --target armeabi-v7a \
    --platform 24 \
    --output-dir "$JNI_LIBS" \
    build --release --package pocket-android-jni

for item in "arm64-v8a:aarch64-linux-android" "armeabi-v7a:armv7-linux-androideabi"; do
    abi="${item%%:*}"
    triple="${item#*:}"
    unicorn="$(find "$CARGO_TARGET_DIR/$triple/release/build" -type f -path '*/unicorn-engine-sys-*/out/lib/libunicorn.so' -print -quit)"
    if [[ -z "$unicorn" ]]; then
        echo "Could not find libunicorn.so for $abi under $CARGO_TARGET_DIR" >&2
        exit 1
    fi
    mkdir -p "$JNI_LIBS/$abi"
    cp "$unicorn" "$JNI_LIBS/$abi/libunicorn.so"
done

cd "$FRONTEND_DIR"
./gradlew --no-daemon --max-workers=2 :app:assembleDebug

APK="$FRONTEND_DIR/app/build/outputs/apk/debug/app-debug.apk"
OUTPUT="$REPO_ROOT/dist/PocketHLE-Bada-Android.apk"
if [[ ! -s "$APK" ]]; then
    echo "Gradle did not produce $APK" >&2
    exit 1
fi
mkdir -p "$(dirname "$OUTPUT")"
cp "$APK" "$OUTPUT"
printf 'APK: %s\n' "$OUTPUT"
