@echo off
setlocal
cd /d "%~dp0"
if /i not "%~1"=="trigger" goto usage
if "%~2"=="" goto usage
if not "%~3"=="" goto usage
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\test_price_format.ps1" -Price "%~2"
exit /b %ERRORLEVEL%

:usage
echo Usage: .\run_price_windows.bat trigger INVALID_PRICE
echo Example: .\run_price_windows.bat trigger 0.0427abc
echo Tests the existing executable's price parser. No orders are sent.
exit /b 2
