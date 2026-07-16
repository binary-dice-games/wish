@echo off
REM Run this file to use this extracted wish release in the current cmd.exe
REM session only -- no persistent changes are made:
REM
REM   wish-env.cmd
REM
REM For a setup that persists across new sessions, run install.ps1 once
REM instead (PowerShell; cmd.exe has no equivalent user-PATH API).

set "WISH_ROOT=%~dp0"
set "PATH=%WISH_ROOT%bin;%PATH%"
set "WISH_LIB=%WISH_ROOT%bin\wish_client.dll"

echo wish is on PATH for this session (try: wish server --renderer web)
