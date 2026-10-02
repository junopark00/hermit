# Security policy

## Supported versions

| Version | Supported |
|---|---|
| 1.0.x | Yes |
| Earlier builds | No |

Security fixes are released for the latest 1.0.x version. Windows 10 and 11 (x64) is the supported
platform; code for other platforms inherited from Moonlight is not maintained.

## Reporting a vulnerability

Please report vulnerabilities **privately** through GitHub's private vulnerability reporting:

1. Open the [Security tab](https://github.com/junopark00/hermit/security) of this repository.
2. Click **Report a vulnerability** and fill in the advisory form.

Do not open a public issue, pull request or discussion for a vulnerability before it is fixed.

Please include:

- the affected version and Windows version, and the host software if relevant;
- a description of the problem and its impact;
- steps to reproduce, or a proof of concept;
- any suggested fix.

You can expect an acknowledgment within a week. Once the issue is confirmed, a fix is prepared and
released, and the advisory is published with credit to the reporter unless you prefer otherwise.

## Scope

In scope are Hermit's own code and the way it uses its components, for example: pairing and the
client identity, the handling of data received from a host (clipboard text, images and file
archives), the single-file launcher, and the build scripts.

Vulnerabilities in the host software should be reported to that project (for Shell:
[junopark00/hermit-shell](https://github.com/junopark00/hermit-shell)). Vulnerabilities in code that
Hermit shares with Moonlight-qt or moonlight-common-c should also be reported to the
[Moonlight project](https://github.com/moonlight-stream), and in third-party libraries (FFmpeg,
SDL, OpenSSL, Qt and others) to their maintainers.
