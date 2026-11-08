cmake_minimum_required(VERSION 3.16)
project(bada-hle CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# OpenGL (portable: libGL on Linux, opengl32 on Windows)
set(OpenGL_GL_PREFERENCE GLVND)
find_package(OpenGL REQUIRED)

# SDL2 + Unicorn. Prefer CMake config packages (vcpkg / installed configs);
# fall back to pkg-config (Linux, MSYS2). Set CMAKE_TOOLCHAIN_FILE to vcpkg on
# Windows/MSVC, or install the -dev packages on Linux/MSYS2.
find_package(SDL2 CONFIG QUIET)
find_package(SDL2_image CONFIG QUIET)
find_package(unicorn CONFIG QUIET)
if(NOT SDL2_FOUND OR NOT SDL2_image_FOUND OR NOT unicorn_FOUND)
    find_package(PkgConfig REQUIRED)
    if(NOT SDL2_FOUND)
        pkg_check_modules(SDL2 REQUIRED sdl2)
    endif()
    if(NOT SDL2_image_FOUND)
        pkg_check_modules(SDL2_IMAGE REQUIRED SDL2_image)
    endif()
    if(NOT unicorn_FOUND)
        pkg_check_modules(UNICORN REQUIRED unicorn)
    endif()
endif()

# pugixml (package-manifest XML) + cxxopts (CLI parsing). Both vcpkg/CONFIG with a
# pkg-config fallback for Linux/MSYS2.
find_package(pugixml CONFIG QUIET)
find_package(cxxopts CONFIG QUIET)
if(NOT pugixml_FOUND OR NOT cxxopts_FOUND)
    find_package(PkgConfig REQUIRED)
    if(NOT pugixml_FOUND)
        pkg_check_modules(PUGIXML REQUIRED pugixml)
    endif()
    if(NOT cxxopts_FOUND)
        pkg_check_modules(CXXOPTS REQUIRED cxxopts)
    endif()
endif()

add_executable(bada-hle
    src/main.cpp
    src/control.cpp
    src/elf_loader.cpp
    src/cpu.cpp
    src/pixel_accel.cpp
    src/hle/dispatch.cpp
    src/hle/libc.cpp
    src/hle/gl.cpp
    src/hle/osp.cpp
    src/hle/io.cpp
    src/hle/audio.cpp
    src/hle/ui.cpp
    src/ui_overlay.cpp
)

target_include_directories(bada-hle PRIVATE src ${SDL2_INCLUDE_DIRS} ${SDL2_IMAGE_INCLUDE_DIRS} ${UNICORN_INCLUDE_DIRS})
target_link_directories(bada-hle PRIVATE ${SDL2_LIBRARY_DIRS} ${SDL2_IMAGE_LIBRARY_DIRS} ${UNICORN_LIBRARY_DIRS})
target_link_libraries(bada-hle PRIVATE
    ${SDL2_LIBRARIES} ${UNICORN_LIBRARIES}
    OpenGL::GL)

# SDL2_image: imported target when found via CONFIG (vcpkg), variables via
# pkg-config (Linux/MSYS2). Gives us IMG_SavePNG/IMG_SaveJPG for screenshots.
if(SDL2_image_FOUND)
    target_link_libraries(bada-hle PRIVATE SDL2_image::SDL2_image)
else()
    target_link_libraries(bada-hle PRIVATE ${SDL2_IMAGE_LIBRARIES})
endif()

# pugixml + cxxopts: imported targets via CONFIG, variables via pkg-config.
if(pugixml_FOUND)
    target_link_libraries(bada-hle PRIVATE pugixml::pugixml)
else()
    target_include_directories(bada-hle PRIVATE ${PUGIXML_INCLUDE_DIRS})
    target_link_directories(bada-hle PRIVATE ${PUGIXML_LIBRARY_DIRS})
    target_link_libraries(bada-hle PRIVATE ${PUGIXML_LIBRARIES})
endif()
if(cxxopts_FOUND)
    target_link_libraries(bada-hle PRIVATE cxxopts::cxxopts)
else()
    target_include_directories(bada-hle PRIVATE ${CXXOPTS_INCLUDE_DIRS})
endif()

if(WIN32)
    target_link_libraries(bada-hle PRIVATE ws2_32) # control server sockets
endif()

if(MSVC)
    # NOMINMAX: stop <windows.h> (pulled in by SDL/GL) from defining min/max
    #   macros that clobber std::min/std::max.
    # WIN32_LEAN_AND_MEAN: trim the <windows.h> surface.
    # _CRT_SECURE_NO_WARNINGS: allow portable fopen/etc. without /sdl noise.
    target_compile_definitions(bada-hle PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
    target_compile_options(bada-hle PRIVATE /W3 /wd4101 /wd4244)
    # pkg-config resolves the RELEASE static SDL2/Unicorn (SDL2-static.lib from
    # x64-windows-static/lib), which are built against the *release* static CRT
    # (/MT) even in a Debug build. Use /MT here too (NOT /MTd): a debug-CRT app
    # linked against release-CRT static libs aborts at runtime on heap-validation
    # / iterator-debug-level mismatch. Switch to dynamic deps -> drop this.
    set_property(TARGET bada-hle PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded")
else()
    target_compile_options(bada-hle PRIVATE -g -O2 -Wall -Wno-unused-parameter)
endif()
