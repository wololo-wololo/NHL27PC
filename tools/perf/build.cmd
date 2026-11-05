@echo off
set PATH=C:\Program Files (x86)\Microsoft Visual Studio\Installer;%PATH%
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
set PATH=C:\Program Files\LLVM\bin;C:\Users\Administrator\Downloads\NHL.27.PS5-DUPLEX\nhl27pc\.tools\glslang-16.6.0\bin;%PATH%
cd /d C:\Users\Administrator\Downloads\NHL.27.PS5-DUPLEX\nhl27pc
cmake -S . -B _Build\windows-local -DKYTY_BUILD_LAUNCHER=OFF >_Build\configure.log 2>&1 || (type _Build\configure.log & exit /b 1)
cmake --build _Build\windows-local --config Release --target kyty_emulator -j 16
