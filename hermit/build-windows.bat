@echo off
rem Hermit x64 release build: compile, deploy a runnable folder, zip it, and pack the
rem single-file exe. This is the supported way to build Hermit (see docs\building.md).
rem
rem Requirements:
rem   - Visual Studio 2022 or 2026 (or its Build Tools) with the C++ workload
rem   - Qt 6 for MSVC x64 (hermit\install_qt_msvc.py installs Qt 6.11.3 to C:\Qt)
rem   - Python 3 (source checks)
rem   - git submodule update --init --recursive
rem
rem Usage: hermit\build-windows.bat [QtBinDir]
rem   Qt is looked up in this order: the QtBinDir argument (the Qt "bin" folder), then
rem   %HERMIT_QT_DIR%\bin (the Qt kit folder, e.g. C:\Qt\6.11.3\msvc2022_64), then the
rem   default C:\Qt\6.11.3\msvc2022_64\bin.
rem Output: build\hermit-x64\Hermit-<version>.exe (single file), build\hermit-x64\deploy (runnable
rem folder) and build\hermit-x64\Hermit-x64-<version>.zip

setlocal enableDelayedExpansion

set SOURCE_ROOT=%~dp0..
for %%i in ("%SOURCE_ROOT%") do set SOURCE_ROOT=%%~fi
set QT_BIN=%~1
if "%QT_BIN%"=="" if not "%HERMIT_QT_DIR%"=="" set QT_BIN=%HERMIT_QT_DIR%\bin
if "%QT_BIN%"=="" set QT_BIN=C:\Qt\6.11.3\msvc2022_64\bin
set OUT_ROOT=%SOURCE_ROOT%\build\hermit-x64
set BUILD_FOLDER=%OUT_ROOT%\build
set DEPLOY_FOLDER=%OUT_ROOT%\deploy

if not exist "%QT_BIN%\qmake.exe" (
    echo Qt not found at %QT_BIN%. Pass the Qt bin folder as the first argument, set HERMIT_QT_DIR,
    echo or install Qt with: python hermit\install_qt_msvc.py
    exit /b 1
)

set VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe
if not exist "%VSWHERE%" (
    echo vswhere.exe not found. Install Visual Studio Build Tools with the C++ workload.
    exit /b 1
)
set VS_PATH=
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set VS_PATH=%%i
if "%VS_PATH%"=="" (
    echo No Visual Studio installation with the MSVC x64 toolset was found.
    exit /b 1
)

rem Hermit: mistakes that compile and only fail at runtime (QML members, translation placeholders)
set PYTHONUTF8=1
python "%SOURCE_ROOT%\hermit\tests\check-qml-members.py"
if !ERRORLEVEL! NEQ 0 (
    echo QML member check failed
    exit /b 1
)
python "%SOURCE_ROOT%\hermit\tests\check-translations.py"
if !ERRORLEVEL! NEQ 0 (
    echo Translation check failed
    exit /b 1
)
call "%VS_PATH%\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul
if !ERRORLEVEL! NEQ 0 goto Error
set PATH=%QT_BIN%;%PATH%

if not exist "%SOURCE_ROOT%\libs\windows\lib\x64" (
    echo Downloading prebuilt dependencies (setup-deps.ps1)
    powershell -NoProfile -ExecutionPolicy Bypass -File "%SOURCE_ROOT%\setup-deps.ps1"
    if !ERRORLEVEL! NEQ 0 goto Error
)

for /f "usebackq delims=" %%i in (`git -C "%SOURCE_ROOT%" describe --tags --always --dirty`) do set VERSION=%%i
echo Building Hermit !VERSION!

if exist "%DEPLOY_FOLDER%" rmdir /s /q "%DEPLOY_FOLDER%"
if not exist "%BUILD_FOLDER%" mkdir "%BUILD_FOLDER%"
mkdir "%DEPLOY_FOLDER%"

rem Let cl.exe compile the files of each batch in parallel.
set CL=/MP

pushd "%BUILD_FOLDER%"
qmake.exe "%SOURCE_ROOT%\hermit.pro"
if !ERRORLEVEL! NEQ 0 (popd & goto Error)
nmake /nologo release
if !ERRORLEVEL! NEQ 0 (popd & goto Error)
popd

echo Copying DLL dependencies
copy /y "%SOURCE_ROOT%\libs\windows\lib\x64\*.dll" "%DEPLOY_FOLDER%" >nul
if !ERRORLEVEL! NEQ 0 goto Error
copy /y "%BUILD_FOLDER%\AntiHooking\release\AntiHooking.dll" "%DEPLOY_FOLDER%" >nul
if !ERRORLEVEL! NEQ 0 goto Error
copy /y "%SOURCE_ROOT%\app\SDL_GameControllerDB\gamecontrollerdb.txt" "%DEPLOY_FOLDER%" >nul
if !ERRORLEVEL! NEQ 0 goto Error
rem Hermit has no Discord integration; the prebuilt dependency set still contains its library.
if exist "%DEPLOY_FOLDER%\discord-rpc.dll" del "%DEPLOY_FOLDER%\discord-rpc.dll"

set WINDEPLOYQT_ARGS=--no-system-d3d-compiler --no-system-dxc-compiler --skip-plugin-types qmltooling,generic --no-ffmpeg
set WINDEPLOYQT_ARGS=!WINDEPLOYQT_ARGS! --no-quickcontrols2fusion --no-quickcontrols2imagine --no-quickcontrols2universal
set WINDEPLOYQT_ARGS=!WINDEPLOYQT_ARGS! --no-quickcontrols2fusionstyleimpl --no-quickcontrols2imaginestyleimpl --no-quickcontrols2universalstyleimpl --no-quickcontrols2windowsstyleimpl --no-quickcontrols2fluentwinui3styleimpl

echo Deploying Qt dependencies
windeployqt.exe --dir "%DEPLOY_FOLDER%" --release --qmldir "%SOURCE_ROOT%\app\gui" --no-opengl-sw --no-compiler-runtime --no-sql !WINDEPLOYQT_ARGS! "%BUILD_FOLDER%\app\release\Hermit.exe"
if !ERRORLEVEL! NEQ 0 goto Error
copy /y "%BUILD_FOLDER%\app\release\Hermit.exe" "%DEPLOY_FOLDER%" >nul
if !ERRORLEVEL! NEQ 0 goto Error

for %%d in (Fusion Imagine Universal Windows FluentWinUI3) do if exist "%DEPLOY_FOLDER%\qml\QtQuick\Controls\%%d" rmdir /s /q "%DEPLOY_FOLDER%\qml\QtQuick\Controls\%%d"
if exist "%DEPLOY_FOLDER%\qml\QtQuick\NativeStyle" rmdir /s /q "%DEPLOY_FOLDER%\qml\QtQuick\NativeStyle"
if exist "%DEPLOY_FOLDER%\icuuc.dll" del "%DEPLOY_FOLDER%\icuuc.dll"

rem Ship the VC++ runtime next to the exe so the folder runs on PCs without the redistributable.
set VC_REDIST_DLL_PATH=
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -find VC\Redist\MSVC\*\x64\Microsoft.VC*.CRT`) do set VC_REDIST_DLL_PATH=%%i
if "!VC_REDIST_DLL_PATH!"=="" (
    echo Warning: VC++ runtime DLLs not found; the target PC needs the VC++ redistributable.
) else (
    copy /y "!VC_REDIST_DLL_PATH!\*.dll" "%DEPLOY_FOLDER%" >nul
    if !ERRORLEVEL! NEQ 0 goto Error
)

rem License of Hermit, NOTICE (credits and third-party components) and the license texts of the
rem bundled components. The single-file exe carries them inside, like the rest of this folder.
echo Copying license files
copy /y "%SOURCE_ROOT%\LICENSE" "%DEPLOY_FOLDER%\LICENSE.txt" >nul
if !ERRORLEVEL! NEQ 0 goto Error
copy /y "%SOURCE_ROOT%\NOTICE" "%DEPLOY_FOLDER%\NOTICE.txt" >nul
if !ERRORLEVEL! NEQ 0 goto Error
set LICENSES=%DEPLOY_FOLDER%\licenses
mkdir "!LICENSES!"
copy /y "%SOURCE_ROOT%\licenses\*.txt" "!LICENSES!" >nul
if !ERRORLEVEL! NEQ 0 goto Error
copy /y "%SOURCE_ROOT%\moonlight-common-c\moonlight-common-c\enet\LICENSE" "!LICENSES!\ENet.txt" >nul
if !ERRORLEVEL! NEQ 0 goto Error
copy /y "%SOURCE_ROOT%\moonlight-common-c\moonlight-common-c\nanors\LICENSE" "!LICENSES!\nanors.txt" >nul
if !ERRORLEVEL! NEQ 0 goto Error
copy /y "%SOURCE_ROOT%\qmdnsengine\qmdnsengine\LICENSE.txt" "!LICENSES!\qmdnsengine.txt" >nul
if !ERRORLEVEL! NEQ 0 goto Error
copy /y "%SOURCE_ROOT%\app\SDL_GameControllerDB\LICENSE" "!LICENSES!\SDL_GameControllerDB.txt" >nul
if !ERRORLEVEL! NEQ 0 goto Error
copy /y "%SOURCE_ROOT%\app\res\fonts\IBM-Plex-OFL.txt" "!LICENSES!\IBM-Plex-OFL.txt" >nul
if !ERRORLEVEL! NEQ 0 goto Error

set ZIP=%OUT_ROOT%\Hermit-x64-!VERSION!.zip
if exist "!ZIP!" del "!ZIP!"
powershell -NoProfile -Command "Compress-Archive -Path '%DEPLOY_FOLDER%\*' -DestinationPath '!ZIP!'"
if !ERRORLEVEL! NEQ 0 goto Error

rem Single-file exe: the deploy folder compressed into one launcher exe (hermit\onefile).
echo Building the single-file exe
set ONEFILE_SRC=%SOURCE_ROOT%\hermit\onefile
set ONEFILE_OUT=%OUT_ROOT%\onefile
if not exist "%ONEFILE_OUT%" mkdir "%ONEFILE_OUT%"
set CL=
pushd "%ONEFILE_OUT%"
rc /nologo /I "%ONEFILE_SRC%" /fo launcher.res "%ONEFILE_SRC%\launcher.rc"
if !ERRORLEVEL! NEQ 0 (popd & goto Error)
cl /nologo /utf-8 /O2 /MT /EHsc /W3 /DUNICODE /D_UNICODE "%ONEFILE_SRC%\launcher.cpp" launcher.res /Fe:launcher.exe /link /SUBSYSTEM:WINDOWS cabinet.lib user32.lib shell32.lib ole32.lib
if !ERRORLEVEL! NEQ 0 (popd & goto Error)
cl /nologo /utf-8 /O2 /MT /EHsc /W3 /DUNICODE /D_UNICODE "%ONEFILE_SRC%\pack.cpp" /Fe:pack.exe /link cabinet.lib
if !ERRORLEVEL! NEQ 0 (popd & goto Error)
set ONEFILE=%OUT_ROOT%\Hermit-!VERSION!.exe
"%ONEFILE_OUT%\pack.exe" "%ONEFILE_OUT%\launcher.exe" "%DEPLOY_FOLDER%" Hermit.exe "!ONEFILE!"
if !ERRORLEVEL! NEQ 0 (popd & goto Error)
popd

echo Build succeeded: !ONEFILE! (single file) and !ZIP! (folder)
exit /b 0

:Error
echo Build failed.
exit /b 1
