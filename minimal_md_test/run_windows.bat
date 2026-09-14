@echo off
setlocal
cd /d "%~dp0"
if not exist build\minimal_md_login.exe (
  echo ERROR: build\minimal_md_login.exe not found. Run build_windows.bat first.
  exit /b 1
)
build\minimal_md_login.exe
exit /b %errorlevel%
