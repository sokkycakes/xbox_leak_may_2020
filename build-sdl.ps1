$ErrorActionPreference = 'Stop'
$guideRoot = $PSScriptRoot
$guideCompiler = (Get-Command clang).Source
$guideCompilerDirectory = Split-Path -Parent $guideCompiler
$guideC = Join-Path $guideCompilerDirectory 'i686-w64-mingw32-clang.exe'
$guideCpp = Join-Path $guideCompilerDirectory 'i686-w64-mingw32-clang++.exe'
cmake -S "$guideRoot/build/SDL3-3.4.18" -B "$guideRoot/build/sdl-x86" -G Ninja `
    '-DCMAKE_SYSTEM_NAME=Windows' '-DCMAKE_SYSTEM_PROCESSOR=x86' `
    "-DCMAKE_C_COMPILER=$guideC" "-DCMAKE_CXX_COMPILER=$guideCpp" `
    '-DCMAKE_BUILD_TYPE=Release' '-DSDL_SHARED=OFF' '-DSDL_STATIC=ON' `
    '-DSDL_TEST_LIBRARY=OFF' '-DSDL_TESTS=OFF' '-DSDL_EXAMPLES=OFF' `
    '-DSDL_AUDIO=OFF' '-DSDL_RENDER=OFF' '-DSDL_GPU=OFF' '-DSDL_CAMERA=OFF' `
    '-DSDL_HAPTIC=OFF' '-DSDL_SENSOR=OFF' '-DSDL_DIALOG=OFF' '-DSDL_OPENGL=OFF' `
    '-DSDL_OPENGLES=OFF' '-DSDL_VULKAN=OFF'
if ($LASTEXITCODE -ne 0) { throw 'SDL configuration failed' }
cmake --build "$guideRoot/build/sdl-x86" -j 8
if ($LASTEXITCODE -ne 0) { throw 'SDL build failed' }
