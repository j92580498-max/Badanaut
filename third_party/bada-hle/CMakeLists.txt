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
badanaut_add_audio_decoders()

add_library(bada-hle-core STATIC
    src/elf_loader.cpp src/cpu.cpp src/pixel_accel.cpp
    src/hle/dispatch.cpp src/hle/libc.cpp src/hle/gl.cpp src/hle/osp.cpp
    src/hle/io.cpp src/hle/audio.cpp src/hle/ui.cpp
)
target_include_directories(bada-hle-core PUBLIC src)
target_link_libraries(bada-hle-core PUBLIC bada-sdl unicorn bada-pvr bada-tlsf bada-audio-decoders OpenGL::GL Threads::Threads)
add_executable(bada-hle src/main.cpp src/control.cpp src/ui_overlay.cpp)
target_include_directories(bada-hle PRIVATE src)
target_link_libraries(bada-hle PRIVATE bada-hle-core SDL2_image::SDL2_image
    bada-pugixml bada-cxxopts bada-nuklear)
set_target_properties(bada-hle PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/native"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/native"
    BUILD_RPATH "$ORIGIN" INSTALL_RPATH "$ORIGIN")

set(BADA_RUNTIME_BUNDLE "${REPO_ROOT}/bada_libs")
foreach(abi soft-float hard-float)
    foreach(library libstdc++.so.6 libgcc_s.so.1 libsupc++.so)
        if(NOT EXISTS "${BADA_RUNTIME_BUNDLE}/${abi}/${library}")
            message(FATAL_ERROR "Missing bundled Bada runtime: ${abi}/${library}")
        endif()
    endforeach()
endforeach()
# An always-run copy also picks up runtime updates without relinking the engine.
add_custom_target(bada-runtime-bundle ALL
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${BADA_RUNTIME_BUNDLE}" "$<TARGET_FILE_DIR:bada-hle>/bada_libs"
    COMMENT "Copying bundled ARM Bada runtimes and notices"
    VERBATIM)
add_dependencies(bada-hle bada-runtime-bundle)
install(TARGETS bada-hle RUNTIME DESTINATION .)
install(DIRECTORY "${BADA_RUNTIME_BUNDLE}" DESTINATION .)
if(WIN32)
    target_link_libraries(bada-hle PRIVATE ws2_32)
    target_compile_definitions(bada-hle PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
    target_compile_definitions(bada-hle-core PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
endif()
if(MSVC)
    target_sources(bada-hle PRIVATE src/windows.manifest)
    target_compile_options(bada-hle PRIVATE /utf-8 /W3 /wd4101 /wd4244)
    target_compile_options(bada-hle-core PRIVATE /utf-8 /W3 /wd4101 /wd4244)
else()
    target_compile_options(bada-hle PRIVATE -O2 -Wall -Wno-unused-parameter)
    target_compile_options(bada-hle-core PRIVATE -O2 -Wall -Wno-unused-parameter)
endif()

if(BUILD_TESTING)
    add_executable(bada-framework-test "${REPO_ROOT}/frontends/pocket-android/tests/native/framework_test.cpp")
    target_link_libraries(bada-framework-test PRIVATE bada-hle-core)
    add_test(NAME desktop-runtime-framework COMMAND bada-framework-test)
    add_executable(bada-elf-test "${REPO_ROOT}/frontends/pocket-android/tests/native/elf_test.cpp" src/elf_loader.cpp)
    target_include_directories(bada-elf-test PRIVATE src)
    add_test(NAME runtime-arm-elf COMMAND bada-elf-test)
    foreach(probe bada-framework-test bada-elf-test)
        set_target_properties(${probe} PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/native"
            RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/native")
        if(MSVC)
            target_compile_options(${probe} PRIVATE /utf-8)
        endif()
    endforeach()
    set_tests_properties(desktop-runtime-framework runtime-arm-elf PROPERTIES TIMEOUT 30)
    foreach(runtime_target unicorn SDL2::SDL2)
        add_custom_command(TARGET bada-framework-test POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:${runtime_target}>" "$<TARGET_FILE_DIR:bada-framework-test>")
    endforeach()
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
