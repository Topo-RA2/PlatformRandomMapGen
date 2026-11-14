cmake_minimum_required(VERSION 3.24)

project(PlatformRandomMapGen VERSION 1.1.0 LANGUAGES CXX)

include(FetchContent)

set(PLATFORM_DEVKIT_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/../PlatformDevKit" CACHE PATH
    "Path to the shared PlatformDevKit checkout")
if(NOT EXISTS "${PLATFORM_DEVKIT_SOURCE_DIR}/cmake/PlatformOpenSSL.cmake")
    message(FATAL_ERROR "PlatformDevKit OpenSSL policy was not found at ${PLATFORM_DEVKIT_SOURCE_DIR}")
endif()
include("${PLATFORM_DEVKIT_SOURCE_DIR}/cmake/PlatformOpenSSL.cmake")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 与工作区其余仓一致，显式固定 MSVC 动态运行库，不依赖 CMake 的当前默认值。
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
endif()

set(RMG_GENERATOR_VERSION "1.1.0-native" CACHE STRING "Version string reported by rmg --version")
if(NOT RMG_GENERATOR_VERSION MATCHES "^[A-Za-z0-9][A-Za-z0-9._+%-]*$" OR
   RMG_GENERATOR_VERSION STREQUAL "")
    message(FATAL_ERROR "RMG_GENERATOR_VERSION must be a single 1..43 character release token")
endif()
string(LENGTH "RandomMapGen ${RMG_GENERATOR_VERSION}" RMG_VERSION_LINE_LENGTH)
if(RMG_VERSION_LINE_LENGTH GREATER 56)
    message(FATAL_ERROR "RMG_GENERATOR_VERSION makes the --version line exceed 56 characters")
endif()

option(PLATFORMRANDOMMAPGEN_BUILD_TESTS "Build RandomMapGen self-tests" OFF)

FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    # v3.12.0, shared with PlatformSDK.
    GIT_TAG 55f93686c01528224f448c19128836e7df245f72
    GIT_SHALLOW FALSE
)
FetchContent_MakeAvailable(nlohmann_json)

platform_find_openssl(Crypto)

add_library(rmg_native STATIC
    src/cliff_pieces.cpp
    src/ini_config.cpp
    src/map_evaluation.cpp
    src/map_types.cpp
    src/map_writer.cpp
    src/native_api.cpp
    src/native_game_options.cpp
    src/native_generator.cpp
    src/native_resource.cpp
    src/native_theater_catalog.cpp
    src/native_trace.cpp
    src/randomizer.cpp
    src/shore_pieces.cpp
    src/tile_catalog.cpp
    src/yrini_validator.cpp
    third_party/pack_codec/pack_encoding.cpp
    third_party/ra2formats/src/ini.cpp
    third_party/ra2formats/src/mix.cpp
    third_party/ra2formats/src/palette.cpp
    third_party/ra2formats/src/tmp.cpp
)
add_library(PlatformRandomMapGen::Native ALIAS rmg_native)

target_include_directories(rmg_native
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/third_party>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/third_party/ra2formats/include>
)
target_link_libraries(rmg_native
    PRIVATE nlohmann_json::nlohmann_json
    PUBLIC OpenSSL::Crypto
)
target_compile_features(rmg_native PUBLIC cxx_std_20)
set_target_properties(rmg_native PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(MSVC)
    target_compile_options(rmg_native PRIVATE /W4 /utf-8)
else()
    target_compile_options(rmg_native PRIVATE -Wall -Wextra)
endif()

if(WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(CMAKE_ASM_NASM_OBJECT_FORMAT win64)
    enable_language(ASM_NASM)
    target_sources(rmg_native PRIVATE src/native_x87_math.asm)
    target_compile_definitions(rmg_native PRIVATE RMG_HAS_NATIVE_X87_HELPER)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
    enable_language(ASM)
    target_sources(rmg_native PRIVATE src/native_x87_math.S)
    target_compile_definitions(rmg_native PRIVATE RMG_HAS_NATIVE_X87_HELPER)
endif()

add_executable(rmg src/main.cpp)
add_executable(PlatformRandomMapGen::CLI ALIAS rmg)
target_link_libraries(rmg PRIVATE PlatformRandomMapGen::Native)
target_compile_definitions(rmg PRIVATE RMG_GENERATOR_VERSION="${RMG_GENERATOR_VERSION}")
if(MSVC)
    target_compile_options(rmg PRIVATE /utf-8)
endif()
# OpenSSL 在工作区按共享库提供；没有暂存时 rmg 在干净机器上找不到 libcrypto。
if(WIN32)
    add_custom_command(TARGET rmg POST_BUILD
        COMMAND "${CMAKE_COMMAND}"
            "-DRUNTIME_DLLS=$<TARGET_RUNTIME_DLLS:rmg>"
            "-DOUTPUT_DIRECTORY=$<TARGET_FILE_DIR:rmg>"
            -P "${PLATFORM_DEVKIT_SOURCE_DIR}/cmake/StageRuntimeDlls.cmake"
        VERBATIM)
endif()

if(PLATFORMRANDOMMAPGEN_BUILD_TESTS)
    enable_testing()
    add_executable(rmg_tests tests/selftest.cpp)
    target_link_libraries(rmg_tests PRIVATE PlatformRandomMapGen::Native)
    if(MSVC)
        target_compile_options(rmg_tests PRIVATE /utf-8)
    endif()
    add_test(NAME rmg_selftest COMMAND rmg_tests)

    find_program(PLATFORMRANDOMMAPGEN_PWSH_EXECUTABLE NAMES pwsh)
    if(PLATFORMRANDOMMAPGEN_PWSH_EXECUTABLE)
        add_test(NAME rmg_native_cli
            COMMAND "${PLATFORMRANDOMMAPGEN_PWSH_EXECUTABLE}" -NoProfile -File
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_native_cli.ps1"
                -RmgPath "$<TARGET_FILE:rmg>")
        set_tests_properties(rmg_native_cli PROPERTIES
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
            TIMEOUT 180)
    else()
        message(STATUS "PowerShell 7 was not found; the optional rmg_native_cli CTest is unavailable")
    endif()
endif()
