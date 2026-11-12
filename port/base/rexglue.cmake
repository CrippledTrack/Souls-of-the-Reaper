# Linux and Windows use the pinned 0.10 SDK and the shared codegen directory.
find_package(rexglue 0.10.1 REQUIRED CONFIG)
set(_generated "${CMAKE_CURRENT_SOURCE_DIR}/generated/base")
if(NOT EXISTS "${_generated}/sources.cmake")
    message(FATAL_ERROR "Run scripts/build_linux.py or scripts/build_windows.py to generate the base-disc sources first")
endif()
include("${_generated}/sources.cmake")

if(APPLE)
    # The generated registration table is one ~136k-line function. -O2 takes
    # over 30 minutes, and arm64 -O0 GlobalISel is quadratic on it; it only runs
    # once at startup, so use FastISel.
    set_source_files_properties("${_generated}/diablo3_register.cpp" PROPERTIES COMPILE_OPTIONS "-O0;-fno-global-isel"
        SKIP_PRECOMPILE_HEADERS ON)
endif()

function(rexglue_setup_target target)
    add_library(${target}_recomp OBJECT ${GENERATED_SOURCES})
    target_include_directories(${target}_recomp PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/src" "${_generated}")
    target_link_libraries(${target}_recomp PRIVATE rex::runtime)
    rexglue_apply_target_settings(${target}_recomp)
    target_precompile_headers(${target}_recomp PRIVATE "${_generated}/diablo3_pch.h")
    if(WIN32)
        target_compile_options(${target}_recomp PRIVATE -fasync-exceptions)
    else()
        target_compile_options(${target}_recomp PRIVATE -g0)
    endif()
    target_include_directories(${target} PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/src" "${_generated}")
    target_link_libraries(${target} PRIVATE ${target}_recomp rex::runtime ${CMAKE_DL_LIBS})
    rexglue_configure_target(${target} GPU_PLUGINS xenos)
    # The installed SDK stages plugins but does not stage the runtime DSO.
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:rex::runtime> $<TARGET_FILE_DIR:${target}>)
endfunction()
