# lazyplay

Mac の AirPlay ミラーリング機能を使い、低スペックな Windows 端末（Intel Atom 系など）を
ワイヤレス・サブディスプレイ化する軽量 AirPlay レシーバーです。

*Turn a low-spec Windows PC (e.g. Intel Atom) into a wireless sub-display for your Mac via AirPlay mirroring.*

## 特徴

- **GPU ハードウェアデコード (DXVA2/D3D11)**: H.264 を CPU ではなく GPU でデコード。
  ソフトウェアデコードへのフォールバックは意図的に持たない設計です
- **Win32 + Direct3D11 ネイティブのみ**: Electron/Qt 等の重い依存なし。単一 exe で持ち運び可能
- **低遅延**: 受信→描画までを最小化（デコードしたフレームを即時 Present）
- **音声も同時再生**: AirPlay ミラーリング音声（AAC-ELD / 44.1 kHz ステレオ）を復号し、WASAPI でスピーカー出力
- **ペアリング不要**: feature bit 27 を切ってあるため、Mac 側での PIN 入力なしに接続できます
- **スリープ防止**: 起動中は Windows のスリープ/画面オフを抑制

## Pros / Cons（メリット・デメリット）

### Pros（メリット）

- **Lightweight / 軽量**: Win32 + D3D11 native only; no Electron/Qt. A single portable `lazyplay.exe`.
- **GPU hardware decode / GPU ハードウェアデコード**: H.264 decoded by D3D11 / DXVA2, easy on low-spec CPUs like Intel Atom.
- **Low latency / 低遅延**: decoded frames are presented immediately.
- **Audio support / 音声再生**: AirPlay mirroring audio (AAC-ELD 44.1 kHz stereo) decoded and played via WASAPI.
- **No pairing required / ペアリング不要**: connects without a PIN on the Mac/iPhone side.
- **Tablet-friendly / タブレット対応**: tap or long-press to open a control menu (toggle fullscreen, move to next display, exit).
- **Prevents sleep / スリープ防止**: keeps the host awake and the display on while mirroring.

### Cons（デメリット）

- **Windows only / Windows 専用**: requires Windows 10/11 x64.
- **GPU-dependent / GPU 必須**: needs a D3D11 GPU with H.264 hardware decode; no software decode fallback.
- **No DRM content / DRM 非対応**: Netflix and other DRM-protected content are blacked out on the sender side (Apple limitation, same as Apple TV).
- **No AirPlay 2 / AirPlay 2 非対応**: H.265 and AirPlay 2 features are not implemented.
- **No reverse control / 逆方向制御不可**: the receiver cannot send touch/keyboard input back to the Mac/iPhone. AirPlay mirroring is one-way.

## 使い方

1. `lazyplay.exe` を起動すると全画面で開きます（`-window` でウィンドウ起動）
2. Mac/iPhone と同一ネットワークに接続し、画面ミラーリングから `lazyplay-display` を選択
3. タップ / 右クリック / タッチ長押しでメニュー（Toggle fullscreen / Move to next display / Exit）が開きます。終了は `Esc`/`Q` でも可能
4. `Alt+Enter` で全画面/ウィンドウ切替、`Shift+Alt+Enter` で全画面を次のディスプレイへ移動

**ファイアウォール**: AirPlay は内向き TCP 5000/7000（と mDNS UDP 5353）を使います。
初回起動時に Windows ファイアウォールの許可ダイアログが出たら許可してください。
Mac/iPhone 側にデバイスは見えるのに接続だけできない場合はファイアウォールの設定を確認してください。

**推奨**: コントロール パネルの「Windows Defender ファイアウォール」→「許可されたアプリ」
→「別のアプリの許可」から `lazyplay.exe` を追加してください。

または管理者権限の PowerShell / コマンドプロンプトで:

```powershell
netsh advfirewall firewall add rule name="lazyplay" dir=in action=allow program="C:\path\to\lazyplay.exe" enable=yes profile=any
```

### コマンドラインオプション

| オプション | 説明 | デフォルト |
|---|---|---|
| `-name <name>` | AirPlay 上の表示デバイス名 | `lazyplay-display` |
| `-fps <30\|60>` | 最大フレームレート | `30` |
| `-res <720p\|1080p>` | 受信解像度 | `1080p` |
| `-vsync <0\|1>` | 垂直同期 | `1` |
| `-window` | 全画面ではなくウィンドウで起動 | off（全画面がデフォルト） |

* 描画は Per-Monitor-V2 DPI aware のため、Windows の表示スケーリング（125% 等）下でも
  物理ピクセルに 1:1 で描画されます。デフォルトの全画面起動でパネル解像度と一致すれば
  ドットバイドット表示になります（ウィンドウモードでは枠・タイトルバー分だけ領域が減ります）。
* キーボード無しのタブレット運用を想定: 終了は右クリック / タッチ長押しです。
  全画面時はマウスカーソルも非表示になります。
* Netflix 等の DRM 保護コンテンツはミラーリング画像に含まれません（macOS 側で黒化される
  仕様。Apple TV 等でも同様の制約があります）。

## ビルド

Windows 上の MinGW-w64 (gcc/g++) / MSYS2 (UCRT64) で:

```
make            # lazyplay.exe（初回は FFmpeg ソースもダウンロード・ビルド）
make test       # ユニットテスト (SHA-512 / AES-CTR / AES-CBC / bplist / WASAPI)
```

統合テスト（実 H.264 ストリームのデコード＆描画検証 / プロトコル E2E）:

```
./test/test_all.exe decode test/test.h264
./lazyplay.exe &                      # 別プロセスで起動
./test/test_all.exe e2e 127.0.0.1 test/test.h264
```

```
./test/test_all.exe wasapi   # 440 Hz サイン波が鳴る簡易再生テスト
```

MSVC は FFmpeg ソースビルドに未対応のため、MSYS2 UCRT64 + `make` を推奨します。`CMakeLists.txt` は MinGW-w64 用に FFmpeg 自動ビルドを含みます。

## 動作環境

- Windows 10 / 11 (x64)
- D3D11 + H.264 ハードウェアデコード対応 GPU（Intel HD Graphics 等）
- macOS 側からの画面ミラーリング（同一 L2 ネットワーク、mDNS 到達が必要）

## 技術構成

- mDNS アナウンス (`_airplay._tcp` / `_raop._tcp`)、RTSP+plist セッション、
  FairPlay SAP 鍵交換、AES-128-CTR 映像復号、NTP タイミング — プロトコルは
  [UxPlay](https://github.com/FDH2/UxPlay) / RPiPlay の実装を参照しています
- FairPlay 部分は UxPlay 同梱の `playfair` を vendoring (`src/playfair/`)
- 音声は RTP/UDP (stream type 96) を AES-128-CBC で復号し、
  FFmpeg のネイティブ AAC デコーダ（LGPL）で AAC-ELD を PCM にデコード、
  WASAPI 共有モードで再生

## License

GPLv3 — `playfair` (FairPlay SAP) を vendoring している関係上、本プロジェクト全体も GPLv3 で公開します。
See [LICENSE](LICENSE).

FFmpeg は初回ビルド時に `thirdparty/` にダウンロード・ビルドされます。`libavcodec` の
ネイティブ AAC デコーダは LGPL-2.1-or-later で、GPLv3 とのリンクが可能です。
