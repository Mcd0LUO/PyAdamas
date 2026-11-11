@echo off
REM py23.cmd - Windows entry point. Forwards the command to the py23-dev
REM container inside WSL2 (Debian).
REM
REM   py23 run x.py
REM   py23 check x.py
REM   py23 build x.py -o out.exe
REM
REM NOTE: keep this file pure ASCII. cmd.exe reads it in the OEM codepage
REM (936 on this machine), so UTF-8 Chinese in REM lines corrupts the
REM command stream. See py23.ps1 and README.md for the details.
setlocal
where pwsh >nul 2>&1
if %ERRORLEVEL%==0 (
  pwsh -NoProfile -ExecutionPolicy Bypass -File "%~dp0py23.ps1" %*
) else (
  powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0py23.ps1" %*
)
exit /b %ERRORLEVEL%