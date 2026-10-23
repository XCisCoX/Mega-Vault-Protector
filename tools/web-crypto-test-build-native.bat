@echo off
setlocal
rem Reuses the installed Windows compiler/core dependencies; installs nothing.
for %%I in ("%~dp0..") do set "REPO=%%~fI"
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq tokens=*" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSROOT=%%I"
if not defined VSROOT exit /b 2
call "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b %errorlevel%
set "SCRATCH=%LOCALAPPDATA%\hermes\cache\scratch\mvpk-crypto"
if not exist "%SCRATCH%" mkdir "%SCRATCH%"
cd /d "%SCRATCH%"
set "DEPS=%LOCALAPPDATA%\MegaVideoProtect\vcpkg_installed\x64-windows"
cmake --build "%REPO%\out\build\vs2022-x64" --config Release --target VideoVaultCore -- /m
if errorlevel 1 exit /b %errorlevel%
cl /nologo /std:c++20 /EHsc /MD /utf-8 /I"%REPO%\core\include" /I"%REPO%\core\src" /I"%DEPS%\include" "%REPO%\tools\web-crypto-test-native.cpp" /Fe:web-crypto-native.exe /link "%REPO%\out\build\vs2022-x64\core\Release\VideoVaultCore.lib" /LIBPATH:"%DEPS%\lib" libsodium.lib argon2.lib sqlcipher.lib avformat.lib avcodec.lib avutil.lib swscale.lib swresample.lib zlib.lib
exit /b %errorlevel%
