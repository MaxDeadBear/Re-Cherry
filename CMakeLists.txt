# Re-Cherry - ReXGlue recompilation of Lollipop Chainsaw.
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(re_cherry LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(CHERRY_X86_64_V3 "Build for x86-64-v3 (AVX2, Haswell and newer)" ON)

include(generated/rexglue.cmake)

if(NOT REXGLUE_ENTRYPOINT_GENERATED_SOURCES)
    message(FATAL_ERROR "No recompiled code. Run codegen first:\n  rexglue codegen re_cherry_manifest.toml")
endif()

foreach(submodule plume XenosRecomp)
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/${submodule}/CMakeLists.txt")
        message(FATAL_ERROR "thirdparty/${submodule} is missing:\n  git submodule update --init --recursive")
    endif()
endforeach()
add_subdirectory(thirdparty/plume plume EXCLUDE_FROM_ALL)

# The runtime only decompresses the shader cache, so only zstd's library is built.
set(ZSTD_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_STATIC ON CACHE BOOL "" FORCE)
set(ZSTD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(thirdparty/XenosRecomp/thirdparty/zstd/build/cmake zstd EXCLUDE_FROM_ALL)

file(GLOB_RECURSE CHERRY_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/src" PREFIX src FILES ${CHERRY_SOURCES})

# XenosRecomp writes res/shader_cache/shader_cache.cpp from shader_dump/ (docs/renderer/shaders.md); until then every shader misses and is dumped.
set(CHERRY_SHADER_CACHE "${CMAKE_CURRENT_SOURCE_DIR}/res/shader_cache/shader_cache.cpp")
if(NOT EXISTS "${CHERRY_SHADER_CACHE}")
    set(CHERRY_SHADER_CACHE "${CMAKE_CURRENT_SOURCE_DIR}/res/shader_cache/shader_cache_empty.cpp")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/res/shader_cache")
list(APPEND CHERRY_SOURCES
    "${CHERRY_SHADER_CACHE}"
    # The cache stores SPIR-V smol-v encoded.
    thirdparty/XenosRecomp/thirdparty/smol-v/source/smolv.cpp)

# Optional Xenia side-by-side comparison build; removing compare/xenia/ and these two lines removes it.
include(compare/xenia/compare.cmake)

if(WIN32)
    add_executable(re_cherry WIN32 ${CHERRY_SOURCES})
else()
    add_executable(re_cherry ${CHERRY_SOURCES})
endif()

# XenosRecomp's own xxHash, so the runtime hashes containers exactly as the translator did.
target_include_directories(re_cherry PRIVATE
    src
    src/gpu/shaders
    thirdparty/XenosRecomp/thirdparty/xxHash
    thirdparty/XenosRecomp/thirdparty/smol-v/source
    thirdparty/XenosRecomp/thirdparty/zstd/lib)
target_link_libraries(re_cherry PRIVATE libzstd_static)

# The native renderer draws through plume; there is no GPU plugin. plume is static, so its D3D12 imports are satisfied here, and Vulkan loads through volk.
target_link_libraries(re_cherry PRIVATE plume)
if(WIN32)
    target_link_libraries(re_cherry PRIVATE d3d12 dxgi dbghelp)
elseif(UNIX AND NOT APPLE)
    target_link_libraries(re_cherry PRIVATE X11 ${CMAKE_DL_LIBS})
endif()

rexglue_setup_target(re_cherry)
cherry_xenia_compare_target(re_cherry)

# Non-Release SDK builds bump a shared atomic on every indirect guest call; the recompiled code opts out.
if(TARGET re_cherry_recomp)
    target_compile_options(re_cherry_recomp PRIVATE -UREXGLUE_ENABLE_PERF_COUNTERS)
endif()

# fp-contract stays off so separate PPC multiplies and adds are never fused into FMAs that round differently.
if(CHERRY_X86_64_V3 AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    foreach(_cherry_target re_cherry re_cherry_recomp)
        if(TARGET ${_cherry_target})
            target_compile_options(${_cherry_target} PRIVATE -march=x86-64-v3 -ffp-contract=off)
        endif()
    endforeach()
endif()
