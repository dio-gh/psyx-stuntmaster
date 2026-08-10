cmake_minimum_required(VERSION 3.24)

project(stuntmaster_pc VERSION 0.0.1 LANGUAGES C CXX)

option(STUNTMASTER_BUILD_TESTS "Build deterministic core tests" ON)
option(STUNTMASTER_ENABLE_PSYCROSS "Build the PsyCross presentation backend" OFF)

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

add_library(stuntmaster_core
    src/stuntmaster/core/sha256.cpp
    src/stuntmaster/disc/cue_sheet.cpp
    src/stuntmaster/disc/iso9660.cpp
    src/stuntmaster/game/retail_hle.cpp
    src/stuntmaster/game/retail_patch.cpp
    src/stuntmaster/game/retime_hooks.cpp
    src/stuntmaster/game/supported_game.cpp
    src/stuntmaster/presentation/debug_overlay.cpp
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
    src/stuntmaster/app/launcher_settings.cpp
    src/stuntmaster/app/options.cpp
    src/stuntmaster/app/quick_save.cpp
)
target_link_libraries(stuntmaster PRIVATE stuntmaster_core)
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
    enable_language(RC)
    target_sources(stuntmaster PRIVATE res/stuntmaster.rc)
    target_include_directories(stuntmaster PRIVATE
        $<$<COMPILE_LANGUAGE:RC>:${CMAKE_CURRENT_SOURCE_DIR}/res>)

    add_executable(stuntmaster_launcher WIN32
        src/stuntmaster/launcher/main.cpp
        src/stuntmaster/app/launcher_settings.cpp
        res/stuntmaster.rc
    )
    target_include_directories(stuntmaster_launcher PRIVATE
        src/stuntmaster/app
        $<$<COMPILE_LANGUAGE:RC>:${CMAKE_CURRENT_SOURCE_DIR}/res>)
    target_compile_features(stuntmaster_launcher PRIVATE cxx_std_20)
    target_link_libraries(stuntmaster_launcher PRIVATE shell32 ole32)
    if(MSVC)
        target_compile_options(
            stuntmaster_launcher PRIVATE /W4 /permissive- /EHsc)
        set_property(TARGET stuntmaster_launcher PROPERTY
            MSVC_RUNTIME_LIBRARY
            "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
    set_target_properties(stuntmaster_launcher PROPERTIES
        OUTPUT_NAME "stuntmaster-launcher"
        FOLDER "Applications"
        INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO ON
        INTERPROCEDURAL_OPTIMIZATION_RELEASE ON
    )
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

    set(STUNTMASTER_FFMPEG_ROOT "" CACHE PATH
        "Root of the source-built static FFmpeg install containing include and lib")
    if(NOT STUNTMASTER_FFMPEG_ROOT)
        message(FATAL_ERROR
            "STUNTMASTER_FFMPEG_ROOT is required for the PsyCross build. "
            "Use tools/build_windows.cmd to build the pinned source release.")
    endif()
    find_path(STUNTMASTER_FFMPEG_INCLUDE_DIR libavcodec/avcodec.h
        PATHS "${STUNTMASTER_FFMPEG_ROOT}/include"
        NO_DEFAULT_PATH REQUIRED)

    set(STUNTMASTER_FFMPEG_TARGETS)
    foreach(STUNTMASTER_FFMPEG_LIBRARY IN ITEMS
            avcodec avformat avutil swresample swscale)
        string(TOUPPER "${STUNTMASTER_FFMPEG_LIBRARY}"
            STUNTMASTER_FFMPEG_LIBRARY_UPPER)
        find_library(
            STUNTMASTER_${STUNTMASTER_FFMPEG_LIBRARY_UPPER}_LIBRARY
            NAMES "${STUNTMASTER_FFMPEG_LIBRARY}"
            PATHS "${STUNTMASTER_FFMPEG_ROOT}/lib"
            NO_DEFAULT_PATH REQUIRED)
        add_library(
            stuntmaster_ffmpeg_${STUNTMASTER_FFMPEG_LIBRARY} STATIC IMPORTED)
        set_target_properties(
            stuntmaster_ffmpeg_${STUNTMASTER_FFMPEG_LIBRARY} PROPERTIES
            IMPORTED_LOCATION
                "${STUNTMASTER_${STUNTMASTER_FFMPEG_LIBRARY_UPPER}_LIBRARY}")
        list(APPEND STUNTMASTER_FFMPEG_TARGETS
            stuntmaster_ffmpeg_${STUNTMASTER_FFMPEG_LIBRARY})
    endforeach()
    target_link_libraries(stuntmaster_ffmpeg_avformat INTERFACE
        stuntmaster_ffmpeg_avcodec stuntmaster_ffmpeg_avutil)
    target_link_libraries(stuntmaster_ffmpeg_avcodec INTERFACE
        stuntmaster_ffmpeg_avutil ole32 user32)
    target_link_libraries(stuntmaster_ffmpeg_swresample INTERFACE
        stuntmaster_ffmpeg_avutil)
    target_link_libraries(stuntmaster_ffmpeg_swscale INTERFACE
        stuntmaster_ffmpeg_avutil)
    target_link_libraries(stuntmaster_ffmpeg_avutil INTERFACE
        user32 bcrypt)
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
        src/stuntmaster/media/str_decoder.cpp
        src/stuntmaster/presentation/audio_output.cpp
        src/stuntmaster/presentation/movie_player.cpp
        src/stuntmaster/presentation/psycross_presenter.cpp
    )
    target_include_directories(
        stuntmaster SYSTEM PRIVATE ${STUNTMASTER_FFMPEG_INCLUDE_DIR})
    target_link_libraries(stuntmaster PRIVATE psycross_static)
    target_link_libraries(stuntmaster PRIVATE ${STUNTMASTER_FFMPEG_TARGETS})
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
        src/stuntmaster/app/options.cpp
        src/stuntmaster/app/quick_save.cpp
    )
    target_link_libraries(stuntmaster_quick_save_tests PRIVATE stuntmaster_core)
    target_include_directories(
        stuntmaster_quick_save_tests PRIVATE src/stuntmaster/app)
    target_compile_features(stuntmaster_quick_save_tests PRIVATE cxx_std_20)
    if(MSVC)
        target_compile_options(stuntmaster_quick_save_tests PRIVATE /UNDEBUG)
    else()
        target_compile_options(stuntmaster_quick_save_tests PRIVATE -UNDEBUG)
    endif()
    add_test(NAME stuntmaster_quick_save_tests COMMAND stuntmaster_quick_save_tests)
endif()

if(WIN32)
    install(TARGETS stuntmaster
        RUNTIME DESTINATION .)
    if(TARGET stuntmaster_launcher)
        install(TARGETS stuntmaster_launcher
            RUNTIME DESTINATION .)
    endif()
    install(FILES input.example.ini
        DESTINATION .
        RENAME input.ini)
    install(FILES README.md LICENSE THIRD_PARTY.md
        DESTINATION .)
    install(DIRECTORY docs/
        DESTINATION docs
        FILES_MATCHING PATTERN "*.md")
    install(FILES docs/DISTRIBUTION.txt
        DESTINATION .
        RENAME README-FIRST.txt)

    if(STUNTMASTER_ENABLE_PSYCROSS)
        install(FILES
            "${STUNTMASTER_FFMPEG_ROOT}/share/licenses/ffmpeg/COPYING.LGPLv2.1"
            DESTINATION licenses
            RENAME FFmpeg-LGPL-2.1-or-later.txt)
        install(FILES
            "${STUNTMASTER_FFMPEG_ROOT}/share/licenses/ffmpeg/COPYING.LGPLv3"
            DESTINATION licenses
            RENAME FFmpeg-LGPL-3.0.txt)
        install(FILES
            "${STUNTMASTER_FFMPEG_ROOT}/share/licenses/ffmpeg/LICENSE.md"
            DESTINATION licenses
            RENAME FFmpeg-LICENSE.md)
        if(DEFINED VCPKG_INSTALLED_DIR AND DEFINED VCPKG_TARGET_TRIPLET)
            set(STUNTMASTER_VCPKG_SHARE
                "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/share")
            if(EXISTS "${STUNTMASTER_VCPKG_SHARE}/sdl2/copyright")
                install(FILES "${STUNTMASTER_VCPKG_SHARE}/sdl2/copyright"
                    DESTINATION licenses RENAME SDL2.txt)
            endif()
            if(EXISTS "${STUNTMASTER_VCPKG_SHARE}/openal-soft/copyright")
                install(FILES
                    "${STUNTMASTER_VCPKG_SHARE}/openal-soft/copyright"
                    DESTINATION licenses RENAME OpenAL-Soft.txt)
            endif()
            if(EXISTS "${STUNTMASTER_VCPKG_SHARE}/fmt/copyright")
                install(FILES "${STUNTMASTER_VCPKG_SHARE}/fmt/copyright"
                    DESTINATION licenses RENAME fmt.txt)
            endif()
        endif()
    endif()

    set(CPACK_GENERATOR ZIP)
    set(CPACK_PACKAGE_NAME "stuntmaster-pc")
    set(CPACK_PACKAGE_VENDOR "Stuntmaster PC contributors")
    set(CPACK_PACKAGE_FILE_NAME
        "stuntmaster-pc-${PROJECT_VERSION}-windows-x64")
    set(CPACK_PACKAGE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/dist")
    set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY ON)
    include(CPack)
endif()
