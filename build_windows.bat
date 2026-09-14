@echo off
setlocal
cd /d "%~dp0"
rem Vendor runtime DLLs are intentionally not stored in the public repository.
if not exist sdk\win64\soptthosttraderapi_se.dll (
  echo ERROR: Missing sdk\win64\soptthosttraderapi_se.dll
  echo Copy the Windows x64 v3.7.5_CP_20251125 DLL from the vendor SDK package into sdk\win64.
  exit /b 1
)
if not exist sdk\win64\soptthostmduserapi_se.dll (
  echo ERROR: Missing sdk\win64\soptthostmduserapi_se.dll
  echo Copy the Windows x64 v3.7.5_CP_20251125 DLL from the vendor SDK package into sdk\win64.
  exit /b 1
)
rem Locate a Visual Studio installation with the x64 C++ toolchain.
rem Direct cl build: CMake is not required.
set "CTP_VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "CTP_VS_ROOT="
if exist "%CTP_VSWHERE%" for /f "usebackq tokens=*" %%i in (`"%CTP_VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "CTP_VS_ROOT=%%i"
if defined CTP_VS_ROOT goto setup_vs
if /i "%VSCMD_ARG_TGT_ARCH%"=="x64" goto compile
echo ERROR: Visual Studio x64 C++ tools not found.
echo Install Desktop development with C++ and Windows SDK.
echo Or run this script from an x64 Native Tools Command Prompt.
exit /b 1
:setup_vs
call "%CTP_VS_ROOT%\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64
if errorlevel 1 exit /b 1
:compile
where cl.exe >nul 2>&1
if errorlevel 1 exit /b 1
if not exist build\bin mkdir build\bin
if not exist build\obj mkdir build\obj
rem SDK headers under sdk/include are UTF-8 copies; sdk/win64 is original.
cl.exe /nologo /std:c++17 /EHsc /W4 /utf-8 /O2 /MT /DWIN32 /DISLIB /DNOMINMAX /DWIN32_LEAN_AND_MEAN /external:Isdk\include /external:W0 /Fo:build\obj\main.obj /Fe:build\bin\ctp_stock_connect.exe src\main.cpp /link /MACHINE:X64 /LIBPATH:sdk\win64 soptthosttraderapi_se.lib soptthostmduserapi_se.lib advapi32.lib
if errorlevel 1 exit /b 1
copy /y sdk\win64\soptthosttraderapi_se.dll build\bin\ >nul
if errorlevel 1 exit /b 1
copy /y sdk\win64\soptthostmduserapi_se.dll build\bin\ >nul
if errorlevel 1 exit /b 1
echo BUILD OK: build\bin\ctp_stock_connect.exe
echo Next: run_windows.bat --mode all
exit /b 0
