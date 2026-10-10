cmake_minimum_required(VERSION 3.14)
project(Taipan VERSION 0.2.0 LANGUAGES CXX)

# Taipan is plain C++17 with no dependencies.
#
#   cmake -S . -B build && cmake --build build
#
# Targets:
#   taipan_lib   static library to link into your engine (include/taipan.h)
#   taipan       standalone runner:  taipan script.tp
#   taipan-lsp   language server for editors (VS Code extension in editors/vscode)
#   embed_demo   example of embedding in a game engine
#   api_test     embedding API self-test  (ctest)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(TAIPAN_BUILD_EXAMPLES "Build the embedding example" ON)
option(TAIPAN_BUILD_TESTS "Build the API self-test" ON)
option(TAIPAN_GC_DEBUG "Poison freed objects to catch GC bugs (slow, leaks)" OFF)

file(GLOB TAIPAN_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp)

add_library(taipan_lib STATIC ${TAIPAN_SOURCES})
target_include_directories(taipan_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
if (TAIPAN_GC_DEBUG)
    target_compile_definitions(taipan_lib PRIVATE TAIPAN_GC_DEBUG)
endif()
if (MSVC)
    target_compile_options(taipan_lib PRIVATE /W3 /utf-8 /EHsc)
else()
    target_compile_options(taipan_lib PRIVATE -Wall -Wextra)
endif()

add_executable(taipan tools/taipan_main.cpp)
target_link_libraries(taipan PRIVATE taipan_lib)

add_executable(taipan-lsp tools/taipan_lsp.cpp)
target_include_directories(taipan-lsp PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(taipan-lsp PRIVATE taipan_lib)

if (TAIPAN_BUILD_EXAMPLES)
    add_executable(embed_demo examples/embed_demo.cpp)
    target_link_libraries(embed_demo PRIVATE taipan_lib)
endif()

if (TAIPAN_BUILD_TESTS)
    enable_testing()
    add_executable(api_test tests/api_test.cpp)
    target_link_libraries(api_test PRIVATE taipan_lib)
    add_test(NAME api_test COMMAND api_test WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    # host binding speed (compare with tests/perf/host_bench_lua.c built against Lua)
    add_executable(host_bench tests/perf/host_bench.cpp)
    target_link_libraries(host_bench PRIVATE taipan_lib)
endif()
