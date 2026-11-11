# Keep the pinned upstream checkout unchanged. Only the Windows build compiles
# a corrected copy of this translation unit from the build directory.
function(badanaut_patch_unicorn_windows)
    set(original "${BADANAUT_THIRD_PARTY}/unicorn/qemu/accel/tcg/translate-all.c")
    set(patched "${CMAKE_BINARY_DIR}/deps/unicorn/badanaut/translate-all.c")
    file(READ "${original}" source)
    string(FIND "${source}" "size = base_end - base;" commit_position)
    string(FIND "${source}" "\\x48\\x83\\xec\\x10" reserve_position)
    string(FIND "${source}" "\\x48\\x83\\xc4\\x10" restore_position)
    if(commit_position LESS 0 OR reserve_position LESS 0 OR restore_position LESS 0)
        message(FATAL_ERROR "The pinned Unicorn Windows JIT source changed; review cmake/UnicornWindows.cmake.")
    endif()

    # Commit only the remaining reserved pages, including caches smaller than
    # the handler's usual 4 MiB batch and the final batch of a larger cache.
    string(REPLACE "size = base_end - base;" "size = (uint32_t)(right - base);" source "${source}")
    # The Windows x64 ABI needs 32 bytes of shadow space and 16-byte alignment
    # before CALL. At callback entry RSP is 8 mod 16: reserve/restore 40 bytes.
    string(REPLACE "\\x48\\x83\\xec\\x10" "\\x48\\x83\\xec\\x28" source "${source}")
    string(REPLACE "\\x48\\x83\\xc4\\x10" "\\x48\\x83\\xc4\\x28" source "${source}")
    string(REPLACE "sub rsp, 0x10; reserve 2 slots as ms fastcall requires"
                   "sub rsp, 0x28; reserve 4 shadow slots and align the call" source "${source}")
    string(REPLACE "add rsp, 0x10 ; clean stack" "add rsp, 0x28 ; clean stack" source "${source}")
    string(PREPEND source "/* Badanaut Windows JIT fixes; see cmake/UnicornWindows.cmake. */\n")
    file(CONFIGURE OUTPUT "${patched}" CONTENT "${source}" @ONLY)

    get_target_property(upstream_sources arm-softmmu SOURCES)
    get_target_property(upstream_directory arm-softmmu SOURCE_DIR)
    set(patched_sources)
    set(replaced FALSE)
    foreach(entry IN LISTS upstream_sources)
        if(entry MATCHES "(^|/)qemu/accel/tcg/translate-all\\.c$")
            list(APPEND patched_sources "${patched}")
            set(replaced TRUE)
        elseif(IS_ABSOLUTE "${entry}")
            list(APPEND patched_sources "${entry}")
        else()
            list(APPEND patched_sources "${upstream_directory}/${entry}")
        endif()
    endforeach()
    if(NOT replaced)
        message(FATAL_ERROR "The Unicorn ARM target no longer contains translate-all.c; review the Windows JIT fix.")
    endif()
    set_property(TARGET arm-softmmu PROPERTY SOURCES "${patched_sources}")
    target_include_directories(arm-softmmu PRIVATE "${BADANAUT_THIRD_PARTY}/unicorn/qemu/accel/tcg")
endfunction()
