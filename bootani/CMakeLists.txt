cmake_minimum_required(VERSION 3.16)
project(bootani C CXX)

# Portable rebuild of the original Xbox boot animation ("bootani") from
# xbox/private/ntos/ani2. See README.md.

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

option(BOOTANI_WITH_SDL2 "Windowed mode and audio playback through SDL2" ON)
option(BOOTANI_WITH_EGL  "Headless mode through EGL (Linux)" ON)

add_subdirectory(src/sound)

set(ANIM_SOURCES
    src/anim/CamControl.cpp
    src/anim/camera.cpp
    src/anim/fastmath.cpp
    src/anim/GreenFog.cpp
    src/anim/logo_renderer.cpp
    src/anim/qrand.cpp
    src/anim/renderer.cpp
    src/anim/scene_renderer.cpp
    src/anim/tex_gen.cpp
    src/anim/VBlob.cpp
    src/anim/xbs_app.cpp
)

add_executable(bootani
    src/main.cpp
    src/io/image_io.cpp
    src/gfx/d3d8_gl.cpp
    src/gfx/gl_loader.cpp
    src/gfx/shaders_glsl.cpp
    src/platform/platform.cpp
    ${ANIM_SOURCES}
)
target_include_directories(bootani PRIVATE
    src
    src/anim
    src/gfx
    third_party/khronos
)
target_link_libraries(bootani PRIVATE bootsound)
# The kernel build linked the textures and shaders into the image (BINARY_RESOURCE).
target_compile_definitions(bootani PRIVATE BINARY_RESOURCE)

if(MSVC)
    target_compile_definitions(bootani PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)
    target_compile_options(bootani PRIVATE /W3)
else()
    target_compile_options(bootani PRIVATE -Wall -Wno-unused-variable -Wno-unused-but-set-variable -Wno-conversion-null -Wno-switch -fno-strict-aliasing -fsigned-char -Wno-sign-compare -Wno-char-subscripts)
endif()

set(BOOTANI_HAVE_CONTEXT OFF)

if(BOOTANI_WITH_SDL2)
    find_package(SDL2 CONFIG QUIET)
    if(NOT SDL2_FOUND)
        find_package(PkgConfig QUIET)
        if(PkgConfig_FOUND)
            pkg_check_modules(SDL2PC IMPORTED_TARGET sdl2)
        endif()
    endif()
    if(TARGET SDL2::SDL2)
        target_link_libraries(bootani PRIVATE SDL2::SDL2)
        if(TARGET SDL2::SDL2main)
            target_link_libraries(bootani PRIVATE SDL2::SDL2main)
        endif()
    elseif(TARGET PkgConfig::SDL2PC)
        target_link_libraries(bootani PRIVATE PkgConfig::SDL2PC)
    endif()
    if(TARGET SDL2::SDL2 OR TARGET PkgConfig::SDL2PC)
        target_sources(bootani PRIVATE src/platform/platform_sdl.cpp)
        target_compile_definitions(bootani PRIVATE BOOTANI_HAVE_SDL2)
        set(BOOTANI_HAVE_CONTEXT ON)
        message(STATUS "bootani: SDL2 window/audio backend enabled")
    else()
        message(STATUS "bootani: SDL2 not found, no windowed mode")
    endif()
endif()

if(BOOTANI_WITH_EGL AND UNIX AND NOT APPLE)
    find_path(EGL_INCLUDE_DIR EGL/egl.h)
    find_library(EGL_LIBRARY NAMES EGL)
    if(EGL_INCLUDE_DIR AND EGL_LIBRARY)
        target_sources(bootani PRIVATE src/platform/platform_egl.cpp)
        target_include_directories(bootani PRIVATE ${EGL_INCLUDE_DIR})
        target_link_libraries(bootani PRIVATE ${EGL_LIBRARY})
        target_compile_definitions(bootani PRIVATE BOOTANI_HAVE_EGL)
        set(BOOTANI_HAVE_CONTEXT ON)
        message(STATUS "bootani: EGL headless backend enabled")
    else()
        message(STATUS "bootani: EGL not found, headless mode will use a hidden SDL window")
    endif()
endif()

if(NOT BOOTANI_HAVE_CONTEXT)
    message(FATAL_ERROR "bootani needs SDL2 (any platform) or EGL (Linux) to create an OpenGL context")
endif()
