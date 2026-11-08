#!/usr/bin/env bash
set -euo pipefail

FRONTEND_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$FRONTEND_DIR/../.." && pwd)"
SDK_ROOT="${ANDROID_SDK_ROOT:-${ANDROID_HOME:-$HOME/Android/Sdk}}"
NDK_VERSION="26.1.10909125"
NDK_ROOT="${ANDROID_NDK_HOME:-$SDK_ROOT/ndk/$NDK_VERSION}"

export ANDROID_SDK_ROOT="$SDK_ROOT"
export ANDROID_HOME="$SDK_ROOT"
export ANDROID_NDK_HOME="$NDK_ROOT"
export CMAKE_BUILD_PARALLEL_LEVEL="${CMAKE_BUILD_PARALLEL_LEVEL:-2}"

if [[ ! -f "$NDK_ROOT/build/cmake/android.toolchain.cmake" ]]; then
    echo "Android NDK $NDK_VERSION was not found at $NDK_ROOT" >&2
    exit 1
fi

cd "$FRONTEND_DIR"
./gradlew --no-daemon --max-workers=2 :app:assembleDebug

APK="$FRONTEND_DIR/app/build/outputs/apk/debug/app-debug.apk"
OUTPUT="$REPO_ROOT/dist/Badanaut-Android.apk"
if [[ ! -s "$APK" ]]; then
    echo "Gradle did not produce $APK" >&2
    exit 1
fi
mkdir -p "$(dirname "$OUTPUT")"
cp "$APK" "$OUTPUT"
printf 'APK: %s\n' "$OUTPUT"
