@echo off
setlocal
cd /d "%~dp0"
rem This wrapper selects the guarded basic-function test. Without --send-order it is dry-run only.
call run_windows.bat --mode trader --test basic %*
exit /b %ERRORLEVEL%
