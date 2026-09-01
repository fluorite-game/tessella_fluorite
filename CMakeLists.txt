# Native build for the tessella_fluorite package.
#
# Two things have to agree on a C++ runtime here, and only two: Filament and this. Filament's
# static archives are built against libc++, so the whole link is clang + libc++.
#
# What is *not* in this list is the point of it. `maplibre_fluorite` links mbgl-core and its
# vendored glslang, harfbuzz, freetype, nunicode and sqlite — and mbgl-core is built with
# GCC/libstdc++, which is why that build cannot link its own test probes without pulling
# libstdc++ in beside libc++. tessella is a Rust staticlib with no C++ runtime of its own, so
# the mismatch does not arise.
#
# Required cache vars:
#   TESSELLA_DIR                 tessella checkout (for the generated capture ABI header)
#   TESSELLA_LIB                 libtessella_ffi.a, from `cargo build -p tessella-ffi`
#   FILAMENT_INCLUDE_DIR         Filament headers
#   FILAMENT_LINK_LIBRARIES_DIR  Filament .a files
#   FLUORITE_CORE_FFI_LIB        libfluorite_core_ffi.so, optional
#
# FLUORITE_CORE_FFI_LIB is optional and load-bearing when set: it makes this take Filament
# *from* fluorite rather than linking its own copy. Two copies of Filament in one process hand
# the caller different globals from the Engine it was given, which crashes inside the material
# descriptor tables rather than at the seam.

cmake_minimum_required(VERSION 3.24)

if (NOT CMAKE_C_COMPILER)
    find_program(CLANG_C clang REQUIRED)
    set(CMAKE_C_COMPILER ${CLANG_C})
endif ()
if (NOT CMAKE_CXX_COMPILER)
    find_program(CLANG_CXX clang++ REQUIRED)
    set(CMAKE_CXX_COMPILER ${CLANG_CXX})
endif ()

project(tessella_fluorite CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
add_compile_options(-stdlib=libc++ -Wall -Wextra -Werror)
add_link_options(-stdlib=libc++)

if (NOT TESSELLA_DIR)
    message(FATAL_ERROR "set TESSELLA_DIR to a tessella checkout")
endif ()
if (NOT TESSELLA_LIB)
    message(FATAL_ERROR "set TESSELLA_LIB to libtessella_ffi.a")
endif ()

add_library(tsf_consumer STATIC
        native/src/reader.cc
        native/src/drawlist.cc
        native/src/host.cc)

target_include_directories(tsf_consumer
        PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/native/include
        # The capture ABI: one flat header, generated from the same mbgl declarations the
        # producer packs against and asserted field by field against them.
        ${TESSELLA_DIR}/include
        ${FILAMENT_INCLUDE_DIR})

target_link_libraries(tsf_consumer PUBLIC ${TESSELLA_LIB})
