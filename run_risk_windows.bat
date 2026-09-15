@echo off
setlocal
cd /d "%~dp0"
if /i "%~1"=="settings" goto settings
if /i "%~1"=="trigger" goto trigger
if /i "%~1"=="trigger-second" goto trigger_second
echo Usage: .\run_risk_windows.bat settings ^| trigger ^| trigger-second
echo This screenshot self-test does not connect or send an order.
exit /b 2

:settings
call run_windows.bat --test risk --risk-action settings
exit /b %ERRORLEVEL%

:trigger
call run_windows.bat --test risk --risk-action trigger --confirm TRIGGER_DAILY_ORDER_LIMIT
exit /b %ERRORLEVEL%

:trigger_second
call run_windows.bat --test risk --risk-action trigger-second --confirm TRIGGER_SECOND_ORDER_LIMIT
exit /b %ERRORLEVEL%
