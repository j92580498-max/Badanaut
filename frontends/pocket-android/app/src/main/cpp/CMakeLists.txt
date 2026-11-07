cmake_minimum_required(VERSION 3.22)
project(pockethle_bada_android LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

get_filename_component(REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../../../../" ABSOLUTE)
set(BADA_ROOT "${REPO_ROOT}/third_party/bada-hle/src")
set(SDL_ROOT "${REPO_ROOT}/third_party/SDL2")
set(UNICORN_INCLUDE "${REPO_ROOT}/third_party/unicorn/include")
set(UNICORN_LIBRARY "${CMAKE_CURRENT_LIST_DIR}/../jniLibs/${ANDROID_ABI}/libunicorn.so")

if(NOT EXISTS "${UNICORN_LIBRARY}")
    message(FATAL_ERROR "Missing ${UNICORN_LIBRARY}. Run scripts/build-android.sh first.")
endif()

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

add_library(unicorn SHARED IMPORTED GLOBAL)
set_target_properties(unicorn PROPERTIES
    IMPORTED_LOCATION "${UNICORN_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${UNICORN_INCLUDE}"
)

add_library(bada-android SHARED
    "${BADA_ROOT}/main.cpp"
    "${BADA_ROOT}/control.cpp"
    "${BADA_ROOT}/elf_loader.cpp"
    "${BADA_ROOT}/cpu.cpp"
    "${BADA_ROOT}/hle/dispatch.cpp"
    "${BADA_ROOT}/hle/libc.cpp"
    "${BADA_ROOT}/hle/osp.cpp"
    "${BADA_ROOT}/hle/io.cpp"
    "${BADA_ROOT}/hle/ui.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/gl_android.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/ui_overlay_android.cpp"
)
set_target_properties(bada-android PROPERTIES OUTPUT_NAME main)
target_include_directories(bada-android PRIVATE
    "${BADA_ROOT}"
    "${SDL_INCLUDE_COMPAT}"
    "${UNICORN_INCLUDE}"
)
target_link_libraries(bada-android PRIVATE
    SDL2::SDL2
    unicorn
    pugixml
    cxxopts
    android
    log
    GLESv2
    EGL
)
