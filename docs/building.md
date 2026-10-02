# Building Hermit

Hermit is built on Windows with MSVC and Qt 6 through a single script, `hermit\build-windows.bat`.
It produces a portable folder, a ZIP of that folder and a single-file exe. This page also covers the
tests, translations and icons.

Windows 10/11 x64 is the supported platform. The macOS, Linux and embedded code paths inherited
from Moonlight are still in the source tree, but they are not maintained or tested.

## Requirements

- Windows 10 or 11, x64
- Visual Studio 2022 or 2026, or the matching Build Tools, with the **Desktop development with C++**
  workload (MSVC x64 toolset). The build finds it with `vswhere`.
- Qt 6 for MSVC x64 with Qt Quick Controls and Qt SVG. Hermit is developed with **Qt 6.11.3**
  (`msvc2022_64`); other Qt 6 releases may work but are not tested.
- Python 3 (the build runs two source checks)
- Git, with the submodules checked out

## One-time setup

```powershell
# Visual Studio Build Tools with the C++ workload
winget install --id Microsoft.VisualStudio.BuildTools -e --override "--quiet --wait --norestart --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"

# Python, and py7zr for the Qt installer script
winget install --id Python.Python.3.13 -e
python -m pip install --user py7zr

# Qt 6.11.3 for MSVC x64, installed to C:\Qt\6.11.3\msvc2022_64
python hermit\install_qt_msvc.py

# Submodules: moonlight-common-c, qmdnsengine, SDL_GameControllerDB
git submodule update --init --recursive
```

`hermit\install_qt_msvc.py` downloads the Qt SDK straight from the official `download.qt.io`
repository: it reads the repository's `Updates.xml`, downloads the archives of the main desktop
package (documentation is skipped), verifies each against its published SHA-1 checksum and extracts
it. It exists because the Qt 6.11 repository layout is not understood by aqtinstall 3.3.0. Options:
`--version` (default `6.11.3`) and `--dest` (default `C:/Qt`). You can also install Qt with the
official Qt online installer instead.

### Using another Qt installation

The scripts default to `C:\Qt\6.11.3\msvc2022_64`. To use Qt from another location, either

- set the `HERMIT_QT_DIR` environment variable to the Qt kit folder (the folder that contains `bin`),
  for example `set HERMIT_QT_DIR=D:\Qt\6.11.3\msvc2022_64`, which `build-windows.bat`,
  `run-tests.ps1` and `make_hermit_ts.py` all honor, or
- pass the Qt `bin` folder to the build script: `hermit\build-windows.bat D:\Qt\6.11.3\msvc2022_64\bin`,
  and `-QtDir D:\Qt\6.11.3\msvc2022_64` to `run-tests.ps1`.

## Build

```powershell
hermit\build-windows.bat
```

The script:

1. runs the source checks (`hermit\tests\check-qml-members.py`, `hermit\tests\check-translations.py`);
2. downloads the prebuilt dependencies on the first build (see below);
3. compiles with qmake and nmake (files are compiled in parallel with `/MP`);
4. deploys a runnable folder with `windeployqt` and the Visual C++ runtime DLLs, so it runs on PCs
   without the redistributable;
5. copies `LICENSE`, `NOTICE` and the third-party license texts (`licenses\` and the submodules'
   license files) into the folder;
6. zips the folder and builds the single-file exe.

Output, where `<version>` is the output of `git describe --tags --always --dirty`:

| Path | Contents |
|---|---|
| `build\hermit-x64\deploy` | The runnable folder |
| `build\hermit-x64\Hermit-x64-<version>.zip` | The same folder as a ZIP |
| `build\hermit-x64\Hermit-<version>.exe` | The single-file exe |

The script does not sign the binaries and does not build an installer.

### The single-file exe

`hermit\onefile` turns the deploy folder into one exe without external tools:

- A small launcher exe (`launcher.cpp`) is followed by the whole deploy folder, compressed with the
  Windows built-in LZMS compression (`pack.cpp` writes it; `onefile.h` describes the layout).
- On its first start, the launcher unpacks the folder once to `%LOCALAPPDATA%\Hermit\app\<hash>`; later
  starts run the unpacked copy directly. When a new version starts for the first time, unpacked older
  versions that are not running are removed.
- Command-line arguments are passed on to `Hermit.exe`, and the launcher returns Hermit's exit code.
- Settings and pairings live in the registry (`HKCU\Software\Hermit\Hermit`), so replacing the exe
  keeps them.
- Pinning Hermit to the taskbar pins the unpacked `Hermit.exe`. After an update, pin it again, or use a
  desktop shortcut to the single-file exe.
- The launcher's error messages are in English, or in Korean when the Windows display language is
  Korean.

## Prebuilt dependencies

On the first build, `build-windows.bat` runs `setup-deps.ps1`, which downloads prebuilt Windows
libraries from the public GitHub repository
[moonlight-stream/moonlight-qt-deps](https://github.com/moonlight-stream/moonlight-qt-deps)
(release `v18.1`, `windows-x64.zip` and `windows-ARM64.zip`) into `libs\windows`. These are the same
dependency builds Moonlight uses: SDL2 (sdl2-compat on SDL3), SDL2_ttf, FFmpeg, OpenSSL, Opus, dav1d,
libplacebo and Microsoft Detours. To update them, delete `libs\windows` (or run `setup-deps.ps1`
again). `setup-deps.py` does the same for the unsupported macOS and Steam Link builds.

The licenses of these libraries are listed in [NOTICE](../NOTICE), and their texts are shipped in
`licenses\`.

## Tests and source checks

Hermit's own logic has standalone tests that build in tens of seconds without building the app:

```powershell
powershell -ExecutionPolicy Bypass -File hermit\tests\run-tests.ps1
```

- `hermit_core_test`: the translation layer (Hermit's translation file is consulted first), session
  summary maths and the history file (including CSV quoting), performance overlay text, automatic
  bitrate, resolution presets for the display's aspect ratio, and that the settings stored by
  connection profiles exist.
- `clipboard_archive_test`: the clipboard file archive: streamed upload (byte-identical to the
  existing format), throttling, validation of received archives (19 kinds of bad paths, sizes and
  duplicates are rejected), extraction and cancel, and real transfer speed over a loopback HTTP
  server.
- `check-qml-members.py`: QML uses only members that the exposed C++ objects have (a misspelled or
  removed member fails only when that line runs).
- `check-translations.py`: Korean translations keep their `%1`…`%9`/`%n` placeholders, and every
  user-visible source string has a Korean translation.

Objects and executables go to `build\tests`. The script exits with a non-zero code if a test fails.
The two Python checks also run on their own (`python hermit\tests\check-translations.py`) and at the
start of every build.

## Translations

Hermit's interface is in English, with a complete Korean translation.

- `app/languages/hermit_ko.ts` holds the strings that Hermit added and the Korean strings missing
  from Moonlight's translation. It is consulted before the per-language `qml_<lang>.ts` files.
- Do not edit `hermit_ko.ts` by hand. Add the string as a `(context, source text)` pair to
  `TRANSLATIONS` in `hermit/translations/make_hermit_ts.py` and run
  `python hermit/translations/make_hermit_ts.py`, which writes `hermit_ko.ts` and compiles
  `hermit_ko.qm` with `lrelease` (taken from `LRELEASE`, else `HERMIT_QT_DIR\bin`).
- The context is the QML file name or the C++ class name (or the first argument of
  `QCoreApplication::translate`). Rewording an English string requires updating its entry, or the
  translation check fails.

## Icons

The icon is 16×16 pixel art of a hermit crab carrying a spiral shell. The drawing is a character grid
inside `hermit/branding/deploy.py`; run `python hermit/branding/deploy.py` to regenerate
`app/res/hermit.svg` (window icon, drawn by Qt) and `app/hermit.ico` (exe and taskbar).
`pixelart.py` scales the pixels without a renderer (exact at multiples of 16; 20 and 24 px are
stretched).

## Theme

The colors and fonts are defined in `app/gui/hermittheme.*` and used from QML through the
`HermitTheme` singleton: a dark theme based on the IBM Carbon gray scale (background `#161616`,
panels `#262626`, borders `#393939`, text `#F4F4F4`/`#C6C6C6`) with a single teal accent (`#08BDBA`,
filled buttons `#007D79`); green, yellow and red are used only for status. Text uses IBM Plex Sans KR
(tabular figures for numbers) and IBM Plex Sans Condensed for labels and table headers
(`app/res/fonts`, SIL Open Font License).

## Unmaintained parts

- `wix/` contains the WiX installer project inherited from Moonlight. It is not built by
  `build-windows.bat`, not maintained and not tested; Hermit is distributed as a ZIP and a single-file
  exe.
- `scripts/generate-src.sh` and `scripts/git-archive-all.sh` create a source archive that includes the
  submodules. `scripts/update-msvcredist.ps1` updates the Visual C++ redistributable entries of the
  WiX bundle.
- Code for macOS, Linux, Steam Link and other embedded targets is inherited from Moonlight and kept
  for now, but it is not built or tested.
