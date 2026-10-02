#requires -Version 5.1
<#
.SYNOPSIS
Builds and runs Hermit's standalone tests (no app build, no stream, no host needed).
.DESCRIPTION
  hermit_core_test       branding translator, session summary maths and history file,
                         performance overlay text, automatic bitrate, display aspect
                         resolution presets, connection profile property names
  clipboard_archive_test clipboard file archive: streamed upload, throttling, validation,
                         extraction, cancel (uses a loopback HTTP server); host file lists
  clipboard_virtual_files_test
                         host files pasted as virtual files: file descriptors, file streams
                         that download while read (resume, seek, refusal, cancel, end of
                         stream) from a loopback HTTP server; never touches the clipboard
  check-qml-members.py   QML uses only members the exposed C++ objects have (a misspelt or
                         removed one fails only when that line runs)
  check-translations.py  Korean translations keep their %1..%9 placeholders

Needs Visual Studio (or its Build Tools) with MSVC and Qt 6 for MSVC x64 (see docs\building.md).
The Qt kit folder is -QtDir, else the HERMIT_QT_DIR environment variable, else
C:\Qt\6.11.3\msvc2022_64. Objects and executables go to build\tests. Exit code is non-zero if
any test fails.
.EXAMPLE
powershell -ExecutionPolicy Bypass -File hermit\tests\run-tests.ps1
.EXAMPLE
powershell -ExecutionPolicy Bypass -File hermit\tests\run-tests.ps1 -QtDir D:\Qt\6.11.3\msvc2022_64
#>
[CmdletBinding()]
param(
    [string]$QtDir = $(if ($env:HERMIT_QT_DIR) { $env:HERMIT_QT_DIR } else { 'C:\Qt\6.11.3\msvc2022_64' })
)
$ErrorActionPreference = 'Stop'

$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$app = Join-Path $repo 'app'
$out = Join-Path $repo 'build\tests'
New-Item -ItemType Directory -Force -Path $out | Out-Null

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'Visual Studio with the MSVC x64 tools was not found.' }
if (-not (Test-Path (Join-Path $QtDir 'bin\Qt6Core.dll'))) { throw "Qt not found at $QtDir" }

$common = "/nologo /EHsc /std:c++17 /permissive- /Zc:__cplusplus /utf-8 /MD /W3 /DUNICODE /D_UNICODE " +
          "/I`"$app`" /I`"$QtDir\include`" /I`"$QtDir\include\QtCore`" /I`"$QtDir\include\QtNetwork`" /I`"$QtDir\include\QtQml`" /I`"$QtDir\include\QtGui`" " +
          "/I`"$repo\moonlight-common-c\moonlight-common-c\src`" /I`"$repo\libs\windows\include`" " +
          "/I`"$repo\libs\windows\include\x64`" /I`"$repo\libs\windows\include\x64\SDL2`""

$tests = @(
    @{ Name = 'hermit_core_test'; Sources = @(
        "$PSScriptRoot\hermit_core_test.cpp",
        "$app\settings\brandingtranslator.cpp",
        "$app\streaming\sessionsummary.cpp",
        "$app\streaming\video\statsoverlay.cpp"); Args = @($repo) },
    @{ Name = 'clipboard_archive_test'; Sources = @(
        "$PSScriptRoot\clipboard_archive_test.cpp",
        "$app\streaming\clipboardarchive.cpp"); Args = @() },
    @{ Name = 'clipboard_virtual_files_test'; Sources = @(
        "$PSScriptRoot\clipboard_virtual_files_test.cpp",
        "$app\streaming\clipboardarchive.cpp"); Args = @();
       Libs = @("$repo\libs\windows\lib\x64\SDL2.lib", 'ole32.lib', 'shell32.lib', 'user32.lib') }
)

# SessionSummary declares a QObject (SessionHistory); run moc on its header for the test build.
& (Join-Path $QtDir 'bin\moc.exe') "$app\streaming\sessionsummary.h" -o "$out\moc_sessionsummary.cpp" "-I$app"
if ($LASTEXITCODE -ne 0) { throw 'moc failed' }
$tests[0].Sources += "$out\moc_sessionsummary.cpp"

$env:PATH = "$QtDir\bin;$repo\libs\windows\lib\x64;$env:PATH"
$failed = @()

# Source checks for mistakes that only show at runtime
$env:PYTHONUTF8 = '1'
foreach ($check in @('check-qml-members.py', 'check-translations.py')) {
    Write-Host ""
    Write-Host "== $check"
    & python (Join-Path $PSScriptRoot $check)
    if ($LASTEXITCODE -ne 0) { $failed += $check }
}
foreach ($t in $tests) {
    Write-Host ""
    Write-Host "== $($t.Name)"
    $exe = Join-Path $out "$($t.Name).exe"
    Remove-Item -LiteralPath $exe -ErrorAction SilentlyContinue
    $sources = ($t.Sources | ForEach-Object { "`"$_`"" }) -join ' '
    $libs = (@($t.Libs) | Where-Object { $_ } | ForEach-Object { "`"$_`"" }) -join ' '
    $cmd = "`"$vs\VC\Auxiliary\Build\vcvarsall.bat`" x64 >nul 2>nul && cd /d `"$out`" && " +
           "cl $common $sources /Fe:`"$exe`" /link `"$QtDir\lib\Qt6Core.lib`" `"$QtDir\lib\Qt6Network.lib`" $libs"
    $output = @(cmd /c $cmd 2>&1 | ForEach-Object { "$_" })
    $compile = @($output | Where-Object { $_ -match '\berror\b|warning C(?!4996|4005)' })
    $compile | ForEach-Object { Write-Host "  $_" }
    if (-not (Test-Path $exe) -or @($compile | Where-Object { $_ -match '\berror\b' }).Count) {
        $failed += "$($t.Name) (build)"
        continue
    }
    & $exe @($t.Args)
    if ($LASTEXITCODE -ne 0) { $failed += $t.Name }
}

Write-Host ""
if ($failed.Count) {
    Write-Host "FAILED: $($failed -join ', ')"
    exit 1
}
Write-Host 'All Hermit tests passed.'
