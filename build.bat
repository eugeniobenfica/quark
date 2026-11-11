@echo off
rem Builds build\quark.exe with MSYS2's gcc (UCRT64). Quark itself needs no compiler to run programs.
setlocal
set PATH=C:\msys64\ucrt64\bin;%PATH%
if not exist "%~dp0build" mkdir "%~dp0build"
gcc -std=c11 -O2 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -Wno-format-truncation -o "%~dp0build\quark.exe" "%~dp0src\*.c" -lm
