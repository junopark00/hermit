# Hermit

Hermit is a game-streaming client for Windows. It streams games, apps or the whole desktop from
another PC over the GameStream protocol, and adds the tools you want for everyday remote use: a
settings panel over the stream, live and automatic bitrate, clipboard sync with images and files,
remote shutdown of the host, and a performance overlay and session summary that tell you how well
the stream is doing.

Hermit pairs best with [Shell](https://github.com/junopark00/hermit-shell), its companion host for
Windows, and also works with other GameStream hosts such as Sunshine and Apollo. It is a fork of
[Moonlight-qt](https://github.com/moonlight-stream/moonlight-qt). An Android client,
[Hermit for Android](https://github.com/junopark00/hermit-android), is available as well.

[한국어 README](README.ko.md)

![The computers list](docs/images/hermit-pc-list.png)

## Highlights

- **Stream panel.** Press Ctrl+Alt+Shift+P during a stream to change settings without leaving it:
  performance overlay, V-Sync, frame pacing, mouse mode, full screen, audio and clipboard sync apply
  immediately; resolution, frame rate, codec and HDR apply with a quick reconnect that resumes the
  running app.
- **Live bitrate.** On Shell hosts the bitrate changes instantly, without reconnecting or freezing
  the picture.
- **Automatic bitrate.** On Shell hosts, Hermit can lower the bitrate when the network struggles and
  raise it again when it recovers, up to the bitrate you chose.
- **Clipboard sync.** Copy and paste text between the two PCs; with Shell, images and files as well,
  with a speed limit so transfers do not hurt the stream. Files copied on the host paste at once in File
  Explorer and download while Explorer copies them (up to 4 GB; 256 MB to the host). You can also drop
  files onto the stream window to put them on the host's clipboard. Each direction needs its
  permission in the host's device permissions (Clipboard Read, Clipboard Set, and File Download or
  File Upload for files), unless the host granted them at pairing (Shell offers presets on its pairing
  page).
- **Virtual display that follows the stream.** Shell creates its virtual display at the resolution
  you stream at, resizes it when you reconnect at a new resolution, and hands the desktop back to the
  monitors when one is turned on.
- **Remote shutdown and restart.** Turn a Shell host off or restart it from the PC list, with a warning
  when other devices are still streaming from it.
- **Performance overlay.** Choose the metrics you care about: bitrate, network loss, round-trip time,
  host latency, decode and render times, and an estimated total latency.
- **Session summary.** After each session, see how it went compared with your last ten, with every
  session recorded in a CSV history.
- **Presets for your display's shape.** On 21:9, 16:10, 3:2 and other non-16:9 displays, the
  resolution lists offer sizes with the display's aspect ratio, so you do not have to type them.
- **Connection profiles.** Save sets of stream settings (for example "1440p windowed" and "Full screen")
  and switch between them in one click.
- **Automatic reconnect.** If the network drops, Hermit reconnects to the same app by itself.
- **Sharper text in a window.** A higher-quality scaler keeps text readable when the stream is shown
  at a different size than it is sent.
- **English and Korean.** The interface is fully translated into Korean and follows the Windows
  display language by default.
- **Portable.** Download a single exe, or a ZIP of the program folder. No installer, no administrator
  rights, and the Visual C++ runtime is included.

And everything you expect from a GameStream client: hardware-accelerated H.264, HEVC and AV1
decoding, HDR and YUV 4:4:4, up to 7.1 surround sound, gamepads with rumble and motion sensors, mouse
capture for games and direct mouse control for desktop work, and passing shortcuts such as Alt+Tab to
the host.

![The app list of a host](docs/images/hermit-apps.png)

## Compatibility

| Feature | Shell | Other GameStream hosts (Sunshine, Apollo, …) |
|---|---|---|
| Streaming, pairing, gamepads, HDR, 4:4:4 | Yes | Yes |
| Stream panel | Yes | Yes (bitrate changes reconnect) |
| Live bitrate change, automatic bitrate | Yes (NVIDIA encoder) | No |
| Clipboard text | Yes | Hosts with the text clipboard extension |
| Clipboard images and files, file drop | Yes | No |
| Remote shutdown and restart | Yes | No |
| Virtual display resized on reconnect | Yes | Depends on the host |

## Requirements

- Windows 10 or Windows 11, 64-bit (x64)
- A GPU with hardware video decoding is recommended (H.264, HEVC or AV1)
- A GameStream host on another PC: [Shell](https://github.com/junopark00/hermit-shell) for the full
  feature set, or another host such as Sunshine or Apollo

## Installation

Download the latest release from the [Releases](https://github.com/junopark00/hermit/releases) page:

- **`Hermit-<version>.exe`**: a single file. On first start it unpacks itself once to
  `%LOCALAPPDATA%\Hermit\app` and then starts in a moment.
- **`Hermit-x64-<version>.zip`**: the program folder. Extract it anywhere and run `Hermit.exe`.

Nothing needs to be installed, and both include the Visual C++ runtime. To uninstall, delete the
file or folder and `%LOCALAPPDATA%\Hermit` (unpacked program files and caches). To remove your
settings, pairings and session history as well, delete the registry key `HKCU\Software\Hermit` and
the folder `%APPDATA%\Hermit`.

## First connection and pairing

1. Start Hermit. Hosts on your local network appear automatically; to add one by address, click the
   **+** button and enter its IP address or host name.
2. Select the host. Hermit shows a four-digit PIN.
3. Enter the PIN in the host's web UI (for Shell, the pairing page at `https://<host address>:47990`).
   **Open Shell pairing page** in the PIN dialog opens it in your browser with the PIN and this PC's
   name already filled in; accept the self-signed certificate warning and sign in with the web UI
   password.
4. Pick an app or the desktop and start streaming. Press **Ctrl+Alt+Shift+H** during a stream to see
   all keyboard shortcuts, and **Ctrl+Alt+Shift+Q** to disconnect.

### Coming from Moonlight

Hermit keeps its own settings, so it can be used next to Moonlight. On its **first run**, if Hermit has
no settings yet and Moonlight is installed for the same Windows user, Hermit **copies Moonlight's
settings once**: your paired hosts, Moonlight's client identity (the certificate a host uses to
recognize a paired client) and your preferences. Hosts paired with Moonlight therefore work in Hermit
without pairing again. Moonlight's settings are only read, never changed, and the two clients are not
kept in sync afterwards. See the [user guide](docs/guide.md#getting-started) for details.

![The settings page](docs/images/hermit-settings.png)

## Documentation

- [User guide](docs/guide.md): every feature in detail, keyboard shortcuts, where Hermit keeps its
  data, troubleshooting
- [Building Hermit](docs/building.md): build from source, tests, translations
- [Contributing](CONTRIBUTING.md) and [security policy](SECURITY.md)

## Building from source

Hermit builds with Visual Studio 2022 or 2026 (or the Build Tools) and Qt 6 for MSVC x64:

```powershell
git clone --recurse-submodules https://github.com/junopark00/hermit.git
cd hermit
python hermit\install_qt_msvc.py      # Qt 6.11.3 to C:\Qt (or use your own Qt, see below)
hermit\build-windows.bat
```

To use Qt from another location, set `HERMIT_QT_DIR` to the Qt kit folder (for example
`D:\Qt\6.11.3\msvc2022_64`) or pass its `bin` folder to the build script. The first build downloads
prebuilt libraries (FFmpeg, SDL, OpenSSL and others) from Moonlight's public dependency repository.
See [Building Hermit](docs/building.md) for the full instructions.

## Privacy and network use

Hermit has no telemetry, no analytics, no accounts and no update checks. Its network traffic is
limited to:

- **The hosts you add or that it discovers** on your local network (mDNS), to list apps, pair and
  stream.
- **Wake-on-LAN packets**, sent to the host's addresses and your local network when you choose
  **Wake PC**.
- **A public STUN server** (`stun.cloudflare.com`, UDP port 3478), to learn the external IPv4 address
  of your network so a host on your LAN can also be reached from outside. Hermit sends this request
  whenever it finds a host on the local network through mDNS (usually at every start, while
  automatic PC discovery is on), and when you manually add a new host with a private IPv4 address
  (10.x, 172.16–31.x, 192.168.x) that is not reached through a VPN. Only a standard STUN binding
  request is sent; it contains no information about you or your hosts.
- **github.com**, only when you open the user guide with the Help button or F1 (in your web
  browser).

Nothing else is contacted while you use Hermit. Building from source also downloads the prebuilt
dependencies from GitHub. Logs (`%TEMP%\Hermit-<number>.log`) stay on your PC; clipboard contents are
never logged.

## License and credits

Hermit is free software, licensed under the [GNU General Public License, version 3 or (at your
option) any later version](LICENSE) (GPL-3.0-or-later).

Hermit is based on [Moonlight-qt](https://github.com/moonlight-stream/moonlight-qt) by Cameron Gutman
and the Moonlight contributors, and includes
[moonlight-common-c](https://github.com/moonlight-stream/moonlight-common-c),
[qmdnsengine](https://github.com/cgutman/qmdnsengine),
[SDL_GameControllerDB](https://github.com/gabomdq/SDL_GameControllerDB) and other open-source
components. See [NOTICE](NOTICE) for the full list of components, their authors and licenses.

## Disclaimer

Hermit is an independent project. It is not affiliated with, endorsed by or sponsored by NVIDIA
Corporation or the Moonlight project. NVIDIA and GameStream are trademarks of NVIDIA Corporation. All
other trademarks are the property of their respective owners.
