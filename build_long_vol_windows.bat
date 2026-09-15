@echo off
setlocal
cd /d "%~dp0"
if not exist sdk\win64\soptthosttraderapi_se.dll goto missing_sdk
if not exist sdk\win64\soptthostmduserapi_se.dll goto missing_sdk
set "CTP_VOL_VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "CTP_VOL_VS_ROOT="
if exist "%CTP_VOL_VSWHERE%" for /f "usebackq tokens=*" %%i in (`"%CTP_VOL_VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "CTP_VOL_VS_ROOT=%%i"
if defined CTP_VOL_VS_ROOT goto setup_vs
if /i "%VSCMD_ARG_TGT_ARCH%"=="x64" goto compile
echo ERROR: Install Visual Studio Desktop development with C++, or use x64 Native Tools.
exit /b 1
:setup_vs
call "%CTP_VOL_VS_ROOT%\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64
if errorlevel 1 exit /b 1
:compile
where cl.exe >nul 2>&1
if errorlevel 1 exit /b 1
if not exist build\bin mkdir build\bin
if not exist build\obj mkdir build\obj
cl.exe /nologo /std:c++17 /EHsc /W4 /utf-8 /O2 /MT /DWIN32 /DISLIB /DNOMINMAX /DWIN32_LEAN_AND_MEAN /external:Isdk\include /external:W0 /Fo:build\obj\long_volatility.obj /Fe:build\bin\ctp_long_vol.exe src\long_volatility.cpp /link /MACHINE:X64 /LIBPATH:sdk\win64 soptthosttraderapi_se.lib soptthostmduserapi_se.lib advapi32.lib user32.lib
if errorlevel 1 exit /b 1
copy /y sdk\win64\soptthosttraderapi_se.dll build\bin\ >nul
if errorlevel 1 exit /b 1
copy /y sdk\win64\soptthostmduserapi_se.dll build\bin\ >nul
if errorlevel 1 exit /b 1
echo BUILD OK: build\bin\ctp_long_vol.exe
echo Next: run_long_vol_windows.bat --help
exit /b 0
:missing_sdk
echo ERROR: Copy both Windows x64 v3.7.5_CP_20251125 vendor DLLs into sdk\win64 first.
exit /b 1
