# Xenia side-by-side comparison mode (compare/xenia/README.md). Off by default; the default build never sees this folder's code.
option(CHERRY_XENIA_COMPARE "Build the Xenia side-by-side comparison mode (Windows only)" OFF)

if(CHERRY_XENIA_COMPARE)
    if(NOT WIN32)
        message(FATAL_ERROR "CHERRY_XENIA_COMPARE needs Windows: the native renderer's comparison window is a Win32 window")
    endif()
    # The game's own D3D must run so Xenia receives its ring packets: the native D3D hooks are replaced by this folder's pass-through hooks.
    list(FILTER CHERRY_SOURCES EXCLUDE REGEX "/src/gpu/hooks/")
    file(GLOB CHERRY_XENIA_COMPARE_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/*.cpp")
    list(APPEND CHERRY_SOURCES ${CHERRY_XENIA_COMPARE_SOURCES})
endif()

function(cherry_xenia_compare_target target)
    if(NOT CHERRY_XENIA_COMPARE)
        return()
    endif()
    target_compile_definitions(${target} PRIVATE CHERRY_XENIA_COMPARE=1)
    # Paired screenshots are encoded as PNG with the Windows Imaging Component.
    target_link_libraries(${target} PRIVATE windowscodecs)
    target_include_directories(${target} PRIVATE "${PROJECT_SOURCE_DIR}")
    # The runtime loads Xenia's GPU as a plugin DLL next to the executable; nothing links against it.
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:rex::gpu-xenos>" "$<TARGET_FILE_DIR:${target}>")
endfunction()
