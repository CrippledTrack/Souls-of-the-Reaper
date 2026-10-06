# diablo3 - ReXGlue Recompiled Project

cmake_minimum_required(VERSION 3.25)
project(diablo3 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(SOULS_ENABLE_EXTRA_FEATURES "Enable optional PC game features" OFF)
if(SOULS_ENABLE_EXTRA_FEATURES AND NOT (WIN32 OR CMAKE_SYSTEM_NAME STREQUAL "Linux"))
    message(FATAL_ERROR "Extra features support Windows and Linux base-disc builds")
endif()
if(SOULS_ENABLE_EXTRA_FEATURES)
    set(SOULS_BASE_XEX "${CMAKE_CURRENT_SOURCE_DIR}/../game/Default.xex" CACHE FILEPATH
        "Base-disc executable used to generate the optional game hooks")
    if(NOT EXISTS "${SOULS_BASE_XEX}")
        message(FATAL_ERROR "Extra features require the original base-disc Default.xex; set SOULS_BASE_XEX")
    endif()
    file(SHA256 "${SOULS_BASE_XEX}" _souls_xex_hash)
    if(NOT _souls_xex_hash STREQUAL "cc918a70940517f974d0fd60d4936c8236e8dc21130cf4a8b8ae915451c289ef")
        message(FATAL_ERROR "Extra-feature hook addresses require the unmodified USA Ultimate Evil Edition base disc")
    endif()
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    include(linux/rexglue.cmake)
else()
    include(generated/rexglue.cmake)
endif()

# Sources
set(DIABLO3_SOURCES
    src/main.cpp
    src/kernel_overrides.cpp
    src/window_features.cpp
)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    list(APPEND DIABLO3_SOURCES src/linux_exit.cpp)
endif()

if(WIN32)
    add_executable(diablo3 WIN32 ${DIABLO3_SOURCES})
else()
    add_executable(diablo3 ${DIABLO3_SOURCES})
endif()

rexglue_setup_target(diablo3)

if(SOULS_ENABLE_EXTRA_FEATURES)
    if(WIN32)
        set(_souls_init "${CMAKE_CURRENT_SOURCE_DIR}/generated/default/diablo3_init.cpp")
    else()
        set(_souls_init "${CMAKE_CURRENT_SOURCE_DIR}/generated/linux/diablo3_init.cpp")
    endif()
    if(NOT EXISTS "${_souls_init}")
        message(FATAL_ERROR "Run platform codegen before enabling extra features")
    endif()
    file(READ "${_souls_init}" _souls_mappings)
    foreach(_hook sub_826FD808 sub_826FBFF0 sub_826FC030 sub_826FC798 sub_826FCE80 sub_8282F0F0 sub_823BD130)
        string(FIND "${_souls_mappings}" "${_hook}" _hook_position)
        if(_hook_position EQUAL -1)
            message(FATAL_ERROR "Generated code is missing ${_hook}; regenerate the base-disc code")
        endif()
    endforeach()
    target_sources(diablo3 PRIVATE
        src/features/pc_features.cpp
        src/features/pc_features_startup.cpp
        src/features/pc_features_storage.cpp)
    target_compile_definitions(diablo3 PRIVATE SOULS_ENABLE_EXTRA_FEATURES=1)
    message(STATUS "Optional PC features enabled: render resolution menu, PC autosave wording and Extras version label")
endif()

# When building the SDK from source (REXSDK_DIR -> add_subdirectory), rexglue_configure_target
# adds rex_app.cpp which includes <imgui.h> and <rex/version.h>. In install mode those headers
# live in the SDK's flat include tree; in source mode they are split:
#   - imgui.h       -> exposed by the 'imgui' target (BUILD_INTERFACE)
#   - rex/version.h -> generated in <build>/rexglue-sdk/include, exposed by 'rexcore' PUBLIC
# Pass include dirs only (no object linking -> no duplicate symbols).
foreach(_dep rexcore imgui)
    if(TARGET ${_dep})
        target_include_directories(diablo3 PRIVATE
            $<TARGET_PROPERTY:${_dep},INTERFACE_INCLUDE_DIRECTORIES>)
    endif()
endforeach()
