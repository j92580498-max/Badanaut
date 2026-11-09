cmake_minimum_required(VERSION 3.22)
project(bada-hle LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
get_filename_component(REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)

set(OpenGL_GL_PREFERENCE GLVND)
find_package(Threads REQUIRED)
find_package(OpenGL REQUIRED)
include("${REPO_ROOT}/cmake/Dependencies.cmake")
badanaut_add_sdl()
badanaut_add_unicorn()
badanaut_add_support_libraries()
badanaut_add_sdl_image()
badanaut_add_nuklear()

add_executable(bada-hle
    src/main.cpp src/control.cpp src/elf_loader.cpp src/cpu.cpp src/pixel_accel.cpp
    src/hle/dispatch.cpp src/hle/libc.cpp src/hle/gl.cpp src/hle/osp.cpp
    src/hle/io.cpp src/hle/audio.cpp src/hle/ui.cpp src/ui_overlay.cpp
)
target_include_directories(bada-hle PRIVATE src)
target_link_libraries(bada-hle PRIVATE bada-sdl SDL2_image::SDL2_image unicorn
    bada-pugixml bada-cxxopts bada-pvr bada-tlsf bada-nuklear OpenGL::GL Threads::Threads)
set_target_properties(bada-hle PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/native"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/native"
    BUILD_RPATH "$ORIGIN" INSTALL_RPATH "$ORIGIN")
if(WIN32)
    target_link_libraries(bada-hle PRIVATE ws2_32)
    target_compile_definitions(bada-hle PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
endif()
if(MSVC)
    target_sources(bada-hle PRIVATE src/windows.manifest)
    target_compile_options(bada-hle PRIVATE /utf-8 /W3 /wd4101 /wd4244)
else()
    target_compile_options(bada-hle PRIVATE -O2 -Wall -Wno-unused-parameter)
endif()

# Keep built runtime libraries beside the engine for standalone runs and packaging.
foreach(runtime_target unicorn SDL2::SDL2 SDL2_image::SDL2_image)
    add_custom_command(TARGET bada-hle POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:${runtime_target}>" "$<TARGET_FILE_DIR:bada-hle>")
    if(UNIX AND NOT APPLE)
        add_custom_command(TARGET bada-hle POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_SONAME_FILE:${runtime_target}>" "$<TARGET_FILE_DIR:bada-hle>")
    endif()
endforeach()
