@echo off
REM Build snesref.exe from this directory.
REM
REM Prerequisites you must supply locally (intentionally NOT committed):
REM   - SDL2 dev package extracted here as SDL2-2.30.9\  (https://libsdl.org)
REM   - at runtime, a libretro SNES core DLL (e.g. snes9x_libretro.dll),
REM     passed as argv[1]. The core is licensed separately from this tool.
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
cd /d %~dp0
cl /nologo /EHsc /O2 /MD frontend.cpp /I "SDL2-2.30.9\include" /Fe:snesref.exe /link /SUBSYSTEM:CONSOLE "SDL2-2.30.9\lib\x64\SDL2.lib" shell32.lib user32.lib
echo BUILD_EXIT=%ERRORLEVEL%
