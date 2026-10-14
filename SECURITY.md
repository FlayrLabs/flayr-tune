# Security Policy

## Supported versions

Security fixes go into the latest release. Please update to it before reporting.

| Version | Supported |
|---|---|
| Latest release | Yes |
| Older releases | No |

## Reporting a vulnerability

**Please don't open a public issue for security problems.**

Report it privately through GitHub instead:
**[Report a vulnerability](https://github.com/FlayrLabs/flayr-tune/security/advisories/new)**

Include what you found, the version, OS and DAW, and the steps or a file that reproduces it.

What to expect:
- We reply within 3 business days.
- We aim to release a fix within 30 days for confirmed issues, sooner for serious ones.
- We credit you in the advisory and release notes, unless you'd rather stay anonymous.
- We'll coordinate with you on when the details are made public.

## What's in scope

Flayr Tune runs inside your DAW, so anything that could affect your computer or your projects is in scope, for example:
- A project, preset or saved plug-in state that crashes the DAW, uses unbounded memory, or runs code when loaded.
- Problems in the installers or install scripts.
- Release files that don't match this source code or aren't signed as described below.

Audio quality bugs and crashes that need no untrusted input are regular bugs: please open a normal issue.

## What Flayr Tune does and doesn't do

- **No network access, and no networking code.** The plug-in never connects to the internet. There is no telemetry, no analytics, no licence check and no auto-update. Releases after 1.3.0 go further: the HTTP, socket and network-interface code that JUCE normally compiles in is replaced by stubs that always fail ([`patches/juce-no-network.patch`](patches/juce-no-network.patch)), so the plug-in binary imports no networking functions at all. See [Checking for networking code](#checking-for-networking-code).
- **No files written** outside your DAW project. Settings and Graph edits are saved inside the project by your DAW.
- **No elevated privileges.** The plug-in runs with your DAW's permissions. The installers only copy the plug-in into the standard plug-in folders.
- Untrusted data the plug-in reads (saved state from a project) is size-limited and validated before use.

## Verifying your download

Every release includes `SHA256SUMS.txt`. Check your file against it:

```sh
shasum -a 256 FlayrTune-*-macOS.dmg                    # macOS
certutil -hashfile FlayrTune-*-Windows-Setup.exe SHA256  # Windows
sha256sum FlayrTune-*-Linux-x64.tar.gz                   # Linux
```

**macOS:** the DMG and plug-ins are signed with Flayr Labs' Apple Developer ID (team `NRNU83UJ68`) and notarized by Apple. To check:

```sh
spctl --assess --type open --context context:primary-signature -v FlayrTune-*-macOS.dmg
codesign -dv --verbose=2 "/Library/Audio/Plug-Ins/Components/Flayr Tune.component"
```

You should see `Notarized Developer ID` and `Developer ID Application: Flayr Labs LLC (NRNU83UJ68)`.

**Windows and Linux:** these files are built from the tagged source by GitHub Actions, in public. Releases after 1.3.0 also carry a signed build provenance attestation. With the [GitHub CLI](https://cli.github.com):

```sh
gh attestation verify FlayrTune-*-Windows-Setup.exe --repo FlayrLabs/flayr-tune
```

The Windows installer isn't Authenticode-signed yet, so Windows SmartScreen may warn about it.

## Checking for networking code

Releases after 1.3.0 contain no networking code, and CI fails any build whose plug-in imports a
networking API ([`scripts/check-no-network.sh`](scripts/check-no-network.sh)). You can check an
installed plug-in yourself with your platform's own tools; each command should print nothing:

```sh
# macOS (repeat with -arch x86_64 on Intel)
nm -u -arch arm64 "/Library/Audio/Plug-Ins/VST3/Flayr Tune.vst3/Contents/MacOS/Flayr Tune" \
  | grep -E '^_(socket|connect|bind|listen|accept|getaddrinfo|send|sendto|recv|recvfrom)$|NSURLSession|CFNetwork'

# Linux
nm -D --undefined-only ~/.vst3/"Flayr Tune.vst3"/Contents/x86_64-linux/"Flayr Tune.so" \
  | grep -wE 'socket|connect|bind|listen|accept|getaddrinfo|send|sendto|recv|recvfrom'
```

```bat
:: Windows, from a "Developer Command Prompt for VS" (dumpbin ships with Visual Studio)
dumpbin /imports "C:\Program Files\Common Files\VST3\Flayr Tune.vst3\Contents\x86_64-win\Flayr Tune.vst3" | findstr /i "wininet winhttp ws2_32 wsock32"
```

Two things you may still see, neither of which is networking: on Linux, `socketpair`, which
JUCE's message loop uses to wake itself inside the process (`AF_LOCAL`, never a network
socket); and `gethostname`, which reads the computer's name.

You can also build Flayr Tune yourself from source; see the [README](README.md#build-from-source).
