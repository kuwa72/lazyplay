# lazyplay

*[English version](README.md)*

iPhone・iPad・Macの画面と音声を、AirPlayでWindows PCに送れる軽量レシーバーです。
手持ちのPCを画面・音声の出力先として再利用できます。低スペック端末でも、対応するH.264ハードウェアデコーダが必要です。

**[Windows x64版をダウンロード](https://github.com/kuwa72/lazyplay/releases/latest)**
Assetsから`lazyplay-win-x64.zip`を選び、展開して`lazyplay.exe`を起動してください。
ビルド環境や送信側の追加アプリは不要です。

![Windows上のlazyplayに映したiPhoneの画面](assets/ios-mirroring.gif)

実際のiPhoneからの画面ミラーリングとアプリ操作の録画です。
元の録画には音声もありますが、このGIFは無音です。音質や遅延の実証には使っていません。
画面と音声を送る際は「画面ミラーリング」を使ってください。音楽・動画だけを送るAirPlayキャストは、この手順の対象外です。

## 特徴

- **GPU ハードウェアデコード (DXVA2/D3D11)**: H.264 を CPU ではなく GPU でデコード。
  ソフトウェアデコードへのフォールバックは意図的に持たない設計です
- **Win32 + Direct3D11 ネイティブのみ**: Electron/Qt 等の重い依存なし。単一 exe で持ち運び可能
- **低遅延**: 受信→描画までを最小化（デコードしたフレームを即時 Present）
- **音声も同時再生**: AirPlay ミラーリング音声（AAC-ELD / 44.1 kHz ステレオ）を復号し、WASAPI でスピーカー出力
- **ペアリング不要**: feature bit 27 を切ってあるため、Mac 側での PIN 入力なしに接続できます
- **スリープ防止**: 起動中は Windows のスリープ/画面オフを抑制

## 使い方

1. `lazyplay.exe` を起動すると全画面で開きます（`-window` でウィンドウ起動）
2. 送信側とWindows PCを、信頼できる同じローカルネットワークに接続します。
   - **iPhone / iPad**: コントロールセンター →「画面ミラーリング」→ `lazyplay-display`
   - **Mac**: コントロールセンター →「画面ミラーリング」→ `lazyplay-display`
   Windowsのウィンドウに画面が映り、対応する音声はPCで選択中の出力デバイスから再生されます。
3. タップ / 右クリック / タッチ長押しでコントロールメニュー（Toggle fullscreen / Move to next display / Audio output / Exit）が開きます。終了はメニューの Exit または `Esc`/`Q` でも可能
4. `Alt+Enter` で全画面/ウィンドウ切替、`Shift+Alt+Enter` で全画面を次のディスプレイへ移動

**ファイアウォール**: AirPlay は内向き TCP 5000/7000（と mDNS UDP 5353）を使います。
初回起動時に Windows ファイアウォールの許可ダイアログが出たら許可してください。
Mac/iPhone 側にデバイスは見えるのに接続だけできない場合はファイアウォールの設定を確認してください。

**推奨**: コントロール パネルの「Windows Defender ファイアウォール」→「許可されたアプリ」
→「別のアプリの許可」から `lazyplay.exe` を追加してください。

### 接続・再生で困ったとき

| 症状 | 確認すること |
|---|---|
| `lazyplay-display`が見つからない | lazyplayを起動したまま、両端末を信頼できる同じLANに接続します。ゲストWi-Fiの端末間通信制限、VPNの経路、mDNSの遮断も確認してください。 |
| 名前は見えるが接続できない | 起動中の`lazyplay.exe`をWindowsファイアウォールの信頼できるプライベートネットワークで許可します。名前が見えても配信ポートが通るとは限りません。ファイアウォール全体は無効にしないでください。 |
| 接続しても画面が黒い | 起動ログの`Hardware decoder unavailable`、D3D11/H.264ハードウェアデコード対応、GPUドライバを確認します。保護コンテンツは送信側で黒くなる場合があります。 |
| 画面は映るが音が出ない | 送信側とWindowsの音量・ミュート状態、Windowsで選択中の再生デバイスを確認します。コントロールメニューのAudio outputで特定デバイスに固定している場合は、そのデバイスが接続されているか確認してください。音楽だけのAirPlay送信先ではなく「画面ミラーリング」を使ってください。 |
| 再生が途切れる | LANの電波状態・混雑を確認します。現在の接続を終了した後、`-res 720p -fps 30`で再起動して試せますが、改善を保証するものではありません。 |

PINペアリングやアクセス制御は実装していません。公共・ゲストネットワークを避け、信頼できるLANだけで使ってください。
不具合や動作した環境は、[Issue](https://github.com/kuwa72/lazyplay/issues/new/choose)から報告できます。
受信側のバージョン、送信側の機種・OS、Windowsのビルド、GPU、試したことを添えてください。
スクリーンショットやログの個人情報は、共有前に取り除いてください。

### コマンドラインオプション

| オプション | 説明 | デフォルト |
|---|---|---|
| `-name <name>` | AirPlay 上の表示デバイス名 | `lazyplay-display` |
| `-fps <30\|60>` | 最大フレームレート | `30` |
| `-res <720p\|1080p>` | 受信解像度 | `1080p` |
| `-vsync <0\|1>` | 垂直同期 | `1` |
| `-window` | 全画面ではなくウィンドウで起動 | off（全画面がデフォルト） |
| `-console` / `--console` | コンソールを割り当てて診断ログを表示 | off（コンソールなし） |

* 描画は Per-Monitor-V2 DPI aware のため、Windows の表示スケーリング（125% 等）下でも
  物理ピクセルに 1:1 で描画されます。デフォルトの全画面起動でパネル解像度と一致すれば
  ドットバイドット表示になります（ウィンドウモードでは枠・タイトルバー分だけ領域が減ります）。
* キーボード無しのタブレット運用を想定: タップ / 右クリック / タッチ長押しでコントロールメニューを
  開きます。全画面時はマウスカーソルも非表示になります。
* コントロールメニューの **Audio output** で音声の出力先を選べます。
  *System default* はWindowsの既定デバイスに追従し、それ以外の項目はそのデバイスに
  固定します（取り外すと無音になり、再接続で自動復帰します）。選択は次回起動時も保持されます。
* `lazyplay.exe` はコンソールなしで起動します。ログは `-console` で表示できます。
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
- iOS / iPadOS / macOSからの画面ミラーリング（同じローカルネットワーク、mDNSへの到達が必要）
- 下記のWindows PCで、iPhoneからの画面・音声キャストの動作報告があります。iPhoneの機種とiOSバージョンは未記録です。
- Macの拡張ディスプレイ表示や、ほかの機種・OSの組み合わせは、このiPhone実例では検証していません。

## 実測したCPU・メモリ使用量

2026年10月5日、利用者がiPhoneの画面・音声をキャスト中と確認した状態で、
実行中のプロセスを30.134秒間、29サンプル測定しました。
PCはASUS ROG Zephyrus G14、Ryzen 9 5900HS、16論理プロセッサ、メモリ31.4GiB、
Windows 11 Homeビルド26200です。OSが報告するGPUはAMD Radeon Graphicsでした。

| プロセスの指標 | 平均 | 測定中の最大 |
|---|---|---|
| CPU使用率（PC全体を100%とする） | 0.463% | 1.124% |
| ワーキングセット（物理メモリ上の使用量） | 79.33MiB | 79.73MiB |
| プライベートバイト（プロセス専用の確保済みメモリ） | 105.31MiB | 105.83MiB |

短時間の1回の観測です。Atom端末での性能や、ほかのアプリとの比較は検証していません。
実行中のバイナリのリリースバージョン、送信側の機種・iOSバージョン、実際の受信解像度・FPSは未記録です。
遅延・実FPS・GPU使用率・VRAM使用量も未測定です。この観測では、要件定義にあるメモリ50MB以下の目標には達していません。
追跡用に[測定データとバイナリのハッシュ](assets/windows-mirroring-2026-10-05.json)を残しています。

自分の環境で測る場合は、Windowsの信頼できるローカルチェックアウトから、受信中に次を実行します。

```powershell
powershell.exe -NoProfile -File .\scripts\Measure-Lazyplay.ps1 -Seconds 30 -Workload mirroring
```

待機時は`-Workload idle`で別に測定してください。複数起動している場合は`-ProcessId <id>`で指定します。
スクリプトはプロセス・システム情報を読み取り、JSONを出力するだけです。アプリの再起動や実行ポリシーの変更は行いません。
未署名スクリプトやネットワーク共有パスがポリシーで制限される場合は、制限を緩めず、普段承認されている実行方法を使ってください。

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
