cmake_minimum_required(VERSION 3.22)
project(badanaut_unicorn LANGUAGES C)

include(FetchContent)
if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif()

# Same Unicorn release previously provided by unicorn-engine-sys 2.1.5.
# bada executes ARM32 guests; other guest architectures are unnecessary.
set(BUILD_SHARED_LIBS ON CACHE BOOL "" FORCE)
set(UNICORN_ARCH "arm" CACHE STRING "" FORCE)
set(UNICORN_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(UNICORN_INSTALL OFF CACHE BOOL "" FORCE)
set(UNICORN_LEGACY_STATIC_ARCHIVE OFF CACHE BOOL "" FORCE)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

FetchContent_Declare(unicorn_source
    URL https://codeload.github.com/unicorn-engine/unicorn/tar.gz/8028ec436f2d9376525352dd38ed9ed6b9f6be10
    URL_HASH SHA256=9d2a2003f0307d0fc239af439654564fe6ef17b67f7c5a39b45fad4c69d6aec2
)
FetchContent_MakeAvailable(unicorn_source)

if(ANDROID)
    # SDL loads libunicorn.so by name, without a versioned Linux SONAME.
    set_target_properties(unicorn PROPERTIES VERSION "" SOVERSION "")
    foreach(target unicorn unicorn-common arm-softmmu)
        target_compile_options(${target} PRIVATE -O2)
    endforeach()
endif()
