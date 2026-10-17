cmake_minimum_required(VERSION 3.20)

project(PS2Runtime VERSION 0.1.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(PS2X_ENABLE_RUNNER_UNITY_BUILD "Build ps2EntryRunner with CMake unity build" ON)
set(PS2X_RUNNER_UNITY_BUILD_BATCH_SIZE 32 CACHE STRING "Unity build batch size for ps2EntryRunner")
option(PS2X_ENABLE_RUNNER_PCH "Precompile the heavy runtime headers for ps2EntryRunner" ON)
option(PS2X_ENABLE_SCCACHE "Use sccache as compiler launcher when available" ON)

option(PS2X_ENABLE_RUNTIME_LOGS "Enable PS2 runtime logs" ON)
option(PS2X_ENABLE_AGRESSIVE_LOGS "Enable very verbose/agressive PS2 runtime logs" ON)
option(PS2X_ENABLE_FUNCTION_LOG_TRACKER "Log every recompiled function entry/exit to ps2_log.txt (very slow, GBs per minute)" OFF)
option(PS2X_ENABLE_IOP_RPC_TRACE "Log unhandled IOP/SIF RPC trace suggestions" ON)
option(PS2X_STRICT_RETURN_DIAGNOSTICS "Route generated JR $ra returns through runtime branch diagnostics" OFF)
option(PS2X_SHOW_WINDOWS_CONSOLE "Show a console window for ps2EntryRunner on Windows release builds" ON)
option(PS2X_ENABLE_DEBUG_UI "Build the desktop runtime debug UI" ON)

if(PS2X_ENABLE_SCCACHE)
    find_program(PS2X_SCCACHE_PROGRAM sccache)

    if(PS2X_SCCACHE_PROGRAM)
        if(CMAKE_C_COMPILER)
            set(CMAKE_C_COMPILER_LAUNCHER "${PS2X_SCCACHE_PROGRAM}")
        endif()

        set(CMAKE_CXX_COMPILER_LAUNCHER "${PS2X_SCCACHE_PROGRAM}")
        message(STATUS "Using sccache: ${PS2X_SCCACHE_PROGRAM}")
    else()
        message(STATUS "sccache not found; continuing without compiler launcher")
    endif()
endif()

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/ReleaseMode.cmake")

set(PS2X_IS_VITA OFF)

if((CMAKE_C_COMPILER MATCHES "arm-vita-eabi") OR
    (CMAKE_CXX_COMPILER MATCHES "arm-vita-eabi"))
    set(PS2X_IS_VITA ON)
endif()

set(PS2X_IS_ANDROID OFF)

if(ANDROID)
    set(PS2X_IS_ANDROID ON)
endif()

option(PS2X_VITA_CREATE_PACKAGE "Create Vita eboot/vpk targets" ON)
set(PS2X_VITA_APP_NAME "PS2 Retro X" CACHE STRING "Display name for the Vita bubble")
set(PS2X_VITA_TITLEID "RANJ00001" CACHE STRING "9-character Vita title id")
set(PS2X_VITA_VERSION "01.00" CACHE STRING "Vita app version")
set(PS2X_DEFAULT_BOOT_ELF "" CACHE STRING "Guest ELF path passed directly to main() when argv is unavailable")

if(PS2X_IS_VITA)
    if(NOT DEFINED ENV{VITASDK})
        message(FATAL_ERROR "VITASDK is not defined. Configure inside the VitaSDK environment or pass the Vita toolchain file.")
    endif()

    set(PS2X_VITASDK "$ENV{VITASDK}")

    if(NOT EXISTS "${PS2X_VITASDK}/share/vita.cmake")
        message(FATAL_ERROR "Could not find vita.cmake under ${PS2X_VITASDK}/share.")
    endif()

    include("${PS2X_VITASDK}/share/vita.cmake" REQUIRED)
else()
    include(FetchContent)
    set(FETCHCONTENT_QUIET FALSE)
    set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(BUILD_GAMES OFF CACHE BOOL "" FORCE)

    if(PS2X_IS_ANDROID)
        set(PLATFORM "Android" CACHE STRING "" FORCE)
    endif()

    FetchContent_Declare(
        raylib
        GIT_REPOSITORY "https://github.com/raysan5/raylib.git"
        GIT_TAG "5.5" # we will migrate to 4.2.0 later, trust me it will be better
        GIT_PROGRESS TRUE
    )
    FetchContent_MakeAvailable(raylib)

    if(PS2X_ENABLE_DEBUG_UI AND NOT PS2X_IS_ANDROID)
        FetchContent_Declare(
            imgui
            GIT_REPOSITORY https://github.com/ocornut/imgui.git
            GIT_TAG "v1.92.7-docking"
            GIT_SHALLOW TRUE
        )
        FetchContent_GetProperties(imgui)

        if(NOT imgui_POPULATED)
            FetchContent_Populate(imgui)
        endif()

        set(PS2X_IMGUI_SOURCE_DIR "${imgui_SOURCE_DIR}")

        add_library(imgui STATIC
            "${PS2X_IMGUI_SOURCE_DIR}/imgui.cpp"
            "${PS2X_IMGUI_SOURCE_DIR}/imgui_draw.cpp"
            "${PS2X_IMGUI_SOURCE_DIR}/imgui_tables.cpp"
            "${PS2X_IMGUI_SOURCE_DIR}/imgui_widgets.cpp"
            "${PS2X_IMGUI_SOURCE_DIR}/imgui_demo.cpp"
        )
        target_include_directories(imgui PUBLIC "${PS2X_IMGUI_SOURCE_DIR}")

        FetchContent_Declare(
            rlImGui
            GIT_REPOSITORY https://github.com/raylib-extras/rlImGui.git
            GIT_TAG "Raylib_5_5"
            GIT_SHALLOW TRUE
        )
        FetchContent_GetProperties(rlImGui)

        if(NOT rlimgui_POPULATED)
            FetchContent_Populate(rlImGui)
        endif()

        set(PS2X_RLIMGUI_SOURCE_DIR "${rlimgui_SOURCE_DIR}")

        # rlImGui's gamepad mapping swaps Square/Circle (B and X) and Start/Back;
        # build a corrected copy so menu navigation uses Circle to go back.
        file(READ "${PS2X_RLIMGUI_SOURCE_DIR}/rlImGui.cpp" PS2X_RLIMGUI_CPP)
        string(REPLACE "GAMEPAD_BUTTON_RIGHT_FACE_RIGHT, ImGuiKey_GamepadFaceLeft" "GAMEPAD_BUTTON_RIGHT_FACE_RIGHT, ImGuiKey_GamepadFaceRight"
               PS2X_RLIMGUI_CPP "${PS2X_RLIMGUI_CPP}")
        string(REPLACE "GAMEPAD_BUTTON_RIGHT_FACE_LEFT, ImGuiKey_GamepadFaceRight" "GAMEPAD_BUTTON_RIGHT_FACE_LEFT, ImGuiKey_GamepadFaceLeft"
               PS2X_RLIMGUI_CPP "${PS2X_RLIMGUI_CPP}")
        string(REPLACE "GAMEPAD_BUTTON_MIDDLE_LEFT, ImGuiKey_GamepadStart" "GAMEPAD_BUTTON_MIDDLE_LEFT, ImGuiKey_GamepadBack"
               PS2X_RLIMGUI_CPP "${PS2X_RLIMGUI_CPP}")
        string(REPLACE "GAMEPAD_BUTTON_MIDDLE_RIGHT, ImGuiKey_GamepadBack" "GAMEPAD_BUTTON_MIDDLE_RIGHT, ImGuiKey_GamepadStart"
               PS2X_RLIMGUI_CPP "${PS2X_RLIMGUI_CPP}")
        # ...and read the first slot holding a real gamepad, not always slot 0.
        string(REPLACE "IsGamepadAvailable(0)" "IsGamepadAvailable(ps2xImGuiPad())" PS2X_RLIMGUI_CPP "${PS2X_RLIMGUI_CPP}")
        string(REPLACE "IsGamepadButtonPressed(0, " "IsGamepadButtonPressed(ps2xImGuiPad(), " PS2X_RLIMGUI_CPP "${PS2X_RLIMGUI_CPP}")
        string(REPLACE "IsGamepadButtonReleased(0, " "IsGamepadButtonReleased(ps2xImGuiPad(), " PS2X_RLIMGUI_CPP "${PS2X_RLIMGUI_CPP}")
        string(REPLACE "GetGamepadAxisMovement(0, " "GetGamepadAxisMovement(ps2xImGuiPad(), " PS2X_RLIMGUI_CPP "${PS2X_RLIMGUI_CPP}")
        string(REPLACE "#include \"rlImGui.h\"" "#include \"rlImGui.h\"\nint (*g_ps2xImGuiPadSelector)() = nullptr;\nstatic int ps2xImGuiPad() { if (g_ps2xImGuiPadSelector) { const int pad = g_ps2xImGuiPadSelector(); return pad < 0 ? 0 : pad; } for (int i = 0; i < 4; ++i) if (IsGamepadAvailable(i)) return i; return 0; }"
               PS2X_RLIMGUI_CPP "${PS2X_RLIMGUI_CPP}")
        set(PS2X_RLIMGUI_PATCHED "${CMAKE_CURRENT_BINARY_DIR}/rlImGui_patched.cpp")
        file(CONFIGURE OUTPUT "${PS2X_RLIMGUI_PATCHED}" CONTENT "${PS2X_RLIMGUI_CPP}" @ONLY)

        add_library(rlImGui STATIC
            "${PS2X_RLIMGUI_PATCHED}"
        )
        target_include_directories(rlImGui PUBLIC "${PS2X_RLIMGUI_SOURCE_DIR}")
        target_link_libraries(rlImGui PUBLIC raylib imgui)
    endif()
endif()

add_library(ps2_host_backend INTERFACE)

set(PS2X_DEFAULT_BOOT_ELF_DEFINE "")

if(PS2X_DEFAULT_BOOT_ELF)
    set(PS2X_DEFAULT_BOOT_ELF_DEFINE "${PS2X_DEFAULT_BOOT_ELF}")
    string(REPLACE "\\" "\\\\" PS2X_DEFAULT_BOOT_ELF_DEFINE "${PS2X_DEFAULT_BOOT_ELF_DEFINE}")
    string(REPLACE "\"" "\\\"" PS2X_DEFAULT_BOOT_ELF_DEFINE "${PS2X_DEFAULT_BOOT_ELF_DEFINE}")
endif()

if(PS2X_IS_VITA)
    set(PS2X_VITA_PREFIX "${PS2X_VITASDK}/arm-vita-eabi")
    set(PS2X_VITA_EXTRA_INCLUDE_DIRS "" CACHE STRING "Extra include directories for Vita host dependencies")
    set(PS2X_VITA_LIBRARY_DIR "${PS2X_VITA_PREFIX}/lib")
    set(PS2X_VITA_REQUIRED_LIBRARY_NAMES
        raylib
        SDL2
        OpenSLES
        taihen_stub
        SceAppMgr_stub
        SceCtrl_stub
        SceGxm_stub
        SceCommonDialog_stub
        SceLibKernel_stub
        SceAudio_stub
        SceTouch_stub
        SceHid_stub
        SceMotion_stub
        SceSysmodule_stub
        SceIofilemgr_stub
        SceNetCtl_stub
        SceNet_stub
        SceDisplay_stub
        SceAppUtil_stub
        SceAudioIn_stub
        ScePower_stub
        SceProcessmgr_stub
        SceIme_stub
        libIMGEGL_stub_weak
        libgpu_es4_ext_stub_weak
        libGLESv2_stub_weak
        libGL_stub_weak
        libGLESv1_CM_stub_weak
    )

    set(PS2X_VITA_INCLUDE_DIRS
        "${PS2X_VITA_PREFIX}/include"
    )

    foreach(PS2X_VITA_OPTIONAL_INCLUDE_DIR IN ITEMS
        "${PS2X_VITA_PREFIX}/include/raylib"
        "${PS2X_VITA_PREFIX}/include/SDL2")
        if(EXISTS "${PS2X_VITA_OPTIONAL_INCLUDE_DIR}")
            list(APPEND PS2X_VITA_INCLUDE_DIRS "${PS2X_VITA_OPTIONAL_INCLUDE_DIR}")
        endif()
    endforeach()

    if(PS2X_VITA_EXTRA_INCLUDE_DIRS)
        list(APPEND PS2X_VITA_INCLUDE_DIRS ${PS2X_VITA_EXTRA_INCLUDE_DIRS})
    endif()

    if(NOT EXISTS "${PS2X_VITA_PREFIX}/include/SDL2/SDL.h")
        message(FATAL_ERROR
            "Missing SDL2 headers under ${PS2X_VITA_PREFIX}/include/SDL2. "
            "Quenom/raylib-5.5-vita requires SDL2 with PVR support installed into VitaSDK.")
    endif()

    if(NOT EXISTS "${PS2X_VITA_LIBRARY_DIR}")
        message(FATAL_ERROR "Missing Vita library directory: ${PS2X_VITA_LIBRARY_DIR}")
    endif()

    function(ps2x_find_vita_library out_var library_name)
        string(MAKE_C_IDENTIFIER "${library_name}" library_id)
        set(cache_var "PS2X_VITA_LIB_${library_id}")

        find_library(${cache_var}
            NAMES "${library_name}" "${library_name}.a" "lib${library_name}.a"
            PATHS "${PS2X_VITA_LIBRARY_DIR}"
            NO_DEFAULT_PATH
        )

        if(NOT ${cache_var})
            message(FATAL_ERROR
                "Could not find Vita library '${library_name}' under ${PS2X_VITA_LIBRARY_DIR}. "
                "Install Quenom/raylib-5.5-vita and its SDL2/PVR dependencies into VitaSDK first.")
        endif()

        set(${out_var} "${${cache_var}}" PARENT_SCOPE)
    endfunction()

    set(PS2X_VITA_RESOLVED_LIBRARIES "")

    foreach(PS2X_VITA_LIBRARY_NAME IN LISTS PS2X_VITA_REQUIRED_LIBRARY_NAMES)
        ps2x_find_vita_library(PS2X_VITA_LIBRARY_PATH "${PS2X_VITA_LIBRARY_NAME}")
        list(APPEND PS2X_VITA_RESOLVED_LIBRARIES "${PS2X_VITA_LIBRARY_PATH}")
    endforeach()

    target_compile_definitions(ps2_host_backend INTERFACE PLATFORM_VITA)
    target_include_directories(ps2_host_backend INTERFACE ${PS2X_VITA_INCLUDE_DIRS})
    target_link_libraries(ps2_host_backend INTERFACE
        ${PS2X_VITA_RESOLVED_LIBRARIES}
        m
        c
        pthread
    )
else()
    target_link_libraries(ps2_host_backend INTERFACE raylib)

    if(PS2X_IS_ANDROID)
        target_link_libraries(ps2_host_backend INTERFACE log android)
    endif()
endif()

if(PS2X_IS_ANDROID)
    set(PS2X_ENABLE_FFMPEG_DEFAULT OFF)
else()
    set(PS2X_ENABLE_FFMPEG_DEFAULT ON)
endif()

option(PS2X_ENABLE_FFMPEG "Enable FFmpeg-backed MPEG video decoding" ${PS2X_ENABLE_FFMPEG_DEFAULT})

if(NOT PS2X_ENABLE_FFMPEG)
    message(STATUS "FFmpeg disabled; MPEG video decode falls back to stub frames")
    add_library(ffmpeg INTERFACE)
elseif(WIN32)
    include(ExternalProject)

    set(FFMPEG_PREBUILT_TAG "n7.1-241205")
    set(FFMPEG_PREBUILT_URL
        "https://github.com/System233/ffmpeg-msvc-prebuilt/releases/download/${FFMPEG_PREBUILT_TAG}/ffmpeg-${FFMPEG_PREBUILT_TAG}-lgpl-amd64-shared.zip")

    set(FFMPEG_PREFIX_DIR "${CMAKE_BINARY_DIR}/ThirdParty/ffmpeg-prefix")
    set(FFMPEG_SOURCE_DIR "${FFMPEG_PREFIX_DIR}/src/ffmpeg_external")
    set(FFMPEG_INCLUDE_DIR "${FFMPEG_SOURCE_DIR}/include")
    set(FFMPEG_LIB_DIR "${FFMPEG_SOURCE_DIR}/bin")
    set(FFMPEG_BIN_DIR "${FFMPEG_SOURCE_DIR}/bin")

    ExternalProject_Add(ffmpeg_external
        URL "${FFMPEG_PREBUILT_URL}"
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        PREFIX "${FFMPEG_PREFIX_DIR}"
        CONFIGURE_COMMAND ""
        BUILD_COMMAND ""
        INSTALL_COMMAND ""
        BUILD_BYPRODUCTS
        "${FFMPEG_LIB_DIR}/avcodec.lib"
        "${FFMPEG_LIB_DIR}/avformat.lib"
        "${FFMPEG_LIB_DIR}/avutil.lib"
        "${FFMPEG_LIB_DIR}/swresample.lib"
        "${FFMPEG_LIB_DIR}/swscale.lib"
    )

    # Link against the import libraries shipped with the Windows DLLs.
    add_library(ffmpeg_avcodec STATIC IMPORTED GLOBAL)
    set_target_properties(ffmpeg_avcodec PROPERTIES
        IMPORTED_LOCATION "${FFMPEG_LIB_DIR}/avcodec.lib"
        INTERFACE_INCLUDE_DIRECTORIES "${FFMPEG_INCLUDE_DIR}"
    )
    add_dependencies(ffmpeg_avcodec ffmpeg_external)

    add_library(ffmpeg_avformat STATIC IMPORTED GLOBAL)
    set_target_properties(ffmpeg_avformat PROPERTIES
        IMPORTED_LOCATION "${FFMPEG_LIB_DIR}/avformat.lib"
        INTERFACE_INCLUDE_DIRECTORIES "${FFMPEG_INCLUDE_DIR}"
    )
    add_dependencies(ffmpeg_avformat ffmpeg_external)

    add_library(ffmpeg_avutil STATIC IMPORTED GLOBAL)
    set_target_properties(ffmpeg_avutil PROPERTIES
        IMPORTED_LOCATION "${FFMPEG_LIB_DIR}/avutil.lib"
        INTERFACE_INCLUDE_DIRECTORIES "${FFMPEG_INCLUDE_DIR}"
    )
    add_dependencies(ffmpeg_avutil ffmpeg_external)

    add_library(ffmpeg_swresample STATIC IMPORTED GLOBAL)
    set_target_properties(ffmpeg_swresample PROPERTIES
        IMPORTED_LOCATION "${FFMPEG_LIB_DIR}/swresample.lib"
        INTERFACE_INCLUDE_DIRECTORIES "${FFMPEG_INCLUDE_DIR}"
    )
    add_dependencies(ffmpeg_swresample ffmpeg_external)

    add_library(ffmpeg_swscale STATIC IMPORTED GLOBAL)
    set_target_properties(ffmpeg_swscale PROPERTIES
        IMPORTED_LOCATION "${FFMPEG_LIB_DIR}/swscale.lib"
        INTERFACE_INCLUDE_DIRECTORIES "${FFMPEG_INCLUDE_DIR}"
    )
    add_dependencies(ffmpeg_swscale ffmpeg_external)

    add_library(ffmpeg INTERFACE)
    target_link_libraries(ffmpeg INTERFACE
        ffmpeg_avcodec
        ffmpeg_avformat
        ffmpeg_avutil
        ffmpeg_swresample
        ffmpeg_swscale
        bcrypt
        secur32
        ws2_32
        user32
        advapi32
        ole32
        shell32
    )

    set(PS2X_FFMPEG_BIN_DIR "${FFMPEG_BIN_DIR}" CACHE INTERNAL "Directory containing FFmpeg runtime DLLs")
    set(PS2X_COPY_FFMPEG_DLLS_SCRIPT
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/CopyFfmpegDlls.cmake"
        CACHE INTERNAL
        "Script used to stage FFmpeg runtime DLLs beside executables")
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET GLOBAL
        libavcodec
        libavformat
        libavutil
        libswresample
        libswscale
    )

    add_library(ffmpeg INTERFACE)
    target_link_libraries(ffmpeg INTERFACE PkgConfig::FFMPEG)
endif()

function(ps2x_stage_ffmpeg_runtime_dlls target_name)
    if(NOT WIN32)
        return()
    endif()

    if(NOT PS2X_FFMPEG_BIN_DIR OR NOT PS2X_COPY_FFMPEG_DLLS_SCRIPT)
        return()
    endif()

    if(TARGET ffmpeg_external)
        add_dependencies(${target_name} ffmpeg_external)
    endif()

    add_custom_command(
        TARGET ${target_name} POST_BUILD
        COMMAND "${CMAKE_COMMAND}"
        "-Dsource_dir=${PS2X_FFMPEG_BIN_DIR}"
        "-Ddest_dir=$<TARGET_FILE_DIR:${target_name}>"
        -P "${PS2X_COPY_FFMPEG_DLLS_SCRIPT}"
        VERBATIM
    )
endfunction()

add_library(ps2_runtime STATIC
    src/lib/ps2_platform.cpp
    src/lib/ps2_music_station.cpp
    src/lib/game_overrides.cpp
    src/lib/gs/ps2_gif_arbiter.cpp
    src/lib/ps2_audio.cpp
    src/lib/ps2_audio_vag.cpp
    src/lib/gs/ps2_gs_memory.cpp
    src/lib/gs/gs_frontend.cpp
    src/lib/gs/gs_cpu_backend.cpp
    src/lib/gs/gs_threaded_backend.cpp
    src/lib/gs/gs_gpu_backend.cpp
    src/lib/gs/gpu/gl_context.cpp
    src/lib/gs/gpu/gs_gpu_renderer.cpp
    src/lib/gs/gs_texture_replacements.cpp
    src/lib/ps2_iop_host.cpp
    src/lib/ps2_iop_worker.cpp
    src/lib/ps2_memory.cpp
    src/lib/ps2_host_settings.cpp
    src/lib/ps2_camera.cpp
    src/lib/ps2_mods.cpp
    src/lib/ps2_pad.cpp
    src/lib/ps2_pad_rumble.cpp
    src/lib/ps2_prompt_art.cpp
    src/lib/ps2_rom_device.cpp
    src/lib/ps2_vfs.cpp
    src/lib/ps2_runtime.cpp
    src/lib/ps2_vif1_interpreter.cpp
    src/lib/vu/ps2_vu1_core.cpp
    src/lib/vu/ps2_vu1_upper.cpp
    src/lib/vu/ps2_vu1_lower.cpp
    src/lib/vu/ps2_vu1_programs.cpp
    src/lib/games_database.cpp
)

# Host-CPU code generation for the runtime/IOP libraries (VU1 interpreter, GS
# rasterizer, SPU2). FP contraction stays off so FMA can't change emulated
# float results (verified with tools/vu1_replay).
option(PS2X_NATIVE_ARCH "Compile the runtime libraries for the build machine's CPU" OFF)
if(PS2X_NATIVE_ARCH AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(ps2_runtime PRIVATE -march=native -ffp-contract=off)
    if(TARGET ps2_iop)
        target_compile_options(ps2_iop PRIVATE -march=native -ffp-contract=off)
    endif()
endif()

# Profile-guided optimisation for the runtime/IOP libraries only (the
# generated game TUs are too large to instrument usefully).
#   PS2X_PGO=generate -> run tools/vu1_replay + a gameplay session, then
#   PS2X_PGO=use      -> rebuild with the collected profile in PS2X_PGO_DIR.
set(PS2X_PGO "off" CACHE STRING "Runtime PGO mode: off, generate or use")
set(PS2X_PGO_DIR "${CMAKE_BINARY_DIR}/pgo" CACHE PATH "Runtime PGO profile directory")
# PGO for both compilers this project builds with. The gate here used to be GCC-only,
# which meant the one platform with a performance problem - Windows, built with clang -
# was the one platform that could not be profiled at all.
#
#   GCC   : -fprofile-generate=<dir> writes .gcda into the directory and
#           -fprofile-use=<dir> reads them.
#   clang : -fprofile-instr-generate writes .profraw where LLVM_PROFILE_FILE points at
#           run time; -fprofile-instr-use wants the merged file, so collect with
#           LLVM_PROFILE_FILE=<PS2X_PGO_DIR>/%p.profraw, run tools/make_pgo_profile.sh,
#           then rebuild. llvm-mingw ships both the profile runtime and llvm-profdata.
#
# A profile is platform specific. The clang route usually transfers between an x86-64
# host build and the llvm-mingw cross build of the same source, but the honest profile
# is the one collected by running the game on the machine being built for.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    set(PS2X_PGO_GENERATE_FLAGS -fprofile-generate=${PS2X_PGO_DIR} -fprofile-update=single)
    set(PS2X_PGO_GENERATE_LINK_FLAGS -fprofile-generate=${PS2X_PGO_DIR})
    set(PS2X_PGO_USE_FLAGS -fprofile-use=${PS2X_PGO_DIR} -fprofile-correction -fprofile-partial-training -Wno-missing-profile -Wno-coverage-mismatch)
elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    # -fprofile-generate, not -fprofile-instr-generate. The front-end mode registers
    # its counters object by object through constructors, and on the llvm-mingw cross
    # build those registrations do not survive into the executable: the profile
    # runtime links, initialises, creates the file, and then reports "Total
    # functions: 0". The IR mode puts its counters in a section the runtime walks
    # directly, so it does not depend on any of that. Same LLVM_PROFILE_FILE, same
    # llvm-profdata merge, same default.profdata.
    set(PS2X_PGO_GENERATE_FLAGS -fprofile-generate)
    set(PS2X_PGO_GENERATE_LINK_FLAGS -fprofile-generate)
    set(PS2X_PGO_USE_FLAGS -fprofile-use=${PS2X_PGO_DIR}/default.profdata -Wno-profile-instr-out-of-date -Wno-profile-instr-unprofiled)
endif()
if(NOT PS2X_PGO STREQUAL "off" AND DEFINED PS2X_PGO_GENERATE_FLAGS)
    foreach(PS2X_PGO_TARGET ps2_runtime ps2_iop)
        if(TARGET ${PS2X_PGO_TARGET})
            # Compiler caches don't hash profile data; always compile these.
            set_target_properties(${PS2X_PGO_TARGET} PROPERTIES CXX_COMPILER_LAUNCHER "" C_COMPILER_LAUNCHER "")
            if(PS2X_PGO STREQUAL "generate")
                target_compile_options(${PS2X_PGO_TARGET} PRIVATE ${PS2X_PGO_GENERATE_FLAGS})
                target_link_options(${PS2X_PGO_TARGET} INTERFACE ${PS2X_PGO_GENERATE_LINK_FLAGS})
            elseif(PS2X_PGO STREQUAL "use")
                target_compile_options(${PS2X_PGO_TARGET} PRIVATE ${PS2X_PGO_USE_FLAGS})
            endif()
        endif()
    endforeach()
endif()

if(PS2X_ENABLE_RUNTIME_LOGS)
    target_compile_definitions(ps2_runtime PUBLIC
        PS2_RUNTIME_LOGS=1
    )
endif()

if(PS2X_ENABLE_AGRESSIVE_LOGS)
    target_compile_definitions(ps2_runtime PUBLIC
        AGRESSIVE_LOGS=1
    )
endif()

if(PS2X_ENABLE_FUNCTION_LOG_TRACKER)
    target_compile_definitions(ps2_runtime PUBLIC
        PS2_FUNCTION_LOG_TRACKER=1
    )
endif()

if(PS2X_ENABLE_IOP_RPC_TRACE)
    target_compile_definitions(ps2_runtime PUBLIC
        PS2X_ENABLE_IOP_RPC_TRACE=1
    )
else()
    target_compile_definitions(ps2_runtime PUBLIC
        PS2X_ENABLE_IOP_RPC_TRACE=0
    )
endif()

if(PS2X_STRICT_RETURN_DIAGNOSTICS)
    target_compile_definitions(ps2_runtime PUBLIC
        PS2X_STRICT_RETURN_DIAGNOSTICS=1
    )
endif()

# Verbose guest/runtime logging for bring-up (scePrintf echo, stub traces).
# Set to 0 (or remove) for release builds.
target_compile_definitions(ps2_runtime PRIVATE
    PS2X_HAS_FFMPEG=$<BOOL:${PS2X_ENABLE_FFMPEG}>
    AGRESSIVE_LOGS=1
)

file(GLOB_RECURSE KERNEL_SRC_FILES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/lib/Kernel/*.cpp"
)

target_sources(ps2_runtime PRIVATE
    ${KERNEL_SRC_FILES}
)

file(GLOB RUNNER_SRC_FILES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/runner/*.cpp"
)

set(RUNNER_MAIN_CPP "${CMAKE_CURRENT_SOURCE_DIR}/src/runner/main.cpp")
set(ROOT_MAIN_CPP "${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp")

if(EXISTS "${RUNNER_MAIN_CPP}")
    list(APPEND RUNNER_SRC_FILES "${RUNNER_MAIN_CPP}")
elseif(EXISTS "${ROOT_MAIN_CPP}")
    list(APPEND RUNNER_SRC_FILES "${ROOT_MAIN_CPP}")
endif()

if(PS2X_IS_ANDROID)
    add_library(ps2EntryRunner SHARED
        ${RUNNER_SRC_FILES}
    )

    target_link_options(ps2EntryRunner PRIVATE "-Wl,--undefined=ANativeActivity_onCreate")
else()
    add_executable(ps2EntryRunner
        ${RUNNER_SRC_FILES}
    )
endif()

if(PS2X_ENABLE_RUNNER_UNITY_BUILD)
    set_target_properties(ps2EntryRunner PROPERTIES
        UNITY_BUILD ON
        UNITY_BUILD_BATCH_SIZE "${PS2X_RUNNER_UNITY_BUILD_BATCH_SIZE}"
    )
endif()

if(PS2X_ENABLE_RUNNER_PCH)
    set(PS2X_RUNNER_PCH_HEADERS
        "${CMAKE_CURRENT_SOURCE_DIR}/include/ps2_runtime_macros.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/ps2_runtime.h"
    )

    foreach(PS2X_RUNNER_GENERATED_HEADER IN ITEMS
        "${CMAKE_CURRENT_SOURCE_DIR}/include/ps2_recompiled_functions.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/ps2_recompiled_stubs.h")
        if(EXISTS "${PS2X_RUNNER_GENERATED_HEADER}")
            list(APPEND PS2X_RUNNER_PCH_HEADERS "${PS2X_RUNNER_GENERATED_HEADER}")
        endif()
    endforeach()

    list(APPEND PS2X_RUNNER_PCH_HEADERS
        "${CMAKE_CURRENT_SOURCE_DIR}/include/ps2_syscalls.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/ps2_stubs.h"
    )

    target_precompile_headers(ps2EntryRunner PRIVATE ${PS2X_RUNNER_PCH_HEADERS})
endif()

if(PS2X_DEFAULT_BOOT_ELF_DEFINE)
    target_compile_definitions(ps2EntryRunner PRIVATE
        PS2X_DEFAULT_BOOT_ELF="${PS2X_DEFAULT_BOOT_ELF_DEFINE}"
    )
endif()

if(MSVC)
    target_compile_options(ps2EntryRunner PRIVATE /FS)
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    # The generated game TUs are enormous; at -O3 single unity files can eat
    # 10GB+ of RAM each and OOM the build. -O2 compiles them with a fraction
    # of the memory at negligible runtime cost for this code shape.
    target_compile_options(ps2EntryRunner PRIVATE -O2)
endif()

target_include_directories(ps2_runtime PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src/lib/Kernel
)

# xxHash (header-only, XXH_INLINE_ALL): XXH3-64 texture hashes as PCSX2
# computes them (texture-pack replacement names).
include(FetchContent)
FetchContent_Declare(
    xxhash
    GIT_REPOSITORY https://github.com/Cyan4973/xxHash.git
    GIT_TAG v0.8.3
    GIT_SHALLOW TRUE
)
FetchContent_GetProperties(xxhash)
if(NOT xxhash_POPULATED)
    FetchContent_Populate(xxhash)
endif()
target_include_directories(ps2_runtime PRIVATE "${xxhash_SOURCE_DIR}")

target_link_libraries(ps2_runtime PUBLIC ps2_host_backend ffmpeg ps2_iop)
# GPU GS renderer: GL 4.5+ via EGL surfaceless (tools) or a GLX context sharing
# the host window's (the game, so the composed display texture can be drawn
# directly) on Linux / a hidden window's WGL context on Windows.
if(UNIX AND NOT APPLE AND NOT PS2X_IS_ANDROID)
    target_link_libraries(ps2_runtime PUBLIC EGL X11 GLX)
elseif(WIN32)
    target_link_libraries(ps2_runtime PUBLIC opengl32 gdi32)
endif()
target_link_libraries(ps2EntryRunner
    PRIVATE
    ps2_runtime
)

if(PS2X_ENABLE_DEBUG_UI AND NOT PS2X_IS_VITA AND NOT PS2X_IS_ANDROID)
    target_sources(ps2EntryRunner PRIVATE
        src/lib/ps2_debug_panel.cpp
    )
    target_link_libraries(ps2EntryRunner PRIVATE rlImGui imgui)
    target_compile_definitions(ps2EntryRunner PRIVATE PS2X_ENABLE_DEBUG_UI=1)
endif()

ps2x_stage_ffmpeg_runtime_dlls(ps2EntryRunner)

if(PS2X_IS_VITA)
    set(VITA_MKSFOEX_FLAGS "${VITA_MKSFOEX_FLAGS} -d PARENTAL_LEVEL=1") 
    set(VITA_MKSFOEX_FLAGS "${VITA_MKSFOEX_FLAGS} -d ATTRIBUTE2=12")

    if(PS2X_VITA_CREATE_PACKAGE)
        set(PS2X_VITA_VPK_FILES "")

        foreach(PS2X_VITA_MODULE IN ITEMS
            libgpu_es4_ext.suprx
            libIMGEGL.suprx
            libGLESv2.suprx
            libpvrPSP2_WSEGL.suprx
            libGL.suprx)
            set(PS2X_VITA_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/vita/module/${PS2X_VITA_MODULE}")

            if(EXISTS "${PS2X_VITA_MODULE_PATH}")
                list(APPEND PS2X_VITA_VPK_FILES FILE "${PS2X_VITA_MODULE_PATH}" "module/${PS2X_VITA_MODULE}")
            else()
                message(WARNING
                    "Missing ${PS2X_VITA_MODULE_PATH}; the vpk will rely on ur0:data/external/ for it")
            endif()
        endforeach()

        vita_create_self(eboot.bin ps2EntryRunner UNSAFE)
        vita_create_vpk(ps2EntryRunner.vpk ${PS2X_VITA_TITLEID} eboot.bin
            VERSION ${PS2X_VITA_VERSION}
            NAME ${PS2X_VITA_APP_NAME}
            ${PS2X_VITA_VPK_FILES}
        )
    endif()
endif()

if(CMAKE_BUILD_TYPE STREQUAL "Release" OR CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
    EnableFastReleaseMode(ps2_runtime)
    EnableFastReleaseMode(ps2EntryRunner)

    if(WIN32 AND NOT PS2X_SHOW_WINDOWS_CONSOLE)
        if(MSVC)
            target_link_options(ps2EntryRunner PRIVATE
                $<$<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>>:/SUBSYSTEM:WINDOWS>
                $<$<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>>:/ENTRY:mainCRTStartup>
            )
        elseif(MINGW)
            target_link_options(ps2EntryRunner PRIVATE
                $<$<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>>:-mwindows>
            )
        endif()
    endif()
endif()

# Work around WinAPI vs raylib symbol clash for CloseWindow on x64
if(MSVC)
    target_link_options(ps2EntryRunner PRIVATE "/FORCE:MULTIPLE")
endif()

# Vita: Work around for GCC 10's LTO plugin
if(PS2X_IS_VITA)
    target_link_options(ps2EntryRunner PRIVATE "-Wl,--allow-multiple-definition")
    target_sources(ps2EntryRunner PRIVATE src/lib/ps2_vita_runtime.cpp)
    set_source_files_properties(src/lib/ps2_vita_runtime.cpp PROPERTIES
        SKIP_UNITY_BUILD_INCLUSION TRUE
        COMPILE_OPTIONS "-fno-lto"
    )
endif()

if(PS2X_IS_ANDROID)
    target_sources(ps2EntryRunner PRIVATE src/lib/ps2_android_runtime.cpp)
endif()

install(TARGETS ps2_runtime ps2EntryRunner
    RUNTIME DESTINATION bin
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
)

if(WIN32 AND PS2X_ENABLE_FFMPEG)
    install(DIRECTORY "${FFMPEG_BIN_DIR}/"
        DESTINATION bin
        FILES_MATCHING
        PATTERN "*.dll"
    )
endif()

install(DIRECTORY include/
    DESTINATION include
)

# Offline VU1 replay benchmark (see tools/vu1_replay.cpp); not built by default.
if(NOT PS2X_IS_ANDROID AND NOT PS2X_IS_VITA)
    add_executable(vu1_replay EXCLUDE_FROM_ALL tools/vu1_replay.cpp)
    target_link_libraries(vu1_replay PRIVATE ps2_runtime)

    # VU1 microprogram recompiler (tools/vu1_recomp.cpp): generates C++ for
    # the programs in VU1 captures. PS2X_VU1_PROGRAMS names the generated
    # file to link into the runner and vu1_replay.
    add_executable(vu1_recomp EXCLUDE_FROM_ALL tools/vu1_recomp.cpp)
    target_link_libraries(vu1_recomp PRIVATE ps2_runtime)
    set(PS2X_VU1_PROGRAMS "" CACHE PATH "Directory of generated VU1 programs (vu1_recomp output)")
    if(PS2X_VU1_PROGRAMS AND EXISTS "${PS2X_VU1_PROGRAMS}/vu1_programs.cpp")
        file(GLOB PS2X_VU1_PROGRAM_SOURCES CONFIGURE_DEPENDS "${PS2X_VU1_PROGRAMS}/*.cpp")
        add_library(ps2_vu1_programs OBJECT ${PS2X_VU1_PROGRAM_SOURCES})
        target_include_directories(ps2_vu1_programs PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/lib/vu)
        target_link_libraries(ps2_vu1_programs PRIVATE ps2_runtime)
        # Same float semantics as the interpreter (FE_TOWARDZERO at run time).
        target_compile_options(ps2_vu1_programs PRIVATE -ffp-contract=off -frounding-math -O2)
        target_link_libraries(vu1_replay PRIVATE ps2_vu1_programs)
        if(TARGET ps2EntryRunner)
            target_link_libraries(ps2EntryRunner PRIVATE ps2_vu1_programs)
        endif()
    endif()

    # PS2X_VU1_PLUGIN_PROGRAMS: vu1_recomp output built as a plugin
    # (vu1_programs.so / .dll next to the runner) that the game loads at
    # startup - for programs learned with PS2X_VU1_LEARN, no relink needed.
    set(PS2X_VU1_PLUGIN_PROGRAMS "" CACHE PATH "Directory of generated VU1 programs to build as the startup plugin")
    if(PS2X_VU1_PLUGIN_PROGRAMS AND EXISTS "${PS2X_VU1_PLUGIN_PROGRAMS}/vu1_programs.cpp")
        file(GLOB PS2X_VU1_PLUGIN_SOURCES CONFIGURE_DEPENDS "${PS2X_VU1_PLUGIN_PROGRAMS}/*.cpp")
        add_library(ps2_vu1_plugin SHARED ${PS2X_VU1_PLUGIN_SOURCES} src/lib/vu/ps2_vu1_plugin_shim.cpp)
        # Headers only: the interpreter methods come from the game (shim).
        target_include_directories(ps2_vu1_plugin PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src/lib/vu
            $<TARGET_PROPERTY:ps2_runtime,INTERFACE_INCLUDE_DIRECTORIES>
            $<TARGET_PROPERTY:ps2_runtime,INCLUDE_DIRECTORIES>)
        target_compile_definitions(ps2_vu1_plugin PRIVATE $<TARGET_PROPERTY:ps2_runtime,INTERFACE_COMPILE_DEFINITIONS>)
        target_compile_options(ps2_vu1_plugin PRIVATE -ffp-contract=off -frounding-math -O2)
        set_target_properties(ps2_vu1_plugin PROPERTIES
            OUTPUT_NAME vu1_programs PREFIX ""
            CXX_VISIBILITY_PRESET hidden
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
            LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
    endif()
endif()

# Offline GS rasterizer benchmark (see tools/gs_replay.cpp); not built by default.
if(NOT PS2X_IS_ANDROID AND NOT PS2X_IS_VITA)
    add_executable(gs_replay EXCLUDE_FROM_ALL tools/gs_replay.cpp)
    target_link_libraries(gs_replay PRIVATE ps2_runtime)
endif()
