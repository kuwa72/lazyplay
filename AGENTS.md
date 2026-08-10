# lazyplay — project notes for agents

Lightweight AirPlay mirroring receiver for Windows (Win32 + D3D11 + Media Foundation + WASAPI, no external deps beyond vendored playfair; FFmpeg is downloaded and built in `thirdparty/` on first build).

## Build & test

- `make` — builds `lazyplay.exe` (MinGW g++/gcc or MSYS2 UCRT64; MSVC via CMakeLists.txt)
- `make test` then `./test/test_all.exe unit` — crypto / bplist / AES-CBC / WASAPI unit tests
- `./test/test_all.exe decode test/test.h264` — MFT decode + NV12 render readback test
- `./test/test_all.exe wasapi` — 440 Hz sine playback smoke test (listen for tone)
- `./test/test_all.exe e2e <host> test/test.h264` — full AirPlay handshake + encrypted streaming against a running `./lazyplay.exe`
- Regenerate the video fixture: `ffmpeg -y -f lavfi -i testsrc=size=1280x720:rate=30 -t 2 -c:v libx264 -profile:v baseline -bf 0 -g 30 -pix_fmt nv12 -f h264 test/test.h264`

## Architecture / protocol (UxPlay-compatible)

- `mdns_sd.cpp` — mDNS announcer/responder for `_airplay._tcp` (7000) + `_raop._tcp` (5000). Features `0x527FFEE6,0x0`: bit 27 (legacy pairing) is **off**, so clients skip SRP pair-setup/pair-verify.
- `rtsp_server.cpp` — RTSP/plist session: GET /info, POST /fp-setup (FairPlay), SETUP (ekey/eiv + streams), RECORD, TEARDOWN, /feedback. SETUP bodies are binary plists (`bplist.cpp`).
- `fairplay.cpp` + `src/playfair/` (vendored from UxPlay, see src/playfair/LICENSE.md) — fp-setup handshake + ekey unwrap. fp-setup phase-2 request byte[12] selects SAP mode 0-3.
- `video_stream.cpp` — TCP mirror data channel: 128-byte headers (LE size/type/timestamp), type 0x00 = AES-128-CTR encrypted length-prefixed NALs (keystream continuous across packets), type 0x01 = SPS/PPS (cached, prepended to the next IDR with the same timestamp).
- Video key/IV = SHA-512("AirPlayStreamKey"<decimal streamConnectionID> + audioAesKey)[0:16] (same for "AirPlayStreamIV").
- `ntp_timing.cpp` — timingProtocol=NTP: sends 32-byte requests to the client timing port every 3 s.
- `decoder_d3d11.cpp` — MS H.264 MFT, low-latency mode, D3D11 NV12 texture output (no software fallback by design, REQUIREMENTS 2.2).
- `renderer_d3d11.cpp` — NV12→RGB (BT.601 limited) pixel shader, aspect-preserved letterbox, Alt+Enter fullscreen, Shift+Alt+Enter cycles fullscreen across displays, Esc/Q quit.
- `main.cpp` — WindowProc: tap / right-click / long-press opens a control menu (Toggle fullscreen / Move to next display / Exit); Esc/Q quits; Alt+Enter toggles fullscreen; Shift+Alt+Enter moves fullscreen to the next display. Calls `SetThreadExecutionState` on startup to prevent sleep/screen-off while mirroring.
- `audio_stream.cpp` — RTP/UDP type-96 receiver: AES-128-CBC per-packet decrypt (per-packet IV reset, partial tail passthrough), duplicate elimination, jitter buffer, FFmpeg AAC-ELD decode → 44.1 kHz s16 stereo PCM.
- `audio_decoder.cpp` — wrapper around FFmpeg's native `libavcodec` AAC decoder (LGPL) for AAC-ELD.
- `audio_wasapi.cpp` — WASAPI shared-mode render thread, resampling to the mix format and volume dB → linear gain.

## Constraints / non-goals

- Pairing (SRP/Ed25519) intentionally not implemented; keep feature bit 27 cleared.
- H.265 requires feature bit 42 + HEVC decoder — not implemented.
