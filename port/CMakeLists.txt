# diablo3 - ReXGlue Recompiled Project

cmake_minimum_required(VERSION 3.25)
project(diablo3 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(SOULS_ENABLE_EXTRA_FEATURES "Enable optional PC game features" OFF)
option(SOULS_TITLE_UPDATE_2 "Build the verified USA TU2" OFF)

if(SOULS_ENABLE_EXTRA_FEATURES AND NOT (WIN32 OR CMAKE_SYSTEM_NAME STREQUAL "Linux"))
    message(FATAL_ERROR "Extra features support Windows and Linux base-disc/TU2 builds")
endif()
if(SOULS_ENABLE_EXTRA_FEATURES)
    set(SOULS_BASE_XEX "${CMAKE_CURRENT_SOURCE_DIR}/../game/Default.xex" CACHE FILEPATH
        "Game executable used to generate the selected optional hooks")
    if(SOULS_TITLE_UPDATE_2)
        set(_souls_expected_hash "447652ffa8abe4c7b8bed590a3887efc23e1181fd836b7a3192b8a2a37ddf80f")
    else()
        set(_souls_expected_hash "cc918a70940517f974d0fd60d4936c8236e8dc21130cf4a8b8ae915451c289ef")
    endif()
    if(NOT EXISTS "${SOULS_BASE_XEX}")
        message(FATAL_ERROR "Extra features require the selected game Default.xex; set SOULS_BASE_XEX")
    endif()
    file(SHA256 "${SOULS_BASE_XEX}" _souls_xex_hash)
    if(NOT _souls_xex_hash STREQUAL _souls_expected_hash)
        message(FATAL_ERROR "Extra-feature executable does not match the selected base-disc/TU2 hook addresses")
    endif()
endif()

if(SOULS_TITLE_UPDATE_2)
    include(title_updates/rexglue.cmake)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
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
if(SOULS_TITLE_UPDATE_2)
    list(APPEND DIABLO3_SOURCES src/title_update_exit.cpp)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    list(APPEND DIABLO3_SOURCES src/linux_exit.cpp)
endif()

if(WIN32)
    add_executable(diablo3 WIN32 ${DIABLO3_SOURCES})
else()
    add_executable(diablo3 ${DIABLO3_SOURCES})
endif()

rexglue_setup_target(diablo3)
if(SOULS_TITLE_UPDATE_2)
    target_compile_definitions(diablo3 PRIVATE SOULS_TITLE_UPDATE_2=1)
endif()

if(SOULS_ENABLE_EXTRA_FEATURES)
    if(SOULS_TITLE_UPDATE_2)
        set(_souls_init "${CMAKE_CURRENT_SOURCE_DIR}/generated/tu2/diablo3_init.cpp")
    elseif(WIN32)
        set(_souls_init "${CMAKE_CURRENT_SOURCE_DIR}/generated/default/diablo3_init.cpp")
    else()
        set(_souls_init "${CMAKE_CURRENT_SOURCE_DIR}/generated/linux/diablo3_init.cpp")
    endif()
    if(NOT EXISTS "${_souls_init}")
        message(FATAL_ERROR "Run platform codegen before enabling extra features")
    endif()
    file(READ "${_souls_init}" _souls_mappings)
    if(SOULS_TITLE_UPDATE_2)
        set(_souls_hooks sub_82703EF0 sub_827026D8 sub_82702718 sub_82702E80 sub_82703568 sub_82835EE0 sub_823BD130
            sub_82702450 sub_82702518 sub_827071C0 sub_82CE2E40 sub_82CE31D8 sub_82CE1608 sub_82CE2100
            sub_82750310 sub_8246EB88 sub_8250B098 sub_8246EB18)
    else()
        set(_souls_hooks sub_826FD808 sub_826FBFF0 sub_826FC030 sub_826FC798 sub_826FCE80 sub_8282F0F0 sub_823BD130
            sub_826FBD68 sub_826FBE30 sub_82700AD8 sub_82CDAD58 sub_82CDB0F0 sub_82CD9520 sub_82CDA018
            sub_82749C10 sub_8246C520 sub_825086E0 sub_8246C4B0)
    endif()
    foreach(_hook IN LISTS _souls_hooks)
        string(FIND "${_souls_mappings}" "${_hook}" _hook_position)
        if(_hook_position EQUAL -1)
            message(FATAL_ERROR "Generated code is missing ${_hook}; regenerate the selected game code")
        endif()
    endforeach()
    target_sources(diablo3 PRIVATE
        src/features/pc_features.cpp
        src/features/pc_features_startup.cpp
        src/features/pc_features_storage.cpp)
    target_compile_definitions(diablo3 PRIVATE SOULS_ENABLE_EXTRA_FEATURES=1)
    message(STATUS "Optional PC features enabled: render resolution and window mode menus, PC autosave wording and Extras version label")
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
