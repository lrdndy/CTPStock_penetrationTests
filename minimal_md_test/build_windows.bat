@echo off
setlocal
cd /d "%~dp0"

for %%f in (ThostFtdcMdApi.h ThostFtdcUserApiStruct.h ThostFtdcUserApiDataType.h soptthostmduserapi_se.lib soptthostmduserapi_se.dll) do (
  if not exist "sdk\%%f" (
    echo ERROR: Missing minimal_md_test\sdk\%%f
    echo See README.md for the required v3.7.5 Windows x64 files.
    exit /b 1
  )
)

set "CTP_VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "CTP_VS_ROOT="
if exist "%CTP_VSWHERE%" for /f "usebackq tokens=*" %%i in (`"%CTP_VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "CTP_VS_ROOT=%%i"
if defined CTP_VS_ROOT goto setup_vs
if /i "%VSCMD_ARG_TGT_ARCH%"=="x64" goto compile
echo ERROR: Visual Studio x64 C++ tools not found.
echo Install Desktop development with C++ and Windows SDK.
exit /b 1

:setup_vs
call "%CTP_VS_ROOT%\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64
if errorlevel 1 exit /b 1

:compile
if not exist build mkdir build
cl.exe /nologo /std:c++17 /EHsc /W4 /utf-8 /O2 /MT /DWIN32 /DISLIB /DNOMINMAX /DWIN32_LEAN_AND_MEAN /Isdk main.cpp /Fe:build\minimal_md_login.exe /link /MACHINE:X64 /LIBPATH:sdk soptthostmduserapi_se.lib
if errorlevel 1 exit /b 1
copy /y sdk\soptthostmduserapi_se.dll build\ >nul
if errorlevel 1 exit /b 1
echo BUILD OK: build\minimal_md_login.exe
exit /b 0
