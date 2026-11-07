cmake_minimum_required(VERSION 3.16)
project(bada_gles_tests LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)
pkg_check_modules(UNICORN REQUIRED IMPORTED_TARGET unicorn)
pkg_check_modules(GLES1 REQUIRED IMPORTED_TARGET glesv1_cm)
pkg_check_modules(GLES2 REQUIRED IMPORTED_TARGET glesv2)
pkg_check_modules(DESKTOP_GL REQUIRED IMPORTED_TARGET gl)

get_filename_component(REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../.." ABSOLUTE)
set(BADA_ROOT "${REPO_ROOT}/third_party/bada-hle/src")
set(BADA_TEST_SOURCES
    "${BADA_ROOT}/cpu.cpp"
    "${BADA_ROOT}/elf_loader.cpp"
    "${BADA_ROOT}/hle/dispatch.cpp"
    "${BADA_ROOT}/hle/libc.cpp"
    "${BADA_ROOT}/hle/gl.cpp"
    "${BADA_ROOT}/hle/osp.cpp"
    "${BADA_ROOT}/hle/io.cpp"
    "${BADA_ROOT}/hle/audio.cpp"
    "${BADA_ROOT}/hle/ui.cpp"
)
add_library(bada-test-engine STATIC ${BADA_TEST_SOURCES}
    "${REPO_ROOT}/frontends/pocket-android/app/src/main/cpp/gl_android.cpp"
)
target_compile_definitions(bada-test-engine PRIVATE BADA_GLES_BACKEND=1)
target_include_directories(bada-test-engine PUBLIC "${BADA_ROOT}")
target_link_libraries(bada-test-engine PUBLIC
    PkgConfig::SDL2 PkgConfig::UNICORN PkgConfig::GLES1 PkgConfig::GLES2
)

enable_testing()
add_executable(bada-gles-test gl_render_test.cpp)
target_link_libraries(bada-gles-test PRIVATE bada-test-engine)
add_executable(bada-runtime-test runtime_test.cpp)
target_link_libraries(bada-runtime-test PRIVATE bada-test-engine)
add_test(NAME runtime-audio COMMAND bada-runtime-test)
set_tests_properties(runtime-audio PROPERTIES TIMEOUT 30)
add_executable(bada-log-test
    diagnostic_log_test.cpp
    "${REPO_ROOT}/frontends/pocket-android/app/src/main/cpp/diagnostic_log.cpp"
)
target_include_directories(bada-log-test PRIVATE "${REPO_ROOT}/frontends/pocket-android/app/src/main/cpp")
add_test(NAME native-log COMMAND bada-log-test)
set_tests_properties(native-log PROPERTIES TIMEOUT 30)
foreach(api RANGE 1 2)
    foreach(abi hard soft)
        add_test(NAME "gles${api}-${abi}" COMMAND bada-gles-test "${api}" "${abi}")
        set_tests_properties("gles${api}-${abi}" PROPERTIES TIMEOUT 60)
        if(api EQUAL 1)
            set_tests_properties("gles${api}-${abi}" PROPERTIES SKIP_RETURN_CODE 77)
        endif()
    endforeach()
endforeach()

# Exercise the same fixed-function guest handlers through desktop compatibility
# GL even when the host's native ES 1 context is unavailable. This is separate
# coverage; it does not substitute for the native ES 1 probe.
add_library(bada-desktop-test-engine STATIC ${BADA_TEST_SOURCES})
target_include_directories(bada-desktop-test-engine PUBLIC "${BADA_ROOT}")
target_link_libraries(bada-desktop-test-engine PUBLIC
    PkgConfig::SDL2 PkgConfig::UNICORN PkgConfig::DESKTOP_GL
)
add_executable(bada-desktop-render-test gl_render_test.cpp)
target_compile_definitions(bada-desktop-render-test PRIVATE BADA_TEST_DESKTOP=1)
target_link_libraries(bada-desktop-render-test PRIVATE bada-desktop-test-engine)
foreach(abi hard soft)
    add_test(NAME "desktop-gles1-${abi}" COMMAND bada-desktop-render-test 1 "${abi}")
    set_tests_properties("desktop-gles1-${abi}" PROPERTIES TIMEOUT 60)
endforeach()
