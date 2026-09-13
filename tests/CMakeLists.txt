# Tests for the abt-reconverse skeleton.
#
# The smoke test is built by a shell script rather than add_executable(): it
# must be compiled by a C compiler against the INSTALLED prefix through
# pkg-config, which is how Margo/Thallium/Yokan consume Argobots.

# The library itself is C++ only; we need a C compiler just for this test.
enable_language(C)
set(CMAKE_C_COMPILER_FOR_TEST "${CMAKE_C_COMPILER}")

# ctest runs the script with a bare environment; on macOS the compiler then
# cannot find the SDK headers unless the sysroot is named on the command line.
set(ABT_TEST_SYSROOT_FLAG "")
if(APPLE AND CMAKE_OSX_SYSROOT)
  set(ABT_TEST_SYSROOT_FLAG "-isysroot ${CMAKE_OSX_SYSROOT}")
endif()

configure_file(run_abi_smoke.sh.in
               "${CMAKE_CURRENT_BINARY_DIR}/run_abi_smoke.sh" @ONLY
               FILE_PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE
                                GROUP_READ GROUP_EXECUTE
                                WORLD_READ WORLD_EXECUTE)

add_test(NAME abi_smoke
         COMMAND "${CMAKE_CURRENT_BINARY_DIR}/run_abi_smoke.sh")
set_tests_properties(abi_smoke PROPERTIES
    PASS_REGULAR_EXPRESSION "PASSED: 0 failure")

# Re-check C linkage on the INSTALLED static library, not just the built one.
add_test(NAME c_linkage
         COMMAND "${CMAKE_SOURCE_DIR}/cmake/check_c_symbols.sh"
                 "${CMAKE_INSTALL_PREFIX}/lib/libabt.a")

# The installed library must export exactly abt.h's ABT_* surface: nothing
# dropped while stubs are replaced by real code in later steps, nothing extra.
add_test(NAME abi_surface
         COMMAND "${CMAKE_SOURCE_DIR}/cmake/check_surface.sh"
                 "${CMAKE_SOURCE_DIR}/include/abt.h"
                 "${CMAKE_INSTALL_PREFIX}/lib/libabt.a")

# functional tests, linked directly against the static library
enable_language(C)
foreach(t core_smoke sync_smoke)
  add_executable(${t} ${t}.c)
  target_link_libraries(${t} PRIVATE abt_static)
  target_include_directories(${t} PRIVATE ${CMAKE_SOURCE_DIR}/include)
  add_test(NAME ${t} COMMAND ${t})
  set_tests_properties(${t} PROPERTIES TIMEOUT 120)
endforeach()
