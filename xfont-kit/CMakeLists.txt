cmake_minimum_required(VERSION 3.10)
project(xfont C CXX)

# XFONT: the Xbox SDK's text library (GDI-style TextOut API, bitmap fonts,
# and the Windows TrueType scaler), built as a plain static library.

set(XFONT_SCALER
    src/scaler/fnterr.c src/scaler/fontmath.c src/scaler/fscaler.c
    src/scaler/fsglue.c src/scaler/interp.c src/scaler/scale.c
    src/scaler/scanlist.c src/scaler/scbitmap.c src/scaler/scendpt.c
    src/scaler/scentry.c src/scaler/scline.c src/scaler/scmemory.c
    src/scaler/scspline.c src/scaler/sfntaccs.c src/scaler/subpixel.c
    src/scaler/sbit.c)

set(XFONT_LIBRARY
    src/library/xfont.c src/library/bitmap.cpp
    src/library/painttext.cpp src/library/truetype.cpp)

if(NOT WIN32)
    list(APPEND XFONT_LIBRARY port/posix/win32_shim.c)
endif()

add_library(xfont STATIC ${XFONT_SCALER} ${XFONT_LIBRARY})
target_include_directories(xfont
    PUBLIC include port/common
    PRIVATE src/scaler src/library)
if(NOT WIN32)
    target_include_directories(xfont PUBLIC port/posix)
endif()
target_compile_definitions(xfont PUBLIC XFONT_TRUETYPE PRIVATE UNICODE _UNICODE)
if(MINGW)
    # MSVC accepts the one-underscore spelling the sources use.
    target_compile_definitions(xfont PRIVATE _declspec=__declspec)
endif()
if(MSVC)
    target_compile_options(xfont PRIVATE /W1)
else()
    # 25-year-old code: keep the build quiet.
    target_compile_options(xfont PRIVATE -w -fno-strict-aliasing)
endif()

option(XFONT_BUILD_DEMO "Build the xfont_demo program" ON)
if(XFONT_BUILD_DEMO)
    add_executable(xfont_demo tools/demo/xfont_demo.c)
    target_link_libraries(xfont_demo xfont)
endif()

# makefont turns a TrueType font into an XFONT bitmap font, using XFONT's
# own scaler.  It is written against the Win32 wide-string CRT, so it only
# builds for Windows.
if(WIN32)
    add_executable(makefont tools/makefont/makefont.cpp)
    target_include_directories(makefont PRIVATE tools/makefont src/library)
    target_compile_definitions(makefont PRIVATE UNICODE _UNICODE)
    target_link_libraries(makefont xfont)
    if(MINGW)
        target_compile_definitions(makefont PRIVATE _declspec=__declspec)
        target_link_options(makefont PRIVATE -municode)
    endif()
endif()
