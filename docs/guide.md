# Hermit user guide

This guide covers everything Hermit adds on top of a standard GameStream client, and where Hermit
keeps its data. For installation and a quick overview, see the [README](../README.md). For building
from source, see [Building Hermit](building.md).

Features marked **(Shell)** need a [Shell](https://github.com/junopark00/hermit-shell) host. Everything
else works with any GameStream host, such as Sunshine or Apollo.

## Contents

- [Getting started](#getting-started)
- [Where Hermit keeps its data](#where-hermit-keeps-its-data)
- [Stream panel](#stream-panel)
- [Live and automatic bitrate (Shell)](#live-and-automatic-bitrate-shell)
- [Clipboard sync (Shell)](#clipboard-sync-shell)
- [Dropping files onto the stream (Shell)](#dropping-files-onto-the-stream-shell)
- [Keyboard shortcuts](#keyboard-shortcuts)
- [Performance overlay](#performance-overlay)
- [Session summary and history](#session-summary-and-history)
- [Connection profiles](#connection-profiles)
- [Automatic reconnect](#automatic-reconnect)
- [Shutting down or restarting a host (Shell)](#shutting-down-or-restarting-a-host-shell)
- [Bitrate entry and recommended values](#bitrate-entry-and-recommended-values)
- [Resolutions for your display's shape](#resolutions-for-your-displays-shape)
- [Other streaming options](#other-streaming-options)
- [Settings layout and language](#settings-layout-and-language)
- [Command line](#command-line)
- [Troubleshooting](#troubleshooting)

## Getting started

1. Start Hermit. Hosts on your local network that announce themselves over mDNS appear
   automatically. To add a host by address, click **Add PC manually** (the + button) and enter its IP address
   or host name.
2. Select the host. Hermit shows a PIN; enter it in the host's web UI (for Shell, the pairing page at
   `https://<host address>:47990`; Sunshine and Apollo have an equivalent PIN page). **Open Shell
   pairing page** in the PIN dialog opens that page in your browser with the PIN and this PC's name
   filled in (the PIN travels in the URL fragment, which the browser does not send to the host). The
   browser warns about the host's self-signed certificate and asks for the web UI password; the
   PIN dialog stays open until pairing completes.
3. Once paired, pick an app or the desktop to start streaming.

**Settings from Moonlight.** On its first run, if Hermit has no settings yet and Moonlight is
installed for the same Windows user, Hermit copies Moonlight's settings once: paired hosts, the
client identity (the certificate and key a host uses to recognize a paired client) and preferences.
Hosts that were paired with Moonlight therefore work without pairing again. Moonlight's settings
are only read, never changed, and the two clients do not sync afterwards. To start from a clean
slate instead, run Hermit for the first time on a Windows user without Moonlight, or delete
Hermit's settings (see below) and Moonlight's before starting Hermit.

**Portable mode.** If a file named `portable.dat` exists in the current directory when Hermit starts,
Hermit keeps its settings in INI files in that directory instead of the registry, and its logs,
box art and caches next to it.

## Where Hermit keeps its data

Hermit uses its own names and locations, so it can be installed next to Moonlight.

| Item | Location |
|---|---|
| Settings, paired hosts, client identity | Registry: `HKCU\Software\Hermit\Hermit` |
| Session history | `%APPDATA%\Hermit\Hermit\session-history.csv` |
| Log of the current run | `%TEMP%\Hermit-<number>.log` |
| Clipboard files received from a host as an archive | `%TEMP%\HermitClipboard` (the last 5 transfers are kept) |
| Box art and other caches | `%LOCALAPPDATA%\Hermit\Hermit\cache` |
| Unpacked single-file exe | `%LOCALAPPDATA%\Hermit\app\<hash>` |

The window title is `<host> - Hermit`, and `Hermit.exe --help` lists the command-line options.

## Stream panel

A side panel over the stream changes options without ending the stream, similar to the stream menu
of other remote desktop clients.

- **Open it** with **Ctrl+Alt+Shift+P**, or click the arrow handle on the edge of the stream window:
  - With the remote desktop mouse ("Optimize mouse for remote desktop instead of games"), the
    pointer moves freely, so the handle is always shown while the stream window is active. The
    pointer is visible over the handle, and hovering or clicking it sends nothing to the host.
  - With the game mouse, the handle is shown only while the mouse is not captured (windowed mode
    before you click into the stream, or after releasing the mouse with Ctrl+Alt+Shift+Z). When a
    stream starts in this mode, a notice under the stream shows both shortcuts for 5 seconds
    (not when reconnecting).

  While the panel is open, the mouse and keyboard go to the panel; closing it (the close button,
  Esc, or the same shortcut) restores the previous capture state. Switching to another app or
  minimizing the stream closes the panel and hides the handle. The handle never takes keyboard
  focus from the stream, also in full screen.
- **Handle position**: choose the right or left edge in the panel; the panel opens on that side.
  Press and drag the handle to move it up or down. Position and side are saved.
- When the panel opens, the bitrate field has keyboard focus. Tabbing through the panel scrolls the
  focused control into view.
- **Applied immediately**: performance overlay (on/off, text size, and the metrics under "Choose
  metrics"), "Mute on this PC", clipboard sync, full screen, and mouse mode. "Mute on this PC"
  is separate from the setting that mutes Hermit in the background, so it stays on when you return to the
  window and across reconnects. The mouse mode button shows the current mode (game or remote
  desktop); changing it with Ctrl+Alt+Shift+M briefly shows the new mode over the stream. Mute, mouse
  mode and full screen as you left them (in the panel or with the shortcuts) carry over to a
  reconnect, whether you apply new settings or Hermit reconnects after a network drop; the next
  stream you start yourself uses the settings again. V-Sync and
  frame pacing also apply immediately, but recreate the renderer, so the picture blinks once. Frame
  pacing has no effect without V-Sync and is shown as off in that case (as in Settings).
- **Bitrate** (top section): on Shell hosts with an NVIDIA encoder, the bitrate changes live as soon
  as you release the slider or enter a value, without reconnecting or freezing the picture. Arrow
  keys apply 0.6 s after the last key press; the text field applies on Enter or when it loses focus,
  and does nothing if the value did not change. On other hosts or encoders, the panel says so and the
  bitrate is applied by reconnecting (see below).
- **Resolution, frame rate, codec (applied by reconnecting)**: resolution (the list includes sizes
  with the aspect ratio of your display, see
  [Resolutions for your display's shape](#resolutions-for-your-displays-shape); sizes that are not in
  the list can be typed as `WIDTHxHEIGHT`, for example `720x1280` for a portrait screen; 320x240 to
  7680x4320), frame rate, video codec, and HDR (when this PC supports HDR). If you never picked a
  bitrate yourself (the bitrate follows the resolution, as in Settings), reconnecting also sets the
  bitrate for the new resolution and frame rate, and the panel shows that value in advance.
  **Apply and reconnect** closes the stream and immediately reconnects with the new settings (about
  2 to 3 seconds); the app keeps running on the host and the stream resumes. In this case the "quit
  app after streaming" setting does not quit the app. **Revert** restores the values the stream
  started with.
- **Virtual display follows the stream (Shell)**: Shell streams from a virtual display created at the
  resolution and refresh rate Hermit asks for. When you reconnect at a new resolution, Shell resizes
  the virtual display to match, so the host desktop fills the new stream without black bars. When a
  monitor on the host is turned on, Shell hands the desktop back to it.
- At the bottom: **Disconnect**, and **Quit app and disconnect** (quits the app on the host and returns
  to the app list, after a confirmation in the panel).
- The panel edits the same values as the Settings page. Options that apply immediately, and the
  bitrate, are saved when changed; options applied by reconnecting are saved by **Apply and
  reconnect**. If you close the panel without reconnecting, a notice says that the changed
  resolution, codec and so on apply from the next connection (**Revert** cancels them).

## Live and automatic bitrate (Shell)

**Automatic bitrate** adjusts the bitrate to the network during the stream. Turn it on with
"Adjust the bitrate automatically to the network (Shell host)" in Settings, or "Adjust automatically to the network" in the stream panel. It is
off by default.

- Every second, Hermit looks at network loss and round-trip time and changes the bitrate live on the
  host. It never reconnects.
  - If more than 2% of frames are lost, or the round-trip time rises well above its usual value (by
    at least 40 ms and 1.5 times), the bitrate drops to 80%, at most once every 2 seconds, down to
    one fifth of the chosen bitrate (at least 2 Mbps).
  - After ten good one-second intervals (and at least 10 seconds since the last change), the bitrate
    rises by one tenth of the chosen bitrate (at least 1 Mbps), up to the chosen bitrate.
  - In intervals with fewer than 20 frames (Shell sends only 10 to 12 frames per second for a still
    picture), loss is not judged; only the round-trip time counts, and the interval counts as neither
    good nor bad. This lets the bitrate recover slowly during document work with frequent pauses.
  - When turned on, it starts from the bitrate the host is currently sending.
- The chosen bitrate is the upper limit. Changing it in the panel sets a new limit and a new starting
  point. The panel shows "Automatic: now N Mbps (up to M Mbps)". While it is adjusting and the
  connection is slow, the warning over the stream says that the bitrate is being lowered
  automatically instead of asking you to lower it.
- Turning it off returns to the chosen bitrate. While it is off, if the host's bitrate differs from
  the chosen one (right after turning it off, or after a request for your own value failed
  temporarily), Hermit sends the chosen value again every 5 seconds.
- It does nothing on hosts that cannot change the bitrate live, or with non-NVIDIA encoders; if the
  first request is refused, the automatic bitrate indicator disappears.
- **Failed requests** (your own value or an automatic one): a timeout may still have reached the host,
  so the same value is sent again, and once again 6 seconds after the connection recovers. While
  requests fail, the panel shows a warning and enables **Apply and reconnect**; the notice over the
  stream includes the panel shortcut (Ctrl+Alt+Shift+P). If a value you picked yourself is not
  confirmed and fails after you close the panel, a notice appears (once for timeouts and once for
  refusals). Failures of automatic adjustments are shown only in the panel, except when the host
  refuses live changes while its bitrate differs from the chosen one, which needs a reconnect.
- With YUV 4:4:4 enabled on a host that does not support it, the stream starts at a lower default;
  as long as you do not change the bitrate setting, that starting value counts as the chosen value.
- Dragging the slider pauses the adjustment; the value where you release it becomes the new limit.
  Automatic values are never saved. **Revert** in the panel leaves the bitrate alone when the host can
  change it live; otherwise it restores the last value you picked that the host actually applied
  (including the "follows the resolution" state, if you had not picked a value yet).
- If a request sent just before a reconnect could have changed the new stream (it was still in
  flight, or the previous session ended with a failed request), Hermit sends the current value again
  after that request finishes.
- The rules live in `app/streaming/autobitrate.h`; Hermit for Android uses the same rules.

When the bitrate changed during a stream (in the panel or automatically), the session summary and
history report a time-weighted average, for example "Automatic · average 14 Mbps (up to 30)".

## Clipboard sync (Shell)

Shell offers clipboard access to paired clients during a stream (`/actions/clipboard`). Hermit uses it
to sync the clipboard in both directions.

| Type | Hosts | Local to host | Host to local | Limit |
|---|---|---|---|---|
| Text | Shell, and hosts with the text clipboard extension | At stream start, when you return to the stream window, and immediately on copy | When you leave the stream window | 1 MB |
| Images | Shell | At stream start, when you return to the stream window, and immediately on copy | When you leave the stream window | PNG, 32 MB, 8192×8192 |
| Files and folders | Shell | At stream start and when you return to the stream window | When you leave the stream window (the list; each file downloads while you paste it) | To the host 256 MB, from the host 4 GB; 1,000 items |

- When a clipboard holds both text and an image (for example, copied spreadsheet cells), the text is
  sent. The priority is text, then image, then files.
- Files can be large, so they are not sent on every copy, only at stream start and when you return to
  the stream window. The host pastes the copy sent at that moment: after editing a file, copy it again
  and click the stream window once to send the new content.
- **Pasting files copied on the host**: when you leave the stream window, Hermit fetches only the
  list of copied files and folders (names, sizes, times) and puts it on the clipboard at once, like
  Remote Desktop. When you paste in File Explorer, Explorer shows its own copy progress and each file
  is downloaded from the host while Explorer copies it, so pasting starts right away and nothing is
  downloaded unless you paste.
  - The speed limit applies. Explorer's Cancel stops the download; Ctrl+Alt+Shift+T in the stream
    window does too, and Explorer then reports the file as cancelled.
  - Up to 4 GB and 1,000 items. Files can be pasted as often as you like while the stream runs; each
    paste downloads them again. When the stream ends (or clipboard sync is turned off), Hermit removes
    them from the clipboard, and a paste still in progress fails.
  - A file that was changed or deleted on the host after you copied it fails to paste (File Explorer
    shows an error); copy it again on the host.
  - A download that stops (network loss, or the host's 5-minute limit per request) continues from
    where it stopped; it fails after 60 seconds without data.
  - This needs a current Shell host. With older Shell versions, and for folders with paths over 259
    characters, the files come over as one archive when you leave the stream window (256 MB limit) and
    are pasted from a temporary folder, as described next.
- Files received as an archive are unpacked to a temporary folder (`%TEMP%\HermitClipboard` on the
  client; Shell uses `%LOCALAPPDATA%\Temp\ShellClipboard` of the user) and placed on the clipboard as
  "copied", so they can be pasted in File Explorer. The last 5 transfers are kept; older ones are
  deleted.
- A received file list is written only after the paths, duplicates and sizes have all been checked.
  Symbolic links and junctions are not followed.
- Both sides track the clipboard sequence number, so unchanged content is not sent again and a side
  never receives back what it wrote itself.
- Network transfer, image conversion and file I/O run on separate threads and never stall the
  stream. Clipboard contents are never logged.
- **Speed limit**: image and file transfers are rate-limited in both directions, leaving bandwidth for
  the video (host upload) and the input (client upload). Choose 10, 20, 30, 50 or 100 Mbps or no limit
  in Settings → Streaming conveniences → "Image and file transfer speed limit" (default 30 Mbps,
  applies from the next stream). For example, 256 MB takes about 70 seconds at 30 Mbps and about
  3.5 minutes at 10 Mbps.
  - Uploads stream the files from disk into the archive as it is sent; downloads are slowed with TCP
    flow control and written to a temporary file (`%TEMP%\HermitClipboard\download-*.apcf`), which is
    checked as a whole before it is unpacked. An archive is never held in memory in full.
  - An upload fails if a file changes size while it is being sent. A transfer with no data in either
    direction for 60 seconds fails.
  - The host protocol is unchanged: one HTTP request carries the same archive format.
- Transfers that take longer than a second show their progress under the stream every 0.5 seconds,
  for example "Sending to the host: 45% (12.0 / 26.0 MB)". Pasting files copied on the host shows File
  Explorer's progress instead.
- **Ctrl+Alt+Shift+T** cancels a transfer in progress and deletes partly received data. A cancelled
  transfer is not retried automatically; copy again if you still need it.
- Other clipboard changes during an image or file transfer (text, for example) are handled in order
  after the transfer finishes.
- On hosts with only the text clipboard extension, only text is synced (detected automatically).
- When an image or files have been sent to the host, or received and ready to paste, a notice appears
  under the stream for 3.5 seconds (for example, "2 items sent to the host (453 KB)"). Text is copied
  too often to be announced. Notices never cover a connection quality warning; only the shortcut list
  (Ctrl+Alt+Shift+H) appears above the warning, which returns when the list closes.
- Content that does not move also gets a short notice: an image over 32 MB or 8192×8192 pixels or in
  a format that can't be converted, text over 1 MB, host files over the limit, host files with names
  the host can't copy ("unsupported or duplicate names"), or a transfer that failed. Host content is
  fetched when you leave the stream window, where the notice is easy to miss, so a notice about host
  content (or a missing permission) is shown once more when you return to the stream window.
- Turn it off with Settings → Streaming conveniences → "Sync clipboard with the host" (on by
  default).
- **Permissions**: the host grants each direction separately in its device permissions. Clipboard
  Read lets this device fetch the host's clipboard; Clipboard Set lets it send to the host's
  clipboard. Files also need File Download (from the host) or File Upload (to the host). A newly
  paired device has none of them. When one is missing, only that direction stops: for example, with
  Clipboard Set alone, what you copy locally still reaches the host. A notice over the stream names
  the missing permission (once, and once more when you return to the stream window); turn it on in
  the host's device permissions and start a new stream.
- Image and file sync is available in the Windows client only.

## Dropping files onto the stream (Shell)

- Drag files or folders from File Explorer onto the stream window to put them on the **host's
  clipboard**, then paste them with Ctrl+V in the folder you want on the host.
- It uses the same path as clipboard sync (Shell host, clipboard and file upload permissions for
  this device), with the same 256 MB limit, speed limit and cancel shortcut (Ctrl+Alt+Shift+T).
- The result is shown over the stream: the number of items and their size, or that clipboard sync is
  off, permission is missing, or the limit was exceeded.
- This works best in windowed mode. In full screen, minimize with Ctrl+Alt+Shift+D or switch to a
  window with Ctrl+Alt+Shift+X to reach File Explorer.

## Keyboard shortcuts

Press **Ctrl+Alt+Shift+H** during a stream to show the list of shortcuts over the stream for
15 seconds; press it again to close it. The stream panel's "Keyboard shortcuts" button opens it too. The screen
shown while a stream starts mentions P (stream settings), H (shortcut list) and Q (disconnect).

All shortcuts are pressed together with **Ctrl+Alt+Shift**:

| Key | Action |
|---|---|
| P | Stream panel |
| S | Performance overlay |
| M | Mouse mode (game / remote desktop) |
| X | Full screen |
| D | Minimize |
| Z | Release mouse and keyboard |
| L | Confine the mouse to the window |
| K | Send system keys (Win, Alt+Tab) to the host |
| C | Show or hide the cursor (in remote desktop mouse mode) |
| V | Type the clipboard text |
| T | Cancel a file transfer |
| Q | Disconnect |
| E | Quit the app on the host and close Hermit |
| H | Shortcut list |

Shortcuts whose effect is not visible show a short notice over the stream: L and K (on or off), Z
(released; click the stream to capture again), C (when the game mouse mode cannot show a cursor),
V (when the clipboard holds no text), and T (when there is no transfer to cancel).

The gamepad combination Start+Select+L1+R1 disconnects, and Select+L1+R1+X toggles the performance
overlay.

## Performance overlay

- **Ctrl+Alt+Shift+S** shows streaming statistics in a translucent dark table of labels and values.
  The on/off state is saved, so the panel and the Settings page always agree.
- In Settings → Performance overlay, choose whether it is on, which metrics it shows and the text size
  (small, medium, large). **Restore defaults** returns to the default metrics.
- Default metrics: video (resolution, FPS, codec), bitrate (average and peak), network loss, round-trip
  time, host latency (average, minimum to maximum) and estimated total latency.
- Optional metrics: received / decoded / rendered FPS, jitter drops, decode time, queue delay, render
  time and decoder.
- Estimated total latency = host latency + half the round-trip time + decode + queue delay + render.
  It is an estimate and can differ from the real input-to-display latency (monitor response time and
  so on are not included).
- Values cover roughly the last 2 seconds and refresh every second. During a stream, the stream panel
  changes the same options.

## Session summary and history

- When you return to the window after streaming for at least 30 seconds, a summary appears: average
  FPS, network loss, host latency, round-trip time, queue delay, decode and render times, in a table
  with this session, the average of the last 10 sessions, and a status (good, fair, poor) with a small
  colored square.
- Average FPS is shown for reference only and not rated: a host sends frames only when the picture
  changes, so a mostly still picture gives a lower average than the target. Stutter is judged by
  network loss and jitter drops.
- If frame pacing increased the queue delay, or loss was high, the summary adds a line or two of
  advice. When the round-trip time is unknown, "–" is shown.
- Every session is appended as one line to `%APPDATA%\Hermit\Hermit\session-history.csv` (opens directly
  in a spreadsheet). Open the folder with "Open history" in the summary, or Settings → Streaming
  conveniences → "Open session history folder".
- Turn the summary off with Settings → Streaming conveniences → "Show a summary after each session"
  (history is still recorded).
- V-Sync and frame pacing are recorded as they were when the stream ended. Statistics cover the whole
  session, even if a window resize replaced the decoder.

## Connection profiles

- Use the profiles button in the toolbar (PC and app views) to save the current settings as a profile,
  apply a profile, or delete one.
- A profile stores resolution, frame rate, bitrate, window mode, V-Sync, frame pacing, codec, HDR,
  YUV 4:4:4, audio, performance overlay and full-size packets. Input, gamepad, language and the other
  settings are shared by all profiles.
- The save dialog's Save button stays disabled until a name is entered, and deleting asks for
  confirmation. The profile that matches the current settings has a check mark. For example, save
  "1440p windowed" and "Full screen" and pick one before connecting.

## Automatic reconnect

If the network drops during a stream (the connection ends or video stops arriving), Hermit returns to
its window and shows "The connection was lost. Reconnecting in 5 s.", then connects to the same app on
the same host. **Connect now** connects at once; **Cancel** (Esc, gamepad B) stops.

- It is not used when you end the stream yourself (Ctrl+Alt+Shift+Q or the gamepad combination), when
  the app on the host exits normally, for failures before the stream started, for encoder or
  protected-content errors on the host, or when no video ever arrived (a firewall or port problem;
  the error is shown immediately).
- Up to 3 attempts in a row. If the reconnected stream lasts more than a minute, the count starts
  over. After the third attempt, the original error is shown.
- If an attempt fails (for example, the host does not respond), Hermit waits again within the
  remaining attempts. If the host refuses to launch the app (permissions, pairing) or another app is
  running, it stops and shows the error.
- If the host is known to be offline, Hermit shows "The host is offline" and skips the attempt when
  the countdown ends. Host polling pauses during a stream, so for a few seconds after the drop the
  host may not yet appear offline.
- When reconnecting, Hermit asks the host which app is running: the same app is resumed, no app means
  a fresh launch, and a different app means no connection.
- No session summary is shown while a reconnect is offered (history is still recorded). Command-line
  streams (`Hermit.exe stream`) do not reconnect.
- Turn it off with Settings → Streaming conveniences → "Reconnect automatically when the connection
  drops" (on by default).

## Shutting down or restarting a host (Shell)

- In the PC view, right-click a host (or press the menu key) and choose **Shut down PC…** or **Restart
  PC…**. Apps running on the host are asked to close first.
- The device needs the **Launch apps** permission in Shell's pairing page; Hermit says so if it is
  missing, and asks you to update Shell if the host does not support remote shutdown.
- Before shutting down or restarting, Hermit checks for other devices streaming from the host and, if
  there are any, warns that their streams will end ("Shut down anyway", "Restart anyway").
- "Also close apps with unsaved work (it is lost)" forces apps that refuse to close to quit (their work is lost).
  It is off by default.

## Bitrate entry and recommended values

- The field next to the bitrate slider in Settings accepts a value in Mbps (0.1 Mbps steps); the slider
  and the field follow each other.
- Below them, Hermit shows the **recommended bitrate** for the selected resolution and frame rate:
  - pixels × FPS × 0.15 bit for H.264; HEVC and AV1 use 70% of that, YUV 4:4:4 1.5 times. The part
    above 60 FPS grows with its square root only.
  - Examples: 2560x1440 at 60 FPS → H.264 33 Mbps, HEVC/AV1 23 Mbps. 1920x1080 at 60 FPS → 19 / 13
    Mbps.
  - Until you change the bitrate yourself, the value that follows resolution and FPS changes is this
    formula's H.264 value. Moving the slider or typing a different value stops that; leaving the
    field without a new value does not.
  - **Use recommended (N Mbps)**, under the recommendation in Settings and in the stream panel, sets
    that value and lets the bitrate follow the resolution and frame rate again.
- The slow connection warning ("above 5 Mbps") is judged against the bitrate currently applied on the
  host, not the one the stream started with.

## Resolutions for your display's shape

The standard resolution presets (720p, 1080p, 1440p, 4K) are 16:9. On a display with another shape,
such as a 21:9 ultrawide, a 16:10 laptop or a 3:2 tablet, Hermit also offers presets with the aspect
ratio of that display, labeled "(display aspect)", in Settings and in the stream panel.

- They use the display Hermit's window is on, or the primary display if that is unknown. In the
  stream panel they use the display the stream is on, at its desktop resolution, so a full screen
  mode of another shape does not change them.
- For the heights 720, 1080, 1440 and, when 4K is offered, 2160, the width is the height times the
  display's aspect ratio. A width that comes out as an even whole number is used as is; otherwise
  it is rounded to a multiple of 8 (or to an even number, if a multiple of 8 would change the
  aspect by more than 1%). Portrait displays use these values for the width instead.
- Sizes that are already in the list (the 16:9 presets or the native resolution), or within 1% of
  the width of an entry with the same height, are not repeated, so a 16:9 display (or one close to
  it, such as 1360x768) shows nothing new. Sizes larger than the video decoder supports are left
  out, as with the other presets.

| Display | Presets added |
|---|---|
| 3440x1440 (21:9) | 1720x720, 2580x1080, 5160x2160, and 3440x1440 in the stream panel (Settings lists it as the native resolution) |
| 1920x1200 (16:10) | 1152x720, 1728x1080, 2304x1440, 3456x2160 |
| 2256x1504 (3:2) | 1080x720, 1620x1080, 2160x1440, 3240x2160 |
| 1920x1080 (16:9) | none |

The chosen size is saved like any other resolution and selected again the next time Settings
opens. If Hermit later runs on a display with another shape, the saved size stays in effect and is
shown as a custom resolution.

## Other streaming options

- **Sharper text in a window (D3D11)**: when the window size differs from the stream resolution, the
  picture is scaled with a Catmull-Rom filter, which keeps text edges crisper than the default bilinear
  filter. At the native size (as in full screen), the regular path is used.
- **Full-size video packets over the Internet**: Settings → Advanced → "Use full-size video packets over
  the internet (1392 bytes)" (off by default). Remote connections then use 1392-byte packets instead of
  1024, about 26% fewer packets per frame. If streams break up or do not start after turning it on, the
  network path cannot carry the larger packets; turn it off again.

## Settings layout and language

- Each group header in Settings is a button; the arrow on the right shows whether the group is open.
  Keyboard and gamepad can focus a header and open it with Enter or Space, and the state is
  remembered. At first only the most used groups are open (basic settings, host settings and
  streaming conveniences); audio, UI, input, gamepad and advanced settings are collapsed.
- The interface is available in English and Korean; by default Hermit follows the Windows display
  language, and the language can be chosen in Settings. Connection stage names, connection quality
  warnings and the gamepad mouse mode notice are translated as well. Translations of other languages
  inherited from Moonlight cover only the parts of the interface that come from Moonlight.
- The Help button in the toolbar (and F1) opens this guide.

## Command line

`Hermit.exe --help` lists the options. The main actions are:

```text
Hermit.exe stream <host> <app>   Start streaming an app
Hermit.exe list <host>           List the apps of a host
Hermit.exe pair <host>           Pair with a host
Hermit.exe quit <host>           Quit the running app on a host
```

`<host>` is a host name, UUID or IP address. Each action accepts `--help` for its own options.

## Troubleshooting

- **A host does not appear automatically.** mDNS discovery works only on the local network and can be
  blocked by firewalls or by "client isolation" on Wi-Fi access points. Add the host by its IP address
  instead.
- **"Unable to connect to the specified PC."** Check that the host software is running, that the
  address is correct, and that the host's firewall allows the GameStream ports (TCP 47984, 47989,
  48010 and UDP 47998 to 48000, 48002 and 48010 by default). Over the Internet, the host's router must forward
  these ports, or both sides must be on the same VPN.
- **"No video received from host."** The connection was set up but video packets never arrived:
  usually a firewall or port forwarding problem for the UDP ports listed in the message.
- **"Starting ... failed" or the connection ends during a stream.** Check that the host is running
  and reachable and that the ports above are open; a message that lists ports names the ones that
  failed. Errors when a stream starts or ends have a **Help** button that opens this section. When
  the network drops during a stream, Hermit reconnects by itself (see
  [Automatic reconnect](#automatic-reconnect)).
- **The stream stutters.** Open the performance overlay (Ctrl+Alt+Shift+S): network loss and jitter
  drops point to the network; lower the bitrate or turn on automatic bitrate on Shell hosts. Long queue
  delays point to frame pacing; try turning it off.
- **Pairing fails.** Make sure the PIN was entered on the right host and that the host's pairing page
  was open before the PIN dialog timed out. **Open Shell pairing page** uses the address Hermit
  reached the host at and the host's web UI port (HTTP port + 1, normally 47990); if the host's web
  UI listens elsewhere, open it yourself and enter the PIN.
- **Logs.** Each run writes `%TEMP%\Hermit-<number>.log`. Attach the log of the affected run when
  reporting a problem (it contains host names and addresses, but no clipboard contents).
