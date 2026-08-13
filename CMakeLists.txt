cmake_minimum_required(VERSION 3.24)

project(stuntmaster_pc VERSION 0.0.4 LANGUAGES C CXX)

option(STUNTMASTER_BUILD_TESTS "Build deterministic core tests" ON)
option(STUNTMASTER_ENABLE_PSYCROSS "Build the PsyCross presentation backend" OFF)

if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/external/wuffs/wuffs-root-directory.txt")
    message(FATAL_ERROR
        "Wuffs submodule is missing; run git submodule update --init external/wuffs")
endif()

if(MSVC)
    # Release archives are portable and do not require the Visual C++
    # Redistributable to be installed on the destination machine.
    set(CMAKE_MSVC_RUNTIME_LIBRARY
        "MultiThreaded$<$<CONFIG:Debug>:Debug>")

    # Remove compiler and linker timestamps from packaged Release binaries.
    # Among other things, cl.exe's /Brepro handling makes the legacy PsyCross
    # __DATE__/__TIME__ build banner stable without patching the submodule.
    add_compile_options(
        "$<$<AND:$<CONFIG:Release>,$<COMPILE_LANGUAGE:C,CXX>>:/Brepro>")
    add_link_options("$<$<CONFIG:Release>:/Brepro>")

    # CMAKE_VS_MSBUILD_COMMAND is not a cache entry. Preserve the exact
    # generator-selected executable for post-build provenance collection.
    if(CMAKE_VS_MSBUILD_COMMAND)
        file(WRITE
            "${CMAKE_BINARY_DIR}/CMakeFiles/StuntmasterMSBuildCommand.txt"
            "${CMAKE_VS_MSBUILD_COMMAND}\n")
    endif()
endif()

# Custom codecs are authored in Wuffs and checked in as generated C. Keeping
# generation out of the ordinary build means consumers need only a C compiler;
# tools/generate_wuffs_codecs.ps1 is the explicit regeneration path.
add_library(stuntmaster_wuffs_codecs STATIC
    src/stuntmaster/media/wuffs/generated/stuntmaster_psx.c
    src/stuntmaster/media/wuffs/generated/stuntmaster_str.c
    src/stuntmaster/media/wuffs/generated/stuntmaster_xa.c
    src/stuntmaster/media/wuffs/generated/stuntmaster_mdec.c
)
target_compile_definitions(stuntmaster_wuffs_codecs
    PUBLIC WUFFS_CONFIG__AVOID_CPU_ARCH
    PRIVATE WUFFS_IMPLEMENTATION WUFFS_CONFIG__MODULES
)
set_source_files_properties(
    src/stuntmaster/media/wuffs/generated/stuntmaster_psx.c
    PROPERTIES COMPILE_DEFINITIONS
        WUFFS_CONFIG__MODULE__STUNTMASTER_PSX
)
# Only the small core unit owns the shared Wuffs base implementation. Codec
# units compile independently against that runtime to avoid duplicate symbols.
set_source_files_properties(
    src/stuntmaster/media/wuffs/generated/stuntmaster_str.c
    src/stuntmaster/media/wuffs/generated/stuntmaster_xa.c
    src/stuntmaster/media/wuffs/generated/stuntmaster_mdec.c
    PROPERTIES COMPILE_DEFINITIONS
        "WUFFS_CONFIG__MODULE__STUNTMASTER_PSX;STUNTMASTER_WUFFS__NO_BASE_IMPLEMENTATION"
)
set_target_properties(stuntmaster_wuffs_codecs PROPERTIES
    FOLDER "Libraries"
    INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO ON
    INTERPROCEDURAL_OPTIMIZATION_RELEASE ON
)

add_library(stuntmaster_media STATIC
    src/stuntmaster/media/str_decoder.cpp
)
target_include_directories(stuntmaster_media PUBLIC src)
target_link_libraries(stuntmaster_media PUBLIC stuntmaster_wuffs_codecs)
target_compile_features(stuntmaster_media PUBLIC cxx_std_20)
set_target_properties(stuntmaster_media PROPERTIES
    FOLDER "Libraries"
    INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO ON
    INTERPROCEDURAL_OPTIMIZATION_RELEASE ON
)
if(MSVC)
    target_compile_options(stuntmaster_media PRIVATE /W4 /permissive- /EHsc)
else()
    target_compile_options(stuntmaster_media PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_library(stuntmaster_core
    src/stuntmaster/core/sha256.cpp
    src/stuntmaster/disc/cue_sheet.cpp
    src/stuntmaster/disc/iso9660.cpp
    src/stuntmaster/game/retail_hle.cpp
    src/stuntmaster/game/retail_patch.cpp
    src/stuntmaster/game/retime_hooks.cpp
    src/stuntmaster/game/supported_game.cpp
    src/stuntmaster/platform/executable_memory.cpp
    src/stuntmaster/presentation/debug_overlay.cpp
    src/stuntmaster/presentation/license_overlay.cpp
    src/stuntmaster/presentation/widescreen_overlay.cpp
    src/stuntmaster/psx/bios_hle.cpp
    src/stuntmaster/psx/executable.cpp
    src/stuntmaster/psx/gpu_command_decoder.cpp
    src/stuntmaster/psx/gpu_polygon_limits.cpp
    src/stuntmaster/psx/gpu_command_bridge.cpp
    src/stuntmaster/psx/gte_runtime.cpp
    src/stuntmaster/psx/memory_card.cpp
    src/stuntmaster/psx/r3000_runtime.cpp
    src/stuntmaster/psx/spu.cpp
)
target_include_directories(stuntmaster_core PUBLIC src)
target_compile_features(stuntmaster_core PUBLIC cxx_std_20)
set_target_properties(stuntmaster_core PROPERTIES
    CXX_EXTENSIONS OFF
    FOLDER "Libraries"
    INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO ON
    INTERPROCEDURAL_OPTIMIZATION_RELEASE ON
)

if(MSVC)
    target_compile_options(stuntmaster_core PRIVATE /W4 /permissive- /EHsc)
else()
    target_compile_options(stuntmaster_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(stuntmaster
    src/stuntmaster/app/main.cpp
    src/stuntmaster/app/diagnostics.cpp
    src/stuntmaster/app/frame_io.cpp
    src/stuntmaster/app/game_setup.cpp
    src/stuntmaster/app/launcher_settings.cpp
    src/stuntmaster/app/licenses.cpp
    src/stuntmaster/app/logging.cpp
    src/stuntmaster/app/options.cpp
    src/stuntmaster/app/quick_save.cpp
    src/stuntmaster/app/user_paths.cpp
)
target_link_libraries(stuntmaster PRIVATE stuntmaster_core)
if(WIN32)
    # SHGetKnownFolderPath (shell32) + CoTaskMemFree (ole32) resolve the
    # per-user <Documents>\Stuntmaster data directory without touching the
    # registry.
    target_link_libraries(stuntmaster PRIVATE shell32 ole32)
    # Ship as a GUI-subsystem app so a double-click shows no console window,
    # but keep the standard int main(int, char**): /ENTRY:mainCRTStartup uses it
    # under /SUBSYSTEM:WINDOWS, and a redirected stdout (probes/CI) still flows.
    set_target_properties(stuntmaster PROPERTIES WIN32_EXECUTABLE ON)
    if(MSVC)
        target_link_options(stuntmaster PRIVATE /ENTRY:mainCRTStartup)
    endif()
endif()
target_compile_features(stuntmaster PRIVATE cxx_std_20)
set_target_properties(stuntmaster PROPERTIES
    FOLDER "Applications"
    INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO ON
    INTERPROCEDURAL_OPTIMIZATION_RELEASE ON
)
add_custom_command(TARGET stuntmaster POST_BUILD
    COMMAND ${CMAKE_COMMAND}
        -DSTUNT_INPUT_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/input.example.ini
        -DSTUNT_INPUT_DESTINATION=$<TARGET_FILE_DIR:stuntmaster>/input.ini
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tools/copy_default_input.cmake
    VERBATIM
)

if(WIN32)
    # The game window icon/version resource. The standalone launcher was retired
    # in favour of the executable's own first-launch flow (see docs/dev).
    enable_language(RC)
    target_sources(stuntmaster PRIVATE res/stuntmaster.rc)
    target_include_directories(stuntmaster PRIVATE
        $<$<COMPILE_LANGUAGE:RC>:${CMAKE_CURRENT_SOURCE_DIR}/res>)
    # licenses.cpp includes res/resource.h to resolve the RCDATA ids.
    target_include_directories(stuntmaster PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/res)
endif()

if(STUNTMASTER_ENABLE_PSYCROSS)
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/external/PsyCross/CMakeLists.txt")
        message(FATAL_ERROR "PsyCross submodule is missing; run git submodule update --init")
    endif()
    # Prefer vcpkg's config targets. In particular, static OpenAL needs its
    # configuration-specific library plus transitive fmt and avrt links;
    # CMake's legacy FindOpenAL module exposes only one raw .lib path.
    set(CMAKE_FIND_PACKAGE_PREFER_CONFIG ON)
    find_package(OpenAL CONFIG REQUIRED)
    add_subdirectory(external/PsyCross EXCLUDE_FROM_ALL)

    if(TARGET OpenAL::OpenAL)
        get_target_property(STUNTMASTER_PSYCROSS_PRIVATE_LINKS
            psycross_static LINK_LIBRARIES)
        list(REMOVE_ITEM STUNTMASTER_PSYCROSS_PRIVATE_LINKS OpenAL)
        set_target_properties(psycross_static PROPERTIES
            LINK_LIBRARIES "${STUNTMASTER_PSYCROSS_PRIVATE_LINKS}")
        get_target_property(STUNTMASTER_PSYCROSS_LINKS
            psycross_static INTERFACE_LINK_LIBRARIES)
        list(REMOVE_ITEM STUNTMASTER_PSYCROSS_LINKS
            OpenAL "$<LINK_ONLY:OpenAL>")
        set_target_properties(psycross_static PROPERTIES
            INTERFACE_LINK_LIBRARIES "${STUNTMASTER_PSYCROSS_LINKS}")
        target_link_libraries(psycross_static OpenAL::OpenAL)
    elseif(OPENAL_LIBRARY)
        # Compatibility with non-vcpkg OpenAL packages that expose only the
        # legacy variables.
        get_target_property(STUNTMASTER_PSYCROSS_PRIVATE_LINKS
            psycross_static LINK_LIBRARIES)
        list(REMOVE_ITEM STUNTMASTER_PSYCROSS_PRIVATE_LINKS OpenAL)
        set_target_properties(psycross_static PROPERTIES
            LINK_LIBRARIES "${STUNTMASTER_PSYCROSS_PRIVATE_LINKS}")
        target_link_libraries(psycross_static "${OPENAL_LIBRARY}")
    endif()
    target_sources(stuntmaster PRIVATE
        src/stuntmaster/presentation/audio_output.cpp
        src/stuntmaster/presentation/movie_player.cpp
        src/stuntmaster/presentation/psycross_presenter.cpp
    )
    target_link_libraries(stuntmaster PRIVATE
        psycross_static
        stuntmaster_media)
    target_compile_definitions(stuntmaster PRIVATE
        STUNTMASTER_HAS_PSYCROSS=1
    )
    message(STATUS "PsyCross presentation adapter enabled")
endif()

if(STUNTMASTER_BUILD_TESTS)
    enable_testing()
    add_executable(stuntmaster_core_tests tests/core_tests.cpp)
    target_link_libraries(stuntmaster_core_tests PRIVATE stuntmaster_core)
    target_compile_features(stuntmaster_core_tests PRIVATE cxx_std_20)
    if(MSVC)
        target_compile_options(stuntmaster_core_tests PRIVATE /UNDEBUG)
    else()
        target_compile_options(stuntmaster_core_tests PRIVATE -UNDEBUG)
    endif()
    add_test(NAME stuntmaster_core_tests COMMAND stuntmaster_core_tests)

    add_executable(stuntmaster_quick_save_tests
        tests/quick_save_tests.cpp
        src/stuntmaster/app/launcher_settings.cpp
        src/stuntmaster/app/logging.cpp
        src/stuntmaster/app/options.cpp
        src/stuntmaster/app/quick_save.cpp
        src/stuntmaster/app/user_paths.cpp
    )
    target_link_libraries(stuntmaster_quick_save_tests PRIVATE stuntmaster_core)
    if(WIN32)
        target_link_libraries(stuntmaster_quick_save_tests PRIVATE shell32 ole32)
    endif()
    target_include_directories(
        stuntmaster_quick_save_tests PRIVATE src/stuntmaster/app)
    target_compile_features(stuntmaster_quick_save_tests PRIVATE cxx_std_20)
    if(MSVC)
        target_compile_options(stuntmaster_quick_save_tests PRIVATE /UNDEBUG)
    else()
        target_compile_options(stuntmaster_quick_save_tests PRIVATE -UNDEBUG)
    endif()
    add_test(NAME stuntmaster_quick_save_tests COMMAND stuntmaster_quick_save_tests)

    add_executable(stuntmaster_wuffs_smoke_tests
        tests/wuffs_smoke_tests.cpp
    )
    target_include_directories(stuntmaster_wuffs_smoke_tests PRIVATE
        src/stuntmaster/media/wuffs/generated
    )
    target_link_libraries(
        stuntmaster_wuffs_smoke_tests PRIVATE stuntmaster_wuffs_codecs)
    target_compile_features(stuntmaster_wuffs_smoke_tests PRIVATE cxx_std_20)
    add_test(
        NAME stuntmaster_wuffs_smoke_tests
        COMMAND stuntmaster_wuffs_smoke_tests)

    # Disc-backed validation tool. It is not registered as a CTest because the
    # fingerprinted retail BIN/CUE is intentionally never part of the source
    # tree or release artifacts.
    add_executable(stuntmaster_str_codec_probe
        tests/str_codec_probe.cpp)
    target_link_libraries(stuntmaster_str_codec_probe PRIVATE
        stuntmaster_core
        stuntmaster_media)
    target_compile_features(stuntmaster_str_codec_probe PRIVATE cxx_std_20)

    foreach(STUNTMASTER_WUFFS_CODEC_TEST IN ITEMS str xa mdec)
        set(STUNTMASTER_WUFFS_CODEC_TEST_SOURCE
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/wuffs_${STUNTMASTER_WUFFS_CODEC_TEST}_tests.cpp")
        if(EXISTS "${STUNTMASTER_WUFFS_CODEC_TEST_SOURCE}")
            set(STUNTMASTER_WUFFS_CODEC_TEST_TARGET
                "stuntmaster_wuffs_${STUNTMASTER_WUFFS_CODEC_TEST}_tests")
            add_executable(${STUNTMASTER_WUFFS_CODEC_TEST_TARGET}
                "${STUNTMASTER_WUFFS_CODEC_TEST_SOURCE}")
            target_include_directories(
                ${STUNTMASTER_WUFFS_CODEC_TEST_TARGET} PRIVATE
                src/stuntmaster/media/wuffs/generated)
            target_link_libraries(
                ${STUNTMASTER_WUFFS_CODEC_TEST_TARGET} PRIVATE
                stuntmaster_wuffs_codecs)
            target_compile_features(
                ${STUNTMASTER_WUFFS_CODEC_TEST_TARGET} PRIVATE cxx_std_20)
            if(MSVC)
                target_compile_options(
                    ${STUNTMASTER_WUFFS_CODEC_TEST_TARGET} PRIVATE /UNDEBUG)
            else()
                target_compile_options(
                    ${STUNTMASTER_WUFFS_CODEC_TEST_TARGET} PRIVATE -UNDEBUG)
            endif()
            add_test(
                NAME ${STUNTMASTER_WUFFS_CODEC_TEST_TARGET}
                COMMAND ${STUNTMASTER_WUFFS_CODEC_TEST_TARGET})
        endif()
    endforeach()
endif()

if(WIN32)
    # The release artifact is the single, self-configuring executable. It seeds
    # its own input.ini and config into <Documents>/Stuntmaster on first run and
    # embeds every third-party license text (viewable in-app and via
    # --licenses), so nothing needs to ship beside it -- no launcher, docs, or
    # licenses/ directory. tools/package_windows.ps1 copies the built
    # stuntmaster.exe to dist/ under the versioned release name and builds the
    # corresponding-source bundle; there is deliberately no CPack archive.
    install(TARGETS stuntmaster
        RUNTIME DESTINATION .)
endif()
