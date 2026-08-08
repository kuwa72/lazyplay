# lazyplay

Mac の AirPlay ミラーリング機能を使い、低スペックな Windows 端末（Intel Atom 系など）を
ワイヤレス・サブディスプレイ化する軽量 AirPlay レシーバーです。

*Turn a low-spec Windows PC (e.g. Intel Atom) into a wireless sub-display for your Mac via AirPlay mirroring.*

## 特徴

- **GPU ハードウェアデコード (DXVA2/D3D11)**: H.264 を CPU ではなく GPU でデコード。
  ソフトウェアデコードへのフォールバックは意図的に持たない設計です
- **Win32 + Direct3D11 ネイティブのみ**: Electron/Qt 等の重い依存なし。バイナリは約 700KB の単一 exe
- **低遅延**: 受信→描画までを最小化（デコードしたフレームを即時 Present）
- **音声は受け取って破棄**: サブディスプレイ用途に特化（要件上、音声デコードは行いません）
- **ペアリング不要**: feature bit 27 を切ってあるため、Mac 側での PIN 入力なしに接続できます

## 使い方

1. `lazyplay.exe` を起動するとウィンドウが開きます
2. Mac と同一ネットワークに接続し、Mac の画面ミラーリングから `lazyplay-display` を選択
3. 終了は `Esc` または `Q`、`Alt+Enter` で全画面/ウィンドウ切替

### コマンドラインオプション

| オプション | 説明 | デフォルト |
|---|---|---|
| `-name <name>` | AirPlay 上の表示デバイス名 | `lazyplay-display` |
| `-fps <30\|60>` | 最大フレームレート | `30` |
| `-res <720p\|1080p>` | 受信解像度 | `720p` |
| `-vsync <0\|1>` | 垂直同期 | `1` |

## ビルド

Windows 上の MinGW-w64 (gcc/g++) で:

```
make            # lazyplay.exe
make test       # ユニットテスト (SHA-512 / AES-CTR / bplist)
```

統合テスト（実 H.264 ストリームのデコード＆描画検証 / プロトコル E2E）:

```
./test/test_all.exe decode test/test.h264
./lazyplay.exe &                      # 別プロセスで起動
./test/test_all.exe e2e 127.0.0.1 test/test.h264
```

MSVC の場合は `CMakeLists.txt` を使用してください。

## 動作環境

- Windows 10 / 11 (x64)
- D3D11 + H.264 ハードウェアデコード対応 GPU（Intel HD Graphics 等）
- macOS 側からの画面ミラーリング（同一 L2 ネットワーク、mDNS 到達が必要）

## 技術構成

- mDNS アナウンス (`_airplay._tcp` / `_raop._tcp`)、RTSP+plist セッション、
  FairPlay SAP 鍵交換、AES-128-CTR 映像復号、NTP タイミング — プロトコルは
  [UxPlay](https://github.com/FDH2/UxPlay) / RPiPlay の実装を参照しています
- FairPlay 部分は UxPlay 同梱の `playfair` を vendoring (`src/playfair/`)

## License

GPLv3 — `playfair` (FairPlay SAP) を vendoring している関係上、本プロジェクト全体も GPLv3 で公開します。
See [LICENSE](LICENSE).
