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
- [Local cursor (Shell)](#local-cursor-shell)
- [Keyboard shortcuts](#keyboard-shortcuts)
- [Performance overlay](#performance-overlay)
- [Network diagnostics](#network-diagnostics)
- [Session summary and history](#session-summary-and-history)
- [Connection profiles](#connection-profiles)
- [Automatic reconnect](#automatic-reconnect)
- [Shutting down, restarting or sleeping a host (Shell)](#shutting-down-restarting-or-sleeping-a-host-shell)
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
   PIN dialog stays open until pairing completes. Hermit waits about 5 minutes for the PIN with
   every host; after that it says no PIN was entered (Shell also drops the attempt at 5 minutes, so
   the PIN is no longer accepted there). **Cancel** stops the attempt
   and asks the host to drop it; selecting the host again starts a new one with a new PIN. NVIDIA
   GameStream hosts are not asked to drop it on Cancel or when no PIN was entered: there Hermit uses
   the client ID that Moonlight clients share, so that request could unpair all of them.
3. Once paired, pick an app or the desktop to start streaming.

**Pair with a QR link (Shell 1.3.0 or later).** On the host's pairing page (`https://<host
address>:47990`, **Pair with QR**), an administrator creates a QR code with the device permissions
already chosen; next to it is the same link as text (`hermit://pair?...`). Copy that link, click
**Add PC manually** on this PC and paste it into the address field: Hermit adds the host and pairs
with it in one go, without a PIN or a second visit to the web UI. The link carries a one-time secret
and the host's certificate fingerprint, so Hermit also checks that the host it reached is the one the
QR came from. A link is valid for **120 seconds** and for one pairing; the host accepts it from the
local network or Tailscale only. The errors: *already been used* and *expired* mean create a new QR on
the host; *local network only* means this PC reached the host over the Internet; *no QR pairing is
waiting* means the host has no open QR (create it again); *does not match the QR code* means the
address answered with a different certificate than the QR named (another device may be impersonating
the host, nothing was paired); *Shell must be updated* means the host is older than 1.3.0 (pair with a
PIN instead, the host stays in the list). Copy the link only to paste it on this PC: Windows clipboard
history and cloud clipboard sync can carry it elsewhere, and Hermit never reads the clipboard by
itself.

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
| Network diagnostics (when turned on) | `%TEMP%\Hermit-netdiag-<unix time>.csv` (the last 5 files are kept) |
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

**Automatic bitrate** (network adaptation) adjusts the stream to the network while it runs. Turn it
on with "Adjust the bitrate automatically to the network (Shell host)" in Settings, or "Adjust
automatically to the network" in the stream panel. It is off by default.

- Every 250 ms, Hermit looks at the frames that arrived, the frame numbers the network skipped and
  the round-trip time, and tells two kinds of trouble apart. It never reconnects.
  - **Congestion**: the round-trip time rises well above its usual value (by more than 40 ms and 1.5
    times), with or without loss. The bitrate is cut once at once, as a precaution. Further cuts
    need a delivery symptom in the last two windows (a dropped frame, or a gap between arrivals of
    2.5 frame intervals): then the cuts go on every 0.5 s down to the policy's floor (below).
    Without a symptom, one more cut comes only when the round-trip time is still rising (10 ms
    above the previous cut's), 1.5 s later at the earliest, and that is all. The second recording
    showed round trips of 50 to 79 ms for 4 to 6 s with every frame delivered on time, which did
    not react to cuts at all.
  - **Link loss**: 5% or more of the frames are lost, at least two in a window (a single dropped
    frame is a blip), while the round-trip time stays flat (within 20 ms of usual). Lowering the
    bitrate does not help here (the first recording showed the same
    loss at 35 and at 7 Mbps), so Hermit first raises the host's forward error correction (FEC) from
    the host's default (usually 20%) to 35% and then 50%. 50% parity puts 1.5 times the bytes on the
    wire, so when loss goes on for 2 more seconds at 50% the bitrate is cut by 15%, at most twice
    (about 0.72 times, which brings the wire rate back near the original); further cuts come only
    from congestion. FEC changes need Shell 1.4 (serverinfo `LiveFec`); on older hosts the bitrate
    rule applies alone.
  - A decision needs two windows of the same kind in a row (0.5 s), and there is at most one action
    per 0.5 s. One window of the other kind does not switch the regime until it repeats.
  - The usual round-trip time is the minimum seen; it rises (0.5 ms per second) only in clean
    windows, so a congestion that lasts is never mistaken for the new normal.
  - Windows with fewer than 3 frames (a still picture: Shell sends only 10 to 12 frames per second
    for one) decide nothing and count as neither good nor bad, so desktop work with pauses recovers.
  - **Recovery**: after 3 s of clean windows (less than 1% loss, usual round trip) the bitrate rises
    by 15% of the chosen bitrate, every 3 s, up to the chosen bitrate; the first rise after a cut
    waits 6 s after that cut. FEC goes down one step after 30 s of clean windows, and again 30 s
    later at the earliest. The third recording showed 4% loss that FEC 35% hid completely while 20%
    turned it into dropped frames within 0.5 s, so a quicker step down only found the loss again.
    When loss raises FEC again within 10 s of a step down, the next step down waits twice as long
    (60 s, then 120 s at most); a step down that holds for 10 s brings the wait back to 30 s.
    A grey window (1 to 5% loss, or a round trip 16 to 40 ms above usual) counts as half a clean
    window, and 24 grey or clean windows in a row end a trouble regime, so a link with a steady 2%
    loss still recovers, at half speed.
  - When turned on, it starts from the bitrate the host is currently sending and asks the host for
    its default FEC once (`percent=default`); that reply is the ladder's base, and Hermit never sends
    a value below it.
- **When the network is unstable** (the setting under the checkbox, also in the panel) chooses what
  to protect:
  - **Keep the frame rate** (game): congestion cuts the bitrate by 30% per step, down to one eighth
    of the chosen bitrate (at least 2 Mbps). The picture gets blurry for a moment but keeps moving.
  - **Keep the quality** (remote desktop): congestion cuts by 20% per step, down to one third of the
    chosen bitrate (at least 4 Mbps), and the link-loss rule stops there too, keeping FEC at 50%.
    Sharp text and UI come first; a further step that lowers the frame rate instead is planned.
    Floors are rounded down to 0.5 Mbps. Switching to this policy while the bitrate is below its
    floor raises it to the floor at the next clean window (one request).
  - **Auto** (default) follows the mouse mode: the game (relative) mouse keeps the frame rate, the
    remote desktop (absolute) mouse keeps the quality. Switching the mouse mode switches the policy
    at once.
- The chosen bitrate is the upper limit. Changing it in the panel sets a new limit and a new starting
  point. The panel shows "Automatic: now N Mbps, FEC F% (up to M Mbps)" with the host's effective
  FEC ("FEC ?" until the host has answered; no FEC part on hosts that cannot change it); the
  performance overlay's "Automatic" line adds the regime word ("congested" or "loss") while the link
  is not clean. While it is adjusting and the connection is slow, the warning over the stream says
  that the bitrate is being lowered automatically instead of asking you to lower it.
- FEC requests work like bitrate requests: one at a time, the newest value follows, a failed one is
  sent again after 5 s, and a host that refuses them (404) is not asked again this session. Turning
  the setting off sends the host's default FEC back when it had been raised.
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
- The rules live in `app/streaming/networkadapt.h` (replayed over a real recording in
  `hermit/tests/hermit_core_test.cpp`); Hermit for Android still uses the earlier one-second rules.
- With network diagnostics recording, every decision goes to the event column ("adapt: congested ->
  24500 kbps", "adapt: loss -> fec 35", "adapt: recover -> 30000 kbps").

When the bitrate changed during a stream (in the panel or automatically), the session summary and
history report a time-weighted average, for example "Automatic · average 14 Mbps (up to 30)".

## Clipboard sync (Shell)

Shell offers clipboard access to paired clients during a stream (`/actions/clipboard`). Hermit uses it
to sync the clipboard in both directions.

| Type | Hosts | Local to host | Host to local | Limit |
|---|---|---|---|---|
| Text | Shell, and hosts with the text clipboard extension | At stream start, when you return to the stream window, and immediately on copy (also while the stream window has focus, for example a screenshot tool's hotkey) | Within about a second of the copy on the host (every 2 seconds with text-only hosts) | 1 MB |
| Images | Shell | At stream start, when you return to the stream window, and immediately on copy | Within about a second of the copy on the host | PNG, 32 MB, 8192×8192 |
| Files and folders | Shell | At stream start and when you return to the stream window | Within about a second of the copy on the host (the list; each file downloads while you paste it) | To the host 256 MB, from the host 4 GB; 1,000 items |

- **What you copy on the host arrives while you stream.** Hermit asks the host every second
  whether its clipboard changed (a small request; with hosts that only have the text clipboard
  extension, it fetches the text every 2 seconds) and brings a change over at once, whether or not
  the stream window has the focus: text, images (up to the 32 MB limit) and the list of copied files
  follow the same rule. You do not need to leave the stream window; leaving it checks right away
  as well, so what you just copied on the host is there before you paste it locally. Image and file
  transfers keep the speed limit described below.
- When a clipboard holds both text and an image (for example, copied spreadsheet cells), the text is
  sent. The priority is text, then image, then files.
- Files can be large, so they are not sent on every copy, only at stream start and when you return to
  the stream window. The host pastes the copy sent at that moment: after editing a file, copy it again
  and click the stream window once to send the new content.
- **Pasting files copied on the host**: when you copy files or folders on the host, Hermit fetches
  only their list (names, sizes, times) and puts it on the clipboard at once, like Remote Desktop.
  When you paste in File Explorer, Explorer shows its own copy progress and each file is downloaded
  from the host while Explorer copies it, so pasting starts right away and nothing is downloaded
  unless you paste.
  - The speed limit applies. Explorer's Cancel stops the download; Ctrl+Alt+Shift+T in the stream
    window does too, and Explorer then reports the file as cancelled.
  - Up to 4 GB and 1,000 items. Files can be pasted as often as you like while the stream runs; each
    paste downloads them again. When the stream ends (or clipboard sync is turned off), Hermit removes
    them from the clipboard, and a paste still in progress fails.
  - A file that was changed or deleted on the host after you copied it fails to paste (File Explorer
    shows an error); copy it again on the host.
  - A download that stops (network loss, or the host's 30-minute limit per request) continues from
    where it stopped. It waits up to 5 minutes for data, since the host may first have to download a
    OneDrive placeholder; File Explorer shows the copy waiting, and its Cancel ends it. The file fails
    when two attempts in a row bring no data. While the host is still busy with an earlier read of the
    same file (HTTP 503), for example one that just stopped, the paste asks again every 1 to 2 seconds
    for up to 5 minutes; these answers do not count as attempts.
  - The list itself may take up to 2 minutes (folders in OneDrive, for example). If it does not
    arrive, or the network fails while it is fetched, it is fetched again on a later check (unless
    you copied something locally since), with one notice for those files; after two more such
    failures it is not fetched again until the host clipboard changes.
    Files that come over as one archive (described below) are fetched once more after a network
    error or timeout, with a notice of their own also when their list failed before; when that
    fails too, they are not fetched again until the host clipboard changes. After an error on the host (HTTP 500, for example), host files are fetched once more;
    when that fails too, they are not fetched again until the host clipboard changes.
  - This needs a current Shell host. With older Shell versions, and for folders with paths over 259
    characters, the files come over as one archive (256 MB limit) and are pasted from a temporary
    folder, as described next.
- Files received as an archive are unpacked to a temporary folder (`%TEMP%\HermitClipboard` on the
  client; Shell uses `%LOCALAPPDATA%\Temp\ShellClipboard` of the user) and placed on the clipboard as
  "copied", so they can be pasted in File Explorer. The last 5 transfers are kept; older ones are
  deleted.
- A received file list is written only after the paths, duplicates and sizes have all been checked.
  Symbolic links and junctions are not followed in either direction; OneDrive Files On-Demand
  placeholders are ordinary files and are downloaded as they are sent. App execution aliases (the
  commands in `WindowsApps`) and Unix sockets cannot be read and are skipped when sending.
- Both sides track the clipboard sequence number, so content that was delivered is not sent again
  and a side never receives back what it wrote itself. Content that could not be delivered is the
  exception, as described next.
- **When both sides changed, the most recent change wins.** Content that could not be delivered is
  retried until something newer replaces it:
  - Something you copied locally that did not reach the host (busy host, network error) is sent
    again the next time you return to the stream window, unless the host's clipboard changed after
    you copied it: then the host's content wins and comes over.
  - Host content that did not arrive (busy host, network error, or the local clipboard could not be
    written) is fetched again on a later check, unless you copied something locally after it: then
    your copy wins and is sent to the host. The same applies to host content still on its way when
    you copy locally, also when Hermit has not handled that copy yet. A host change counts from the
    check that found it: what you copy locally before that check loses to it, what you copy after
    wins. Content that could not be written to the local clipboard (another program held it) is
    fetched once more; when that fails too, not again until the host clipboard changes.
  - An emptied host clipboard is not a change: nothing comes over, what you copied locally is
    still sent, and what is written on the host next counts from the check that finds it. Content
    the host cannot send (formats Shell does not sync: a program's own formats, RTF or HTML only)
    is a change: nothing comes over, but a copy made locally before it is not sent over it either.
    Shell 1.1.0 cannot tell the two apart and answers for such content as for an empty clipboard,
    so with it that content is not a change and the older local copy is still sent.
  - An empty local clipboard does not count as a copy, nor does a list of host files that Hermit is
    still putting on the clipboard. What another program copies meanwhile does (an e-mail
    attachment, for example).
  - When sync could not be set up (the host could not be reached at stream start, for example), it
    is tried again every 2 seconds (and whenever you copy). Once it is set up, Hermit cannot tell
    whether the host's clipboard changed in between, so the host's content counts as a change made
    at the last attempt that failed: what you copied before that attempt is not sent, and the
    host's content comes over (copy again to send yours); what you copied after it is sent.
- When another program on the host holds its clipboard at that moment, the host answers that it is
  busy (HTTP 503). Nothing is shown; local content moves the next time you return to the stream
  window, and host content on a later check: the next four checks come quickly (300 ms apart,
  about a second in all) while the host clipboard is busy or, right after a copy there, still empty,
  then every second as usual. Leaving the stream window starts such a quick run as well.
  Likewise, a copy made on this PC while another program still holds the clipboard (a screenshot
  with Win+Shift+S, for example) is read again for about a second before it waits for the next
  focus change. Note that global screenshot hotkeys (PrtSc tools, Win+Shift+S) act on the PC whose
  keyboard you press, so a screenshot taken while streaming lands on this PC's clipboard and is sent
  to the host, not the other way round.
  Files dropped on the stream window are not sent again by themselves: the notice says the
  host clipboard is busy, and you drop them again. When the host refuses content (HTTP 422), it names
  the reason (names it can't create, only links, an image it can't convert), and the notice says
  which.
- When the network fails while an image or copied files are sent, they are sent once more the next
  time you return to the stream window; if that fails too, copy them again. When every byte of the
  files was sent but the host did not answer within 5 minutes,
  the files are not sent again: the host most likely took them, and the notice says they may still
  arrive. When they do, they are not fetched back over your local copy. With a Shell host that
  streams files, Hermit recognizes them by their top-level names and total size, also when they
  arrive after something newer was copied on either side, and then they replace nothing (other host
  files come over as usual); with a host that sends an archive, the next new host content is taken
  for them if it is files and you copied nothing locally since. After 30 minutes from the start of
  the upload the host can no longer place them, and new host files come over as usual. When the host
  cannot be reached at all at that moment, what you copied is sent the next time you return to the
  stream window (but see above for a setup that failed).
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
    direction for 60 seconds fails; reading the local files counts, so an upload waits while OneDrive
    downloads a placeholder that is being sent. Once the last byte is sent, Hermit waits up to
    5 minutes for the host's answer, since the host still unpacks the files and places them on its
    clipboard (an antivirus scan can slow this down). Files received from the host as one archive
    wait up to 5 minutes, since the host may first download its own placeholders.
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
  the host can't copy ("unsupported or duplicate names") or only links ("only links or nothing to
  copy"), a whole drive copied locally, host text the host could not read (HTTP 500) or refused as
  too large (HTTP 413; after a network error or timeout, text is fetched again silently on a later
  check), a host image that did not
  arrive because of a network error or timeout (said once for that image, which is fetched once
  more on a later check, and after a second failure not until the host clipboard changes), a local
  image or copied files that a network error stopped (said once; sent once more as described
  above, local text silently and each time), or a transfer that failed. Content retried this way
  is dropped without a notice once newer content replaces it.
  Host content may arrive while you work in another app, where the notice is easy to miss, so a
  notice about host content (or a missing permission) is shown once more when you return to the
  stream window.
- Turn it off with Settings → Streaming conveniences → "Sync clipboard with the host" (on by
  default).
- **Permissions**: the host grants each direction separately in its device permissions. Clipboard
  Read lets this device fetch the host's clipboard; Clipboard Set lets it send to the host's
  clipboard. Files also need File Download (from the host) or File Upload (to the host). A newly
  paired device has them only if the host granted them at pairing (Shell offers presets on its
  pairing page). When one is missing, only that direction stops: for example,
  with Clipboard Set alone, what you copy locally still reaches the host. A notice over the stream names
  the missing permission (once, and once more when you return to the stream window); turn it on in
  the host's device permissions and start a new stream.
- Image and file sync is available in the Windows client only.

## Dropping files onto the stream (Shell)

- Drag files or folders from File Explorer onto the stream window to put them on the **host's
  clipboard**, then paste them with Ctrl+V in the folder you want on the host.
- It uses the same path as clipboard sync (Shell host, clipboard and file upload permissions for
  this device), with the same 256 MB limit, speed limit and cancel shortcut (Ctrl+Alt+Shift+T).
- The result is shown over the stream: the number of items and their size, or that clipboard sync is
  off, permission is missing, the limit was exceeded, or the host clipboard was busy (another program
  on the host held it; drop the files again).
- This works best in windowed mode. In full screen, minimize with Ctrl+Alt+Shift+D or switch to a
  window with Ctrl+Alt+Shift+X to reach File Explorer.

## Local cursor (Shell)

Hermit has two mouse modes: the game mouse (relative: the pointer is captured and its movements are
sent) and the remote desktop mouse (absolute: the pointer moves freely and its position is sent),
chosen in Settings → Input ("Optimize mouse for remote desktop instead of games") and switched
during a stream with Ctrl+Alt+Shift+M or the stream panel. Normally the host draws the mouse pointer
into the video, so in remote desktop mode the pointer you see lags behind your hand by the stream's
delay (capture interval, encoding, network, decoding and V-Sync: typically 25 to 40 ms).

- **What the local cursor does.** With a Shell host that supports it, Hermit asks the host to stop
  drawing the pointer into the video and to send the cursor's shape, hotspot and visibility instead.
  Hermit then shows the pointer itself, as the Windows cursor in exactly the shape the host uses
  (arrow, I-beam, hand, resize arrows, busy spinner and so on, scaled with the video). The pointer
  follows the mouse without any stream delay; only the picture under it keeps the usual delay. When
  the host hides its cursor (full-screen video, typing in a text field, a game that hides it), the
  local pointer is hidden too, and comes back as soon as the host shows it.
- **When it is used.** Only in remote desktop mouse mode, only with a host that advertises the
  feature (older Shell hosts, hosts with the feature turned off, and other GameStream hosts behave
  exactly as before), and only when the host can read cursor shapes (the Desktop Duplication
  capture; with Windows Graphics Capture the host declines). Switching to the game mouse returns
  the pointer to the host within a frame; switching back hands it to Hermit again. A reconnect keeps
  the mode. Inverted (XOR) cursor pixels, which a static cursor cannot show, are drawn mid-grey by
  the host's conversion; it is noticeable mostly on the text cursor over dark backgrounds.
- **The setting.** "Local cursor in remote desktop mouse mode" in Settings → Input (on by default)
  and "Local cursor (remote desktop mouse)" in the stream panel, which applies at once. Turn it off
  to see the host's pointer exactly as the host draws it.
- **The overlay line.** On hosts with the feature, the performance overlay's optional "Cursor"
  metric (Settings → Performance overlay, or "Choose metrics" in the stream panel) shows who draws
  the pointer: "local" (Hermit draws it), "host" (the host draws it: the game mouse, the setting
  off, or the host declined) or "host (another client connected)".
- **Mixed sessions.** The host captures one picture for every client, so it can only leave the
  pointer out when every connected client uses a local cursor. While another client streams the
  same desktop without one (for example a phone, or an older client), the host draws the pointer
  again for everyone and Hermit hides its own; the overlay then says "host (another client
  connected)". When that client leaves, Hermit's local cursor returns by itself.
- Hermit keeps the shapes a stream has used (up to 128, 16 MB) so a cursor the host switches back
  to appears at once; a shape it does not have is fetched from the host, with one more try 2 seconds
  later if the first fails, and a request to switch the mode that got no answer is repeated once
  after 2 seconds.
- Ctrl+Alt+Shift+C still shows the system arrow instead of the host's shape, and hides it again.

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
  time, decoder and cursor (which side draws the pointer on hosts with the local cursor feature; see
  [Local cursor](#local-cursor-shell)).
- Estimated total latency = host latency + half the round-trip time + decode + queue delay + render.
  It is an estimate and can differ from the real input-to-display latency (monitor response time and
  so on are not included).
- Values cover roughly the last 2 seconds and refresh every second. During a stream, the stream panel
  changes the same options.

## Network diagnostics

A measurement mode for stutter on an unstable connection (the picture running in slow motion, then
catching up). It is off by default and changes nothing about the stream; it only records what
happened, so the behaviour can be analysed before any control change.

- Turn it on with Settings → Performance overlay → Diagnostics → "Record network diagnostics to a
  file", or with the same checkbox in the stream panel (starts or stops the recording at once).
- While a stream runs, Hermit writes one row every 100 ms to `%TEMP%\Hermit-netdiag-<unix time>.csv`.
  A toast names the file when the recording starts; when the stream ends, the log gets one summary
  line (rows, longest arrival gap, pacer drops, network loss). The last 5 files are kept.
- Each row covers the 100 ms before it: frames received, decoded and rendered, frames the network
  dropped (skipped frame numbers), frames the pacer discarded, the longest pacing and render queue
  seen, the longest gap between two frame arrivals (a stall still in progress counts), average host
  latency, decode and render time, round-trip time and its variance, loss, the requested (chosen),
  applied (host-confirmed) and automatic bitrates, V-Sync and frame pacing, and an event column
  (bitrate changes, network adaptation decisions, renderer resets).
- The counters are the ones the performance overlay uses, read with lock-free atomics; a recording
  costs one wake-up of its own thread every 100 ms and a small file write. When the mode is off, the
  frame path checks one flag per frame and nothing else.
- Nothing leaves this PC: the file stays in the temp folder until you copy it (clipboard sync and
  file transfers work as usual).

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

## Shutting down, restarting or sleeping a host (Shell)

- In the PC view, right-click a host (or press the menu key) and choose **Shut down PC…** or **Restart
  PC…**. Apps running on the host are asked to close first.
- **Put PC to sleep…** and **Hibernate PC…** are in the same menu when the host reports that it can
  do them (Shell; hibernation needs the hibernation file, `powercfg /hibernate on`). Hermit asks the
  host when the menu opens and hides the items otherwise. Nothing on the host is closed: its apps
  continue when it wakes.
- Before sleep or hibernation, Hermit warns when the host says Wake-on-LAN is off on its wired network
  adapter, or could not check it: the PC may then not be reachable until someone turns it on. When
  Wake-on-LAN works, **Wake PC** in the PC's menu wakes it (from sleep this needs the adapter's
  "Wake on Magic Packet" setting and the "Allow this device to wake the computer" permission; from
  hibernation also the firmware's wake-on-LAN setting).
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
    formula's value for the chosen codec: the HEVC/AV1 figure when that codec is forced, otherwise
    the H.264 figure (with automatic codec selection the stream may fall back to H.264, which must
    not be left short). Moving the slider or typing a different value stops that; leaving the field
    without a new value does not.
  - **Use recommended (N Mbps)**, under the recommendation in Settings and in the stream panel, sets
    the same value and lets the bitrate follow the resolution and frame rate again.
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
  48010 and UDP 47998 to 48000 by default; never forward the web UI port 47990). Over the Internet,
  the host's router must forward these ports, or both sides must be on the same VPN such as Tailscale
  (see [Shell's remote access guide](https://github.com/junopark00/hermit-shell/blob/main/docs/remote-access.md)).
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
- **The mouse feels slow in remote-desktop mode.** Most of the delay is waiting, not processing:
  the host captures the desktop at the virtual display's refresh rate and the client shows the frame at
  the next V-Sync, up to one frame interval each. For desktop work, 120 FPS with V-Sync off (both in
  the stream panel) is noticeably quicker than 60 FPS with V-Sync on; the overlay's host and round-trip
  times stay the same, so judge it by feel. Game mouse mode is not affected in the same way.
- **Pairing fails.** Make sure the PIN was entered on the right host within about 5 minutes (after
  that, select the host again for a new PIN). **Open Shell pairing page** uses the address Hermit
  reached the host at and the host's web UI port (HTTP port + 1, normally 47990); if the host's web
  UI listens elsewhere, or the PIN dialog says the page could not be opened (no browser), open it
  yourself and enter the PIN. If the page says **The web UI is not available from this device**, the
  host (Shell 1.1.2 or later) accepts the web UI from the PC itself only: enter the PIN on the host PC,
  or have **Settings > Network > Web UI Access** in Shell changed to the local network.
- **Logs.** Each run writes `%TEMP%\Hermit-<number>.log`. Attach the log of the affected run when
  reporting a problem (it contains host names and addresses, but no clipboard contents).
