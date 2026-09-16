cmake_minimum_required(VERSION 3.16)
project(abt-reconverse VERSION 1.2.0 LANGUAGES CXX)

#
# libabt: the Argobots C ABI implemented on top of Reconverse.
#
# Phase 2 step 1 = skeleton: every public ABT_* entry point exists as a plain C
# symbol and returns ABT_ERR_FEATURE_NA.  Semantics arrive in later steps.
#

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "build type" FORCE)
endif()

if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
  set(CMAKE_INSTALL_PREFIX "${CMAKE_BINARY_DIR}/install"
      CACHE PATH "install prefix" FORCE)
endif()

# ---------------------------------------------------------------- Reconverse
#
# Reconverse installs no CMake package config, so find_package() is not an
# option: point at an install prefix and use its include dir + static archive
# directly.  The internal queue templates (MPMCQueue, used by the built-in
# pools from step 2 on) live under include/reconverse-internal.
#
set(RECONVERSE_DIR
    "/Users/kale/software/argobots-succession/reconverse/build-abt-pools/install"
    CACHE PATH "Reconverse install prefix (include/ + lib/libreconverse.a)")

set(RECONVERSE_INCLUDE_DIR "${RECONVERSE_DIR}/include")
set(RECONVERSE_INTERNAL_INCLUDE_DIR "${RECONVERSE_DIR}/include/reconverse-internal")
if(EXISTS "${RECONVERSE_DIR}/lib64/libreconverse.a")   # GNUInstallDirs on RHEL-family Linux
  set(RECONVERSE_LIBDIR "${RECONVERSE_DIR}/lib64")
else()
  set(RECONVERSE_LIBDIR "${RECONVERSE_DIR}/lib")
endif()
set(RECONVERSE_LIBRARY "${RECONVERSE_LIBDIR}/libreconverse.a")

foreach(_p "${RECONVERSE_INCLUDE_DIR}/converse.h"
           "${RECONVERSE_INTERNAL_INCLUDE_DIR}/queue.h"
           "${RECONVERSE_LIBRARY}")
  if(NOT EXISTS "${_p}")
    message(FATAL_ERROR "Reconverse artifact not found: ${_p}\n"
                        "Set -DRECONVERSE_DIR=<prefix> to a Reconverse install.")
  endif()
endforeach()

# libreconverse.a's own external dependencies.  `nm -u libreconverse.a` on this
# build shows only libc/libc++, pthread_* and hwloc_* (this Reconverse was
# configured with HWLOC found at /opt/local and LCI_DIR-NOTFOUND, so there are
# no fi_*/lci_* symbols to satisfy).
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

find_library(HWLOC_LIBRARY NAMES hwloc
             HINTS ENV HWLOC_ROOT ENV HWLOC_DIR
             PATH_SUFFIXES lib lib64
             PATHS /opt/local /usr/local /usr
             DOC "hwloc library referenced by libreconverse.a")
if(NOT HWLOC_LIBRARY)
  message(FATAL_ERROR "hwloc not found; libreconverse.a references hwloc_* symbols")
endif()

set(RECONVERSE_LINK_LIBS "${RECONVERSE_LIBRARY}" "${HWLOC_LIBRARY}"
                         Threads::Threads)

# ---------------------------------------------------------------- the library
set(ABT_SOURCES
    src/init.cpp
    src/xstream.cpp
    src/sched.cpp
    src/pool.cpp
    src/thread.cpp
    src/sync.cpp
    src/misc.cpp)

add_library(abt_objs OBJECT ${ABT_SOURCES})
# libabt.so is loaded at program start by every consumer (DAOS, Margo,
# Thallium, Yokan), never dlopen'ed, so its thread_locals can use the
# initial-exec TLS model: a direct %fs load instead of a __tls_get_addr call
# (DISPATCH-LADDER.md). Pair with -DRECONVERSE_TLS_MODEL=initial-exec on the
# reconverse build, whose Cpv variables are the bulk of the accesses. Set to
# an empty string for a dlopen'ed use.
set(ABT_TLS_MODEL "initial-exec" CACHE STRING "TLS model for the shim's thread-local variables (empty for compiler default)")
if(ABT_TLS_MODEL)
  target_compile_options(abt_objs PRIVATE -ftls-model=${ABT_TLS_MODEL})
endif()
set_target_properties(abt_objs PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_options(abt_objs PRIVATE -Wall -Wextra)
target_include_directories(abt_objs PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/src")
# SYSTEM: converse.h declares static functions it does not define in the
# header, which -Wall reports on every translation unit.  Not our warnings.
target_include_directories(abt_objs SYSTEM PRIVATE
    "${RECONVERSE_INCLUDE_DIR}"
    "${RECONVERSE_INTERNAL_INCLUDE_DIR}")

add_library(abt_static STATIC $<TARGET_OBJECTS:abt_objs>)
add_library(abt_shared SHARED $<TARGET_OBJECTS:abt_objs>)

foreach(_t abt_static abt_shared)
  set_target_properties(${_t} PROPERTIES OUTPUT_NAME abt LINKER_LANGUAGE CXX)
  target_include_directories(${_t} INTERFACE
      $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
      $<INSTALL_INTERFACE:include>)
  # Reconverse is a static archive; link it into both flavours so that a
  # consumer of libabt never has to name it.
  target_link_libraries(${_t} PUBLIC ${RECONVERSE_LINK_LIBS})
endforeach()

# Mirror the Argobots 1.2 install names (libabt.1.dylib / libabt.so.1).
set_target_properties(abt_shared PROPERTIES
    VERSION ${PROJECT_VERSION}
    SOVERSION 1
    INSTALL_NAME_DIR "${CMAKE_INSTALL_PREFIX}/lib"
    BUILD_WITH_INSTALL_NAME_DIR TRUE)

# ------------------------------------------------- C-linkage check (post-build)
# The one mistake that silently breaks the ABI is a definition that lands
# outside an extern "C" block: it compiles, links inside C++, and leaves the C
# symbol undefined.  Catch it here rather than in Margo's link line.
add_custom_command(TARGET abt_static POST_BUILD
    COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/cmake/check_c_symbols.sh"
            "$<TARGET_FILE:abt_static>"
    COMMENT "Checking ABT_* symbols have C linkage (no C++ mangling)"
    VERBATIM)

# ------------------------------------------------------------------- install
include(GNUInstallDirs)

install(TARGETS abt_static abt_shared
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(FILES include/abt.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

# Extra libraries a static consumer must add by hand.
set(ABT_LIBS_PRIVATE "-L${RECONVERSE_LIBDIR} -lreconverse")
get_filename_component(_hwloc_dir "${HWLOC_LIBRARY}" DIRECTORY)
if(APPLE)
  string(APPEND ABT_LIBS_PRIVATE " -L${_hwloc_dir} -lhwloc -lpthread -lc++")
else()
  string(APPEND ABT_LIBS_PRIVATE " -L${_hwloc_dir} -lhwloc -lpthread -lstdc++")
endif()

set(ABT_PC_VERSION 1.2.0)
configure_file(argobots.pc.in "${CMAKE_CURRENT_BINARY_DIR}/argobots.pc" @ONLY)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/argobots.pc"
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)

# ---------------------------------------------------------------------- tests
enable_testing()
add_subdirectory(tests)
