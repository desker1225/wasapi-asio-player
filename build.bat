@echo off
rem Builds wasio-player with CMake + Ninja using the MSVC build tools. Override
rem VCVARS if vcvars64.bat lives somewhere other than C:\BuildTools.
rem
rem   set VCVARS=D:\somewhere\vcvars64.bat
rem   set ASIO_SDK_ROOT=D:\path\to\ASIOSDK234
rem   build.bat
rem
rem CMake and Ninja ship inside the same VS/BuildTools install as vcvars64.bat
rem but vcvars64.bat does not put them on PATH. vswhere.exe (installed
rem alongside every VS/BuildTools setup) reports the install root so their bin
rem directories can be added; if vswhere is missing, the root is derived by
rem walking up from VCVARS itself, since vcvars64.bat always sits at
rem <root>\VC\Auxiliary\Build\ on every edition.
rem
rem ASIO_SDK_ROOT is only forwarded to CMake when set; CMakeLists.txt requires
rem it and fails with a clear message if it is missing.

setlocal enabledelayedexpansion
if "%VCVARS%"=="" set VCVARS=C:\BuildTools\VC\Auxiliary\Build\vcvars64.bat
if not exist "%VCVARS%" (
    echo cannot find %VCVARS%
    echo set VCVARS to your vcvars64.bat and try again
    exit /b 1
)

call "%VCVARS%" >nul 2>&1
if errorlevel 1 exit /b 1

set VS_ROOT=
set VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe
if exist "%VSWHERE%" (
    for /f "usebackq delims=" %%I in (`"%VSWHERE%" -products * -property installationPath`) do (
        if "!VS_ROOT!"=="" set VS_ROOT=%%I
    )
)
if "%VS_ROOT%"=="" for %%I in ("%VCVARS%\..\..\..\..") do set VS_ROOT=%%~fI

set CMAKE_BIN=%VS_ROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin
set NINJA_BIN=%VS_ROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja
if exist "%CMAKE_BIN%\cmake.exe" set "PATH=%CMAKE_BIN%;%PATH%"
if exist "%NINJA_BIN%\ninja.exe" set "PATH=%NINJA_BIN%;%PATH%"

where cmake >nul 2>&1
if errorlevel 1 (
    echo cannot find cmake.exe - install the "C++ CMake tools for Windows" component or put cmake on PATH
    exit /b 1
)
where ninja >nul 2>&1
if errorlevel 1 (
    echo cannot find ninja.exe - install the "C++ CMake tools for Windows" component or put ninja on PATH
    exit /b 1
)

pushd "%~dp0"
set BUILD_DIR=build-ninja
set CONFIGURE_ARGS=-S . -B %BUILD_DIR% -G Ninja
if not "%ASIO_SDK_ROOT%"=="" set CONFIGURE_ARGS=%CONFIGURE_ARGS% -DASIO_SDK_ROOT="%ASIO_SDK_ROOT%"

cmake %CONFIGURE_ARGS%
set RC=%errorlevel%
if not "%RC%"=="0" goto done

cmake --build %BUILD_DIR% --parallel 4
set RC=%errorlevel%

:done
popd
if "%RC%"=="0" (
    echo built %BUILD_DIR%\bin - run "ctest --test-dir %BUILD_DIR% --output-on-failure" to test
) else (
    echo build failed, see output above
)
exit /b %RC%
