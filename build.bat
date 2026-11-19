@echo off
rem Builds build\quark.exe and build\libquark.a through build.sh, with MSYS2's bash (and its gcc, UCRT64, for
rem build\host.exe), from a quark installed (or QUARK=path). Quark itself needs no C compiler to run programs.
setlocal
set PATH=C:\msys64\ucrt64\bin;C:\msys64\usr\bin;%PATH%
bash "%~dp0build.sh"
