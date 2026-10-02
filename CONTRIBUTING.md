# Contributing to Hermit

Thank you for your interest in Hermit. Bug reports, feature ideas and pull requests are welcome.

## Supported platform

Hermit supports **Windows 10 and 11 (x64)**. The macOS, Linux and embedded code paths inherited from
Moonlight are still in the source tree but are not maintained or tested. Changes for those
platforms are accepted only if they do not affect Windows, and they will not be tested by the
maintainer.

## Reporting bugs

Open an issue on [GitHub](https://github.com/junopark00/hermit/issues) and include:

- the Hermit version (`Hermit.exe --version`, or the file name of the release you downloaded) and
  the Windows version;
- the host software and its version (for example Shell, Sunshine or Apollo), and the GPU of both
  PCs;
- the steps that lead to the problem, and what you expected instead;
- the log of the affected run, `%TEMP%\Hermit-<number>.log`. Logs contain host names and IP
  addresses; remove them if you prefer.

For security problems, do not open a public issue; see [SECURITY.md](SECURITY.md).

Problems that also occur in Moonlight are usually best reported to the
[Moonlight project](https://github.com/moonlight-stream/moonlight-qt). Do not report Hermit
problems to the Moonlight project.

## Pull requests

1. Discuss larger changes in an issue first, so the direction is agreed before you invest time.
2. Build with `hermit\build-windows.bat` and run the tests (see [Building Hermit](docs/building.md)):

   ```powershell
   powershell -ExecutionPolicy Bypass -File hermit\tests\run-tests.ps1
   ```

3. Keep each pull request focused on one change, and describe what it changes and how you tested it
   (host, codec, windowed or full screen, and so on).

### Code guidelines

- Follow the style of the surrounding code (Qt and C++17 conventions as in the rest of `app/`).
- In files inherited from Moonlight, mark Hermit-specific changes with a comment that starts with
  `Hermit:` where the reason is not obvious. Keep such changes small, so future Moonlight changes
  remain easy to merge.
- Write code comments, log messages and documentation in English.
- Never log clipboard contents, credentials or other user data.
- Hermit must not contact services other than the hosts the user adds and the STUN server
  documented in the README. Any new network access needs a clear reason and must be documented in
  the README's privacy section.

### User-visible text and translations

Every user-visible string has a Korean translation, and the build checks this.

- Use `qsTr()` in QML and `tr()` or `QCoreApplication::translate()` in C++.
- Add the Korean translation of every new or reworded string to `TRANSLATIONS` in
  `hermit/translations/make_hermit_ts.py`, then run `python hermit/translations/make_hermit_ts.py`.
  If you do not speak Korean, say so in the pull request and the maintainer will add the
  translation.
- `python hermit/tests/check-translations.py` must pass.

### QML

`python hermit/tests/check-qml-members.py` checks that QML uses only members the exposed C++ objects
have. Run it after renaming or removing a property, method or signal.

## License

Hermit is licensed under the GNU General Public License, version 3 or (at your option) any later
version (GPL-3.0-or-later). By submitting a contribution, you agree
that it is licensed under the same license.
