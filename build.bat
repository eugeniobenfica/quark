@echo off
rem Builds build\libquark.a, build\quark.exe and build\host.exe with MSYS2's gcc (UCRT64), through build.sh.
rem Quark itself needs no compiler to run programs.
setlocal
set PATH=C:\msys64\ucrt64\bin;C:\msys64\usr\bin;%PATH%
bash "%~dp0build.sh"
