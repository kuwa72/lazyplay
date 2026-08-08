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

## 使い方

1. `lazyplay.exe` を起動すると全画面で開きます（`-window` でウィンドウ起動）
2. Mac と同一ネットワークに接続し、Mac の画面ミラーリングから `lazyplay-display` を選択
3. 終了は 右クリック / タッチ長押し（または `Esc`/`Q`）、`Alt+Enter` で全画面/ウィンドウ切替

**ファイアウォール**: AirPlay は内向き TCP 5000/7000（と mDNS UDP 5353）を使います。
初回起動時に Windows ファイアウォールの許可ダイアログが出たら許可してください。
Mac 側にデバイスは見えるのに接続だけできない場合はファイアウォールを確認してください
（コントロール パネルの「許可されたアプリ」から追加するか、管理者権限で）:

```
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

Windows 上の MinGW-w64 (gcc/g++) で:

```
make            # lazyplay.exe
make test       # ユニットテスト (SHA-512 / AES-CTR / AES-CBC / bplist / FDK AAC / WASAPI)
```

統合テスト（実 H.264 ストリームのデコード＆描画検証 / プロトコル E2E）:

```
./test/test_all.exe decode test/test.h264
./lazyplay.exe &                      # 別プロセスで起動
./test/test_all.exe e2e 127.0.0.1 test/test.h264
```

`test/test.aac`（ADTS 形式）があれば、AAC デコーダの検証もできます:

```
./test/test_all.exe adec test/test.aac
./test/test_all.exe wasapi   # 440 Hz サイン波が鳴る簡易再生テスト
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
- 音声は RTP/UDP (stream type 96) を AES-128-CBC で復号し、
  vendored Fraunhofer FDK AAC (`src/fdk-aac/`) で AAC-ELD を PCM にデコード、
  WASAPI 共有モードで再生

## License

GPLv3 — `playfair` (FairPlay SAP) を vendoring している関係上、本プロジェクト全体も GPLv3 で公開します。
See [LICENSE](LICENSE).

Fraunhofer FDK AAC デコーダは `src/fdk-aac/NOTICE` の条項の下で vendoring しています。
