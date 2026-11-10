cmake_minimum_required(VERSION 3.22)
project(badanaut_android LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
find_package(Threads REQUIRED)

get_filename_component(REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../../../../" ABSOLUTE)
set(BADA_ROOT "${REPO_ROOT}/third_party/bada-hle/src")
include("${REPO_ROOT}/cmake/Dependencies.cmake")
badanaut_add_sdl()
badanaut_add_unicorn()
badanaut_add_support_libraries()

add_library(bada-android SHARED
    "${BADA_ROOT}/main.cpp"
    "${BADA_ROOT}/control.cpp"
    "${BADA_ROOT}/elf_loader.cpp"
    "${BADA_ROOT}/cpu.cpp"
    "${BADA_ROOT}/pixel_accel.cpp"
    "${BADA_ROOT}/math_accel.cpp"
    "${BADA_ROOT}/performance.cpp"
    "${BADA_ROOT}/render_queue.cpp"
    "${BADA_ROOT}/internal_resolution.cpp"
    "${BADA_ROOT}/hle/dispatch.cpp"
    "${BADA_ROOT}/hle/libc.cpp"
    "${BADA_ROOT}/hle/gl.cpp"
    "${BADA_ROOT}/hle/osp.cpp"
    "${BADA_ROOT}/hle/io.cpp"
    "${BADA_ROOT}/hle/audio.cpp"
    "${BADA_ROOT}/hle/ui.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/gl_android.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/diagnostic_log.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/ui_overlay_android.cpp"
)
set_target_properties(bada-android PROPERTIES OUTPUT_NAME main)
target_compile_definitions(bada-android PRIVATE BADA_GLES_BACKEND=1)
# The distributable APK uses assembleDebug; its emulator still needs optimized
# native handlers, especially for audio mixing and software framebuffers.
target_compile_options(bada-android PRIVATE -O2)
target_include_directories(bada-android PRIVATE
    "${BADA_ROOT}"
    "${CMAKE_CURRENT_LIST_DIR}"
)
target_link_libraries(bada-android PRIVATE
    bada-sdl
    unicorn
    Threads::Threads
    bada-pugixml
    bada-cxxopts
    bada-pvr
    bada-tlsf
    android
    log
    GLESv1_CM
    GLESv2
    EGL
)
