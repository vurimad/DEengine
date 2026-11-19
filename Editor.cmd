@echo off
rem Opens the DEengine editor: double-click it. With nothing after it, the editor opens the project and the world it
rem had open last (the sandbox the first time). Anything after it is passed on:
rem     Editor.cmd --project projects\sandbox
rem Inside the editor: File > Open World and Open Project, Cook at the bottom right (Ctrl+B), Preferences for the
rem frame rate limit.
setlocal
set "EXE=%~dp0build\msvc\bin\RelWithDebInfo\deengine.exe"
if not exist "%EXE%" (
    echo The editor isn't built yet:
    echo     %EXE%
    echo Build it from a terminal in this folder:
    echo     cmake --preset msvc
    echo     cmake --build --preset msvc-dev
    pause
    exit /b 1
)
start "" /D "%~dp0." "%EXE%" --editor %*
