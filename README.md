# lazyplay

*[日本語版はこちら / Japanese version](README.ja.md)*

A lightweight AirPlay mirroring receiver that turns a low-spec Windows PC
(e.g. Intel Atom) into a wireless sub-display for your Mac, using macOS's
built-in AirPlay screen mirroring.

## Features

- **GPU hardware decode (DXVA2/D3D11)**: H.264 is decoded on the GPU, not the CPU.
  No software-decode fallback by design.
- **Win32 + Direct3D11 native only**: no Electron/Qt or other heavy dependencies. A single portable `lazyplay.exe`.
- **Low latency**: minimizes the path from receive to render (decoded frames are presented immediately).
- **Audio playback**: decodes AirPlay mirroring audio (AAC-ELD / 44.1 kHz stereo) and plays it back via WASAPI.
- **No pairing required**: feature bit 27 is disabled, so the Mac/iPhone connects without a PIN prompt.
- **Prevents sleep**: keeps the host awake and the display on while mirroring.

## Usage

1. Launch `lazyplay.exe` to start in fullscreen (use `-window` to start windowed instead).
2. Connect your Mac/iPhone to the same network, open Screen Mirroring, and select `lazyplay-display`.
3. Tap / right-click / touch-and-hold opens a control menu (Toggle fullscreen / Move to next display / Exit). You can also quit with `Esc`/`Q`.
4. `Alt+Enter` toggles fullscreen/windowed mode; `Shift+Alt+Enter` moves fullscreen to the next display.

**Firewall**: AirPlay uses inbound TCP 5000/7000 (and mDNS UDP 5353). If a
Windows Firewall prompt appears on first launch, allow access. If your
Mac/iPhone can see the device but cannot connect, check your firewall
settings.

**Recommended**: in Control Panel, go to "Windows Defender Firewall" →
"Allowed apps" → "Change settings" / "Allow another app" and add
`lazyplay.exe`.

### Command-line options

| Option | Description | Default |
|---|---|---|
| `-name <name>` | AirPlay device name shown to senders | `lazyplay-display` |
| `-fps <30\|60>` | Maximum frame rate | `30` |
| `-res <720p\|1080p>` | Receive resolution | `1080p` |
| `-vsync <0\|1>` | Vertical sync | `1` |
| `-window` | Start windowed instead of fullscreen | off (fullscreen is the default) |

* Rendering is Per-Monitor-V2 DPI aware, so the video is drawn 1:1 to
  physical pixels even under Windows display scaling (e.g. 125%). When the
  default fullscreen start matches the panel resolution, this gives a
  dot-by-dot display (windowed mode loses some area to the border/title bar).
* Designed for keyboardless tablet use: use tap / right-click / touch-and-hold
  to open the control menu. The mouse cursor is also hidden in fullscreen.
* DRM-protected content (e.g. Netflix) is not included in the mirrored
  image — this is blacked out on the macOS side by Apple's own restriction,
  the same limitation that applies to Apple TV.

## Build

On Windows with MinGW-w64 (gcc/g++) / MSYS2 (UCRT64):

```
make            # builds lazyplay.exe (downloads and builds FFmpeg on first run)
make test       # unit tests (SHA-512 / AES-CTR / AES-CBC / bplist / WASAPI)
```

Integration tests (real H.264 stream decode & render verification / protocol E2E):

```
./test/test_all.exe decode test/test.h264
./lazyplay.exe &                      # launch in a separate process
./test/test_all.exe e2e 127.0.0.1 test/test.h264
```

```
./test/test_all.exe wasapi   # simple playback test; you should hear a 440 Hz tone
```

MSVC does not support building FFmpeg from source, so MSYS2 UCRT64 +
`make` is recommended. `CMakeLists.txt` includes automatic FFmpeg builds
for MinGW-w64.

## Requirements

- Windows 10 / 11 (x64)
- A GPU with D3D11 + H.264 hardware decode support (e.g. Intel HD Graphics)
- Screen mirroring from macOS (same L2 network, mDNS reachability required)

## Technical overview

- mDNS announcement (`_airplay._tcp` / `_raop._tcp`), RTSP+plist session,
  FairPlay SAP key exchange, AES-128-CTR video decryption, NTP timing — the
  protocol implementation is based on [UxPlay](https://github.com/FDH2/UxPlay) / RPiPlay.
- The FairPlay part vendors UxPlay's bundled `playfair` (`src/playfair/`).
- Audio is RTP/UDP (stream type 96), decrypted with AES-128-CBC, decoded
  from AAC-ELD to PCM by FFmpeg's native AAC decoder (LGPL), and played
  back in WASAPI shared mode.

## License

GPLv3 — because this project vendors `playfair` (FairPlay SAP), the whole
project is published under GPLv3. See [LICENSE](LICENSE).

FFmpeg is downloaded and built into `thirdparty/` on first build.
`libavcodec`'s native AAC decoder is LGPL-2.1-or-later, which can be linked
against GPLv3 code.
