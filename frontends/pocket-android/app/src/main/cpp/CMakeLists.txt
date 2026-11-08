cmake_minimum_required(VERSION 3.22)
project(badanaut_android LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
find_package(Threads REQUIRED)

get_filename_component(REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../../../../" ABSOLUTE)
set(BADA_ROOT "${REPO_ROOT}/third_party/bada-hle/src")
set(SDL_ROOT "${REPO_ROOT}/third_party/SDL2")
add_subdirectory("${REPO_ROOT}/third_party/unicorn" "${CMAKE_CURRENT_BINARY_DIR}/unicorn")

set(SDL_SHARED ON CACHE BOOL "" FORCE)
set(SDL_STATIC OFF CACHE BOOL "" FORCE)
set(SDL_TEST OFF CACHE BOOL "" FORCE)
set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory("${SDL_ROOT}" "${CMAKE_CURRENT_BINARY_DIR}/SDL2")

set(SDL_INCLUDE_COMPAT "${CMAKE_CURRENT_BINARY_DIR}/include_compat")
file(MAKE_DIRECTORY "${SDL_INCLUDE_COMPAT}")
if(NOT EXISTS "${SDL_INCLUDE_COMPAT}/SDL2")
    file(CREATE_LINK "${SDL_ROOT}/include" "${SDL_INCLUDE_COMPAT}/SDL2" SYMBOLIC)
endif()

add_library(pugixml STATIC "${REPO_ROOT}/third_party/pugixml/pugixml.cpp")
target_include_directories(pugixml PUBLIC "${REPO_ROOT}/third_party/pugixml")

add_library(cxxopts INTERFACE)
target_include_directories(cxxopts INTERFACE "${REPO_ROOT}/third_party/cxxopts/include")

add_library(bada-android SHARED
    "${BADA_ROOT}/main.cpp"
    "${BADA_ROOT}/control.cpp"
    "${BADA_ROOT}/elf_loader.cpp"
    "${BADA_ROOT}/cpu.cpp"
    "${BADA_ROOT}/pixel_accel.cpp"
    "${BADA_ROOT}/hle/dispatch.cpp"
    "${BADA_ROOT}/hle/libc.cpp"
    "${BADA_ROOT}/hle/gl.cpp"
    "${BADA_ROOT}/hle/osp.cpp"
    "${BADA_ROOT}/hle/io.cpp"
    "${BADA_ROOT}/hle/audio.cpp"
    "${BADA_ROOT}/hle/ui.cpp"
    "${REPO_ROOT}/third_party/pvr/PVRTDecompress.cpp"
    "${REPO_ROOT}/third_party/tlsf/tlsf.c"
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
    "${REPO_ROOT}/third_party/pvr"
    "${REPO_ROOT}/third_party/tlsf"
    "${CMAKE_CURRENT_LIST_DIR}"
    "${SDL_INCLUDE_COMPAT}"
)
target_link_libraries(bada-android PRIVATE
    SDL2::SDL2
    unicorn
    Threads::Threads
    pugixml
    cxxopts
    android
    log
    GLESv1_CM
    GLESv2
    EGL
)
