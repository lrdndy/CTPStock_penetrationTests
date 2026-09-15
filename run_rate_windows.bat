@echo off
setlocal
cd /d "%~dp0"
rem An explicit max-orders budget and rate-test confirmation are required for live transmission.
rem Without --send-order this entry prints an offline plan only.
call run_windows.bat --mode trader --test rate-live %*
exit /b %ERRORLEVEL%
