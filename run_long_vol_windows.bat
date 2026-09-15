@echo off
setlocal
cd /d "%~dp0"
chcp 65001 >nul
if not exist build\bin\ctp_long_vol.exe (
  echo ERROR: Run build_long_vol_windows.bat first.
  exit /b 1
)
build\bin\ctp_long_vol.exe %*
set "CTP_LONG_VOL_EXIT=%ERRORLEVEL%"
echo.
echo Process exit code: %CTP_LONG_VOL_EXIT%
exit /b %CTP_LONG_VOL_EXIT%
