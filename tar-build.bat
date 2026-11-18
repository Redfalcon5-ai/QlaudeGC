@echo off
rem QlaudeGC build: configure (first run) + build RelWithDebInfo (no LTO, faster links) with Ninja + MSVC.
rem Usage: tar-build.bat [configure|build|all]   (default: all)
setlocal
set QT_ROOT=C:\Qt\6.11.1\msvc2022_64
set PATH=%APPDATA%\Python\Python312\site-packages\cmake\data\bin;%APPDATA%\Python\Python312\Scripts;%QT_ROOT%\bin;%PATH%
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cd /d "%~dp0"
set STEP=%1
if "%STEP%"=="" set STEP=all
if /i "%STEP%"=="build" goto build
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_PREFIX_PATH=%QT_ROOT% -DQT_HOST_PATH=%QT_ROOT% || exit /b 1
if /i "%STEP%"=="configure" exit /b 0
:build
cmake --build build --parallel || exit /b 1
