@echo off
rem Build one CMake target with the TAR toolchain. Usage: tar-target.bat <target>
setlocal
set QT_ROOT=C:\Qt\6.11.1\msvc2022_64
set PATH=%APPDATA%\Python\Python312\site-packages\cmake\data\bin;%APPDATA%\Python\Python312\Scripts;%QT_ROOT%\bin;%PATH%
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cd /d "%~dp0"
cmake --build build --target %1 --parallel || exit /b 1
