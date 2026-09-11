@echo off
setlocal
cd /d "%~dp0"
chcp 65001 >nul
if not exist build\bin\ctp_stock_connect.exe (
  echo ERROR: Run build_windows.bat first.
  exit /b 1
)
build\bin\ctp_stock_connect.exe --config config\connection.ini %*
set "CTP_EXIT=%ERRORLEVEL%"
echo.
echo Process exit code: %CTP_EXIT%
exit /b %CTP_EXIT%
