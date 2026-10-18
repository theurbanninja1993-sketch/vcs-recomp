# Builder support: reading the player's own disc.
#
# A disc arrives as one of four things: a plain 2048-byte .iso, a raw 2352-byte
# .bin (usually with a .cue beside it), or a .chd. The first three are read
# directly; CHD is compressed and needs a decompressor, so libchdr is fetched for
# it here rather than in the root, where every other dependency of the game lives.
#
# Host tools only: ps2x_iso is never built for Windows - the cross-build makes
# ps2EntryRunner - and a codec stack that is never run on the target is not worth
# the configure time or a configure that can fail. So a cross-compile skips it.
option(PS2X_ISO_CHD "Read CHD compressed disc images with ps2x_iso" ON)

add_library(ps2x_builder STATIC src/ps2x_iso9660.cpp)
target_include_directories(ps2x_builder PUBLIC include)
target_compile_features(ps2x_builder PUBLIC cxx_std_20)

if(PS2X_ISO_CHD AND NOT CMAKE_CROSSCOMPILING)
    include(FetchContent)

    # libchdr brings its own lzma, miniz and (single-header) dr_flac, so it needs
    # nothing installed, and being pure C it builds anywhere the tools do. Pinned
    # to a commit rather than to v0.3.0 because the fixes after that release
    # include hardening against malformed CHD files.
    set(PS2X_LIBCHDR_REV 47d794151b2b64f6aba60304e6480f4f5a2bc24b CACHE STRING
        "libchdr revision to build ps2x_iso's CHD support from")
    set(CHDR_WANT_TESTS OFF CACHE BOOL "libchdr: build its tests" FORCE)
    set(CHDR_WANT_RAW_DATA_SECTOR ON CACHE BOOL "libchdr: output ECC and sync" FORCE)
    set(CHDR_WANT_SUBCODE ON CACHE BOOL "libchdr: output CD subchannel data" FORCE)

    FetchContent_Declare(libchdr
        GIT_REPOSITORY https://github.com/rtissera/libchdr.git
        GIT_TAG ${PS2X_LIBCHDR_REV})
    FetchContent_MakeAvailable(libchdr)

    # chdr-static is defined whether or not the shared library is.
    target_compile_definitions(ps2x_builder PRIVATE PS2X_ISO_CHD=1)
    target_link_libraries(ps2x_builder PRIVATE chdr-static)
endif()

add_executable(ps2x_iso tools/ps2x_iso.cpp)
target_link_libraries(ps2x_iso PRIVATE ps2x_builder)
FetchContent_GetProperties(xxhash)
if(xxhash_SOURCE_DIR)
    target_include_directories(ps2x_iso PRIVATE "${xxhash_SOURCE_DIR}")
endif()
