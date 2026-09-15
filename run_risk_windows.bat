@echo off
setlocal
cd /d "%~dp0"
if /i "%~1"=="settings" goto settings
if /i "%~1"=="trigger" goto trigger
echo Usage: .\run_risk_windows.bat settings ^| trigger
echo This screenshot self-test does not connect or send an order.
exit /b 2

:settings
call run_windows.bat --test risk --risk-action settings
exit /b %ERRORLEVEL%

:trigger
call run_windows.bat --test risk --risk-action trigger --confirm TRIGGER_DAILY_ORDER_LIMIT
exit /b %ERRORLEVEL%
