# Portable Xbox boot sound (original SOS sequencer + software MCPX voices)
cmake_minimum_required(VERSION 3.10)
project(bootsound C)

add_library(bootsound STATIC
    bootsound.c
    dsound_soft.c
    dev.c
    cf.c
    evf.c
    globals.c
    proc.c
    sos.c
    stboot.c
)
set_target_properties(bootsound PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF)
target_include_directories(bootsound PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
if(MSVC)
    target_compile_options(bootsound PRIVATE /W3)
    target_compile_definitions(bootsound PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(bootsound PRIVATE -Wall)
    target_link_libraries(bootsound PUBLIC m)
endif()

# WAV dump test tool (bootani/tools/bootsound_wav.c)
option(BOOTSOUND_BUILD_WAV_TOOL "Build the bootsound_wav test tool" ON)
set(_bootsound_wav_src ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/bootsound_wav.c)
if(BOOTSOUND_BUILD_WAV_TOOL AND EXISTS ${_bootsound_wav_src})
    add_executable(bootsound_wav ${_bootsound_wav_src})
    target_link_libraries(bootsound_wav PRIVATE bootsound)
    set_target_properties(bootsound_wav PROPERTIES C_STANDARD 99)
    if(NOT MSVC)
        target_compile_options(bootsound_wav PRIVATE -Wall)
    endif()
endif()
