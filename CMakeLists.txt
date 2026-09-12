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

# Every path above may be given relative to this package, and is resolved against
# it here. A build driven from a manifest cannot write an absolute one: emb's
# sysroot and build directories carry a profile hash that moves whenever the
# manifest does, and cmake resolves a relative -D against its own build dir,
# which is somewhere else again.
foreach (_tsf_path TESSELLA_DIR TESSELLA_LIB FILAMENT_INCLUDE_DIR FLUORITE_INCLUDE_DIR
        FLUORITE_CORE_FFI_LIB FILAMENT_LINK_LIBRARIES_DIR)
    if (${_tsf_path} AND NOT IS_ABSOLUTE "${${_tsf_path}}")
        get_filename_component(${_tsf_path} "${${_tsf_path}}"
                ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
    endif ()
endforeach ()

# The Filament headers, from the sysroot when nobody named them. A cross build
# has them installed under CMAKE_SYSROOT, which find_path searches and a
# hardcoded path cannot name.
if (NOT FILAMENT_INCLUDE_DIR)
    find_path(FILAMENT_INCLUDE_DIR filament/Engine.h)
    if (FILAMENT_INCLUDE_DIR)
        message(STATUS "filament headers: ${FILAMENT_INCLUDE_DIR} (found)")
    endif ()
endif ()

if (NOT TESSELLA_DIR)
    message(FATAL_ERROR "set TESSELLA_DIR to a tessella checkout")
endif ()
if (NOT TESSELLA_LIB)
    message(FATAL_ERROR "set TESSELLA_LIB to libtessella_ffi.a")
endif ()

add_library(tsf_consumer STATIC
        native/src/reader.cc
        native/src/drawlist.cc
        native/src/host.cc
        native/src/filament_renderer.cc
        native/src/map_view.cc)

target_include_directories(tsf_consumer
        PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/native/include
        # The capture ABI: one flat header, generated from the same mbgl declarations the
        # producer packs against and asserted field by field against them.
        ${TESSELLA_DIR}/include
        ${FILAMENT_INCLUDE_DIR})

target_link_libraries(tsf_consumer PUBLIC ${TESSELLA_LIB})

# The Dart-facing half: a map in every Fluorite platform view.
#
# Built only when FLUORITE_INCLUDE_DIR is given, because the extension ABI is
# fluorite's header and there is nothing to register against without it. The
# probes in native/test need none of this, which is why it is not the default.
#
# Linked against libfluorite_core_ffi.so, which is where both Filament and
# `fluorite_set_view_extension` come from.
#
# Leaving them undefined does not work, and the way it fails is worth writing
# down. Dart's native-assets loader dlopens each asset with RTLD_LOCAL, so
# fluorite's symbols are not in the global scope when this library is loaded,
# and the first call into it dies with `undefined symbol:
# fluorite_set_view_extension` -- after the app has started, from inside the
# loader, with no way to catch it.
#
# A DT_NEEDED plus an $ORIGIN rpath is what fixes it: the two libraries sit in
# the same bundle directory, so the loader resolves the soname to the file Dart
# has already mapped. Same path, same inode, one copy -- which is the property
# that matters, because a second Filament in the process hands the caller
# different globals from the Engine it was given.
if (FLUORITE_INCLUDE_DIR)
    add_library(tessella_fluorite SHARED native/src/extension.cc)
    target_include_directories(tessella_fluorite PRIVATE ${FLUORITE_INCLUDE_DIR})
    target_link_libraries(tessella_fluorite PRIVATE tsf_consumer)
    set_target_properties(tessella_fluorite PROPERTIES
            CXX_VISIBILITY_PRESET hidden
            VISIBILITY_INLINES_HIDDEN ON)
    if (FLUORITE_CORE_FFI_LIB)
        target_link_libraries(tessella_fluorite PRIVATE ${FLUORITE_CORE_FFI_LIB})
        set_target_properties(tessella_fluorite PROPERTIES
                BUILD_RPATH "$ORIGIN"
                INSTALL_RPATH "$ORIGIN"
                BUILD_WITH_INSTALL_RPATH ON)
        # Every filament:: and utils:: symbol this leaves undefined is one
        # fluorite exports, so there should be nothing else outstanding. Say so
        # at link time rather than at the first call.
        target_link_options(tessella_fluorite PRIVATE -Wl,--no-undefined)
    else ()
        message(WARNING "FLUORITE_CORE_FFI_LIB unset: tessella_fluorite will not "
                "resolve fluorite's symbols at runtime under Dart's loader")
    endif ()
endif ()
