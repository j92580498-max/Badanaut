cmake_minimum_required(VERSION 3.22)
project(bada-hle LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
get_filename_component(REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)

set(OpenGL_GL_PREFERENCE GLVND)
find_package(OpenGL REQUIRED)
find_package(SDL2 CONFIG REQUIRED)
find_package(SDL2_image CONFIG REQUIRED)
add_subdirectory("${REPO_ROOT}/third_party/unicorn" "${CMAKE_CURRENT_BINARY_DIR}/unicorn")
add_library(bada-pugixml STATIC "${REPO_ROOT}/third_party/pugixml/pugixml.cpp")
target_include_directories(bada-pugixml PUBLIC "${REPO_ROOT}/third_party/pugixml")

add_executable(bada-hle
    src/main.cpp src/control.cpp src/elf_loader.cpp src/cpu.cpp src/pixel_accel.cpp
    src/hle/dispatch.cpp src/hle/libc.cpp src/hle/gl.cpp src/hle/osp.cpp
    src/hle/io.cpp src/hle/audio.cpp src/hle/ui.cpp src/ui_overlay.cpp
)
# Source includes use SDL2/SDL.h on all hosts. vcpkg exports SDL2's leaf include
# directory, so provide a consistently named include prefix from vendored SDL.
file(COPY "${REPO_ROOT}/third_party/SDL2/include/" DESTINATION "${CMAKE_CURRENT_BINARY_DIR}/include_compat/SDL2")
target_include_directories(bada-hle PRIVATE src "${CMAKE_CURRENT_BINARY_DIR}/include_compat"
    "${REPO_ROOT}/third_party/cxxopts/include")
target_link_libraries(bada-hle PRIVATE SDL2::SDL2 SDL2_image::SDL2_image unicorn bada-pugixml OpenGL::GL)
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

# Put the CPU DLL/SONAME alongside the engine for portable packaging. PyInstaller
# follows the rest of the native executable's dependencies when it builds a bundle.
add_custom_command(TARGET bada-hle POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:unicorn>" "$<TARGET_FILE_DIR:bada-hle>"
)
if(UNIX AND NOT APPLE)
    add_custom_command(TARGET bada-hle POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_SONAME_FILE:unicorn>" "$<TARGET_FILE_DIR:bada-hle>")
endif()
if(WIN32)
    add_custom_command(TARGET bada-hle POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:SDL2::SDL2>" "$<TARGET_FILE_DIR:bada-hle>"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:SDL2_image::SDL2_image>" "$<TARGET_FILE_DIR:bada-hle>")
endif()
