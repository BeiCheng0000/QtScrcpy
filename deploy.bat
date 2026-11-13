@echo off
rem ============================================================
rem  QtScrcpy one-click deploy script
rem  Usage: after editing code, double-click this file to build + deploy + launch
rem ============================================================
setlocal

rem ===== config: adjust paths below if your layout differs =====
set "PROJECT_DIR=e:\wk\wk_python\QtScrcpy-dev"
set "BUILD_DIR=%PROJECT_DIR%\build_temp"
set "SRC_EXE=%PROJECT_DIR%\output\x64\RelWithDebInfo\QtScrcpy.exe"
set "PUB_DIR=%PROJECT_DIR%\QtScrcpy-win-x64"
set "PUB_EXE=%PUB_DIR%\QtScrcpy.exe"
set "CMAKE=C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
set "VSVARS=C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvarsall.bat"

rem ===== prepare MSVC build environment =====
if exist "%VSVARS%" (
    call "%VSVARS%" x64 >nul 2>&1
)

echo ============================================
echo  QtScrcpy deploy
echo   project: %PROJECT_DIR%
echo   publish: %PUB_DIR%
echo ============================================

rem ===== 1. stop running QtScrcpy (release file lock) =====
echo [1/3] stopping old process ...
taskkill /IM QtScrcpy.exe /F
ping -n 2 127.0.0.1 >nul

rem ===== 2. build RelWithDebInfo =====
echo [2/3] building RelWithDebInfo ...
"%CMAKE%" --build "%BUILD_DIR%" --config RelWithDebInfo -j8
if errorlevel 1 (
    echo.
    echo BUILD FAILED.
    pause
    exit /b 1
)

rem ===== 3. copy to publish dir and launch =====
echo [3/3] deploying to publish dir ...
if not exist "%SRC_EXE%" (
    echo build output not found: %SRC_EXE%
    pause
    exit /b 1
)
copy /Y "%SRC_EXE%" "%PUB_EXE%" >nul
echo copied: %PUB_EXE%

start "" /D "%PUB_DIR%" "%PUB_EXE%"
echo deploy done, app launched.
endlocal
