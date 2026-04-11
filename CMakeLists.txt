cmake_minimum_required(VERSION 3.16)
project(SwarmRT C ASM)

set(CMAKE_C_STANDARD 11)

# Platform detection
if(WIN32)
    add_definitions(-D_WIN32)
    set(PLATFORM_LIBS ws2_32)
elseif(APPLE)
    add_definitions(-D_DARWIN_C_SOURCE)
    set(PLATFORM_LIBS "")
else()
    add_definitions(-D_GNU_SOURCE)
    set(PLATFORM_LIBS ssl crypto)
endif()

# Common flags
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -Wno-unused-function -O2")
if(CMAKE_C_COMPILER_ID MATCHES "Clang")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wno-macro-redefined")
endif()

# Source directory
set(SRC_DIR ${CMAKE_SOURCE_DIR}/src)

# Core runtime objects
set(CORE_SOURCES
    ${SRC_DIR}/swarmrt_native.c
    ${SRC_DIR}/swarmrt_asm.S
    ${SRC_DIR}/swarmrt_otp.c
    ${SRC_DIR}/swarmrt_task.c
    ${SRC_DIR}/swarmrt_ets.c
    ${SRC_DIR}/swarmrt_phase4.c
    ${SRC_DIR}/swarmrt_phase5.c
    ${SRC_DIR}/swarmrt_hotload.c
    ${SRC_DIR}/swarmrt_io.c
    ${SRC_DIR}/swarmrt_gc.c
    ${SRC_DIR}/swarmrt_node.c
    ${SRC_DIR}/swarmrt_lang.c
    ${SRC_DIR}/swarmrt_http.c
)

# swarmrt_native.c needs -fno-stack-protector (processes migrate between
# scheduler threads via context_swap, invalidating the per-thread canary)
set_source_files_properties(${SRC_DIR}/swarmrt_native.c
    PROPERTIES COMPILE_FLAGS "-fno-stack-protector")

# Enable .S assembly files
enable_language(ASM)

# Static library
add_library(swarmrt_lib STATIC ${CORE_SOURCES})
target_include_directories(swarmrt_lib PUBLIC ${SRC_DIR})
target_link_libraries(swarmrt_lib pthread z ${PLATFORM_LIBS})

# SwarmRT Compiler (swc)
add_executable(swc
    ${SRC_DIR}/swc.c
    ${SRC_DIR}/swarmrt_codegen.c
    ${SRC_DIR}/swarmrt_obfusc.c
    ${SRC_DIR}/swarmrt_repl.c
    ${SRC_DIR}/swarmrt_test.c
)
target_link_libraries(swc swarmrt_lib)

# Search module
add_library(swarmrt_search STATIC ${SRC_DIR}/swarmrt_search.c)
target_include_directories(swarmrt_search PUBLIC ${SRC_DIR})
if(NOT WIN32)
    target_compile_options(swarmrt_search PRIVATE -march=native)
endif()

# Filesystem search CLI (sws)
add_executable(sws ${SRC_DIR}/swarmrt_fsindex.c)
target_link_libraries(sws swarmrt_search pthread z m ${PLATFORM_LIBS})
if(NOT WIN32)
    target_compile_options(sws PRIVATE -march=native)
endif()

# Native benchmark
add_executable(swarmrt-native ${SRC_DIR}/benchmark_native.c)
target_link_libraries(swarmrt-native swarmrt_lib)

# Output to bin/
set_target_properties(swc sws swarmrt-native swarmrt_lib swarmrt_search
    PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin
    ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin
)

# Test targets
if(EXISTS ${SRC_DIR}/test_otp.c)
    add_executable(test-otp ${SRC_DIR}/test_otp.c)
    target_link_libraries(test-otp swarmrt_lib)
    set_target_properties(test-otp PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin)
endif()

if(EXISTS ${SRC_DIR}/test_phase2.c)
    add_executable(test-phase2 ${SRC_DIR}/test_phase2.c)
    target_link_libraries(test-phase2 swarmrt_lib)
    set_target_properties(test-phase2 PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin)
endif()

if(EXISTS ${SRC_DIR}/test_search.c)
    add_executable(test-search ${SRC_DIR}/test_search.c)
    target_link_libraries(test-search swarmrt_search pthread z m ${PLATFORM_LIBS})
    if(NOT WIN32)
        target_compile_options(test-search PRIVATE -march=native)
    endif()
    set_target_properties(test-search PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin)
endif()

# MinGW-w64 cross-compilation toolchain
# Usage: cmake -DCMAKE_TOOLCHAIN_FILE=mingw-w64.cmake ..
