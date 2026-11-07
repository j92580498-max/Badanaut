# Android armeabi-v7a toolchain wrapper for cmake-rs cross-compiles.
#
# See `android-arm64-v8a.toolchain.cmake` for the rationale. Same fix, just
# the 32-bit ARMv7 ABI.

set(ANDROID_ABI armeabi-v7a)
if(NOT DEFINED ANDROID_PLATFORM)
    set(ANDROID_PLATFORM android-21)
endif()

if(NOT DEFINED ENV{ANDROID_NDK_HOME} AND NOT DEFINED ENV{ANDROID_NDK_ROOT})
    message(FATAL_ERROR
        "Set ANDROID_NDK_HOME (or ANDROID_NDK_ROOT) before invoking cargo. "
        "Required for the NDK CMake toolchain include below.")
endif()

if(DEFINED ENV{ANDROID_NDK_HOME})
    set(_pockethle_ndk_path "$ENV{ANDROID_NDK_HOME}")
else()
    set(_pockethle_ndk_path "$ENV{ANDROID_NDK_ROOT}")
endif()

include("${_pockethle_ndk_path}/build/cmake/android.toolchain.cmake")
