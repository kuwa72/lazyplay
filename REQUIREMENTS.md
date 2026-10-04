# Software Requirements Specification (REQUIREMENTS.md)

*[日本語版はこちら / Japanese version](REQUIREMENTS.ja.md)*

## 1. Overview

* **Project name**: lazyplay
* **Purpose**: A lightweight AirPlay receiver that uses macOS's AirPlay mirroring to turn a low-spec Windows PC (e.g. Intel Atom class CPU) into a wireless sub-display (extended or mirrored screen).
* **Target platform**: Windows 10 / 11 (x86/x64)
* **Language / tech stack**:
  * Language: C / C++ (C11 / C++17)
  * Rendering / decode APIs: Direct3D 11 / DXVA2 / Windows Media Foundation (DirectX Hardware Acceleration)
  * Networking: WinSock2 / Windows DNS Service Discovery (mDNS / Bonjour)
  * Audio decode: FFmpeg's native AAC decoder (LGPL; downloaded and built into thirdparty/ on first build)
  * Audio output: WASAPI shared mode
  * Reference implementation: open-source AirPlay receiver architectures such as UxPlay / RPiPlay

---

## 2. Functional requirements

### 2.1. AirPlay reception & network protocol
* **mDNS announcement (Bonjour)**:
  * Advertise `_airplay._tcp.local` (port 7000) and `_raop._tcp.local` (port 5000).
  * Configurable device name (e.g. `lazyplay-display`).
* **RTSP handshake & session management**:
  * Handle RTSP sequences such as `ANNOUNCE`, `SETUP`, `RECORD`, `TEARDOWN`, `SET_PARAMETER`.
  * Pair-Setup / Pair-Verify handshake (encryption key exchange as needed).
* **PTP / NTP time sync**:
  * Acquire timestamps for video sync and compensate for lag.

### 2.2. Hardware acceleration (required)
* **DirectX / DXVA2 (D3D11 Video) H.264 decode**:
  * Drive the H.264 hardware decoder built into Intel Atom integrated GPUs (Intel HD Graphics) directly.
  * No fallback to software decode (FFmpeg/CPU), keeping CPU usage as low as possible.
  * Transfer decoded frames to the Direct3D11 swap chain with zero-copy (or minimal graphics-bus load) rendering.

### 2.3. Resolution & frame-rate control
* **Resolution selection**:
  * Configure the screen resolution requested from the Mac during negotiation (default: 1280x720p; 1920x1080p optional).
* **Refresh-rate (frame-rate) cap**:
  * Fixed cap of 30 fps / 60 fps (via command-line argument or config file).
  * Drop-frame handling to prevent latency buildup and buffer accumulation.

### 2.4. Audio processing
* **AirPlay mirroring audio reception & playback**:
  * Receive stream type 96 (AAC-ELD / 44.1 kHz stereo) over RTP/UDP.
  * Decrypt with AES-128-CBC (IV reset per packet; trailing partial block passed through as plaintext).
  * Decode AAC-ELD to 44.1 kHz s16 stereo PCM with FFmpeg's native AAC decoder (LGPL).
  * Output to speakers in WASAPI shared mode, honoring the Mac's AirPlay volume (`SET_PARAMETER volume`).

### 2.5. Minimal GUI / CLI
* **Window display**:
  * Borderless fullscreen or windowed mode.
  * Optional tray icon.
* **Shortcut keys**:
  * `Alt + Enter`: toggle fullscreen/windowed
  * `Esc` or `Q`: quit
  * Tap / right-click / touch-and-hold: show control menu (Toggle fullscreen / Move to next display / Exit; for keyboardless operation)
  * Mouse cursor hidden in fullscreen
* **Command-line options**:
  * `-name <deviceName>`: displayed device name
  * `-fps <30|60>`: maximum frame rate
  * `-res <720p|1080p>`: receive resolution (default 1080p)
  * `-vsync <0|1>`: vertical sync
  * `-window`: start windowed (default is borderless fullscreen)

---

## 3. Non-functional requirements

* **Minimize CPU / memory usage**:
  * CPU usage: target under 10% on a low-spec Atom machine (e.g. 2 cores / 4 threads).
  * Memory usage: keep under 50 MB.
* **Low latency**:
  * Jitter-buffer control to keep receive-to-render latency at roughly 50–100 ms or less.
* **Minimize build & dependencies**:
  * Exclude all heavy third-party UI libraries (Electron, Qt, etc.) and achieve a lightweight build with Win32 API native + Direct3D + WASAPI + FFmpeg (only the minimal AAC decoder needed).

---

## 4. Development roadmap

1. **Phase 1**: Finalize requirements and organize design documents
2. **Phase 2**: Mock up mDNS announcement & RTSP handshake
3. **Phase 3**: Implement H.264 stream reception & DXVA2/D3D11 decode pipeline
4. **Phase 4**: Resolution / FPS control & windowed/fullscreen switching
5. **Phase 5**: Receive, decrypt, and play AirPlay mirroring audio (AAC-ELD) via WASAPI
6. **Phase 6**: Performance tuning (Atom real-device validation, memory optimization)
