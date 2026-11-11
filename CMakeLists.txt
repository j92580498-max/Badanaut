cmake_minimum_required(VERSION 3.22)
project(Badanaut LANGUAGES C CXX)
include(CTest)
add_subdirectory(third_party/bada-hle)

if(WIN32 AND BUILD_TESTING)
    add_executable(bada-unicorn-windows-test frontends/pocket-android/tests/native/unicorn_windows_test.cpp)
    target_link_libraries(bada-unicorn-windows-test PRIVATE unicorn)
    set_target_properties(bada-unicorn-windows-test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/native"
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/native")
    add_custom_command(TARGET bada-unicorn-windows-test POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:unicorn>" "$<TARGET_FILE_DIR:bada-unicorn-windows-test>")
    add_test(NAME windows-unicorn-cache COMMAND bada-unicorn-windows-test)
    set_tests_properties(windows-unicorn-cache PROPERTIES TIMEOUT 30)
endif()
