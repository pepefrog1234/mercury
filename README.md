# Mercury fork — 短波數據機

[English](README.en.md)

這個儲存庫是 `pepefrog1234/mercury` 維護的 Mercury fork。它是從 Rhizomatica 的 HERMES Mercury 數據機專案 fork 而來，主要用於 Mercury Chat 與短波數位通訊測試。

本 fork 不是 Rhizomatica 官方版。若要和 Mercury Chat 搭配使用，建議雙方都使用這個 fork 或相容版本；本 fork 已修改 ARQ 協定與 TNC 行為，不能保證能和原版 Mercury 或其他未同步修改的版本互通。

## Fork 來源

- 上游專案：Rhizomatica Mercury
- 上游網址：https://github.com/Rhizomatica/mercury
- Fork 分支：`mercuryv2`
- 本 fork 分支：`mercuryv2`
- 目前 fork 分岔點：上游 `mercuryv2` 的 `f308d5a`，commit 訊息為 `Add workflow to automate release process`

Mercury 本身是 HERMES 專案的一部分，由 Rhizomatica 開發。原始 Mercury v2 是以 C 重寫的短波數據機，提供 FreeDV DATAC 模式、ARQ 資料鏈路、廣播/信標、VARA 相容 TCP TNC 介面，以及 Hamlib / HERMES 電台控制。

## 這個 fork 改了什麼

### ARQ 協定與鏈路效率

- 導入 ARQ v5 空中協定的 burst DATA / cumulative ACK：
  - DATAC4 維持單訊框傳輸。
  - DATAC3 最多可在一次 PTT 中送 2 個 DATA 訊框。
  - DATAC1 最多可在一次 PTT 中送 3 個 DATA 訊框。
  - ACK 使用 cumulative ACK，遺失時只重送尚未確認的 burst 尾端。
- 改善半雙工 turn-taking：
  - 調整 ACK 後等待與 TURN_REQ timing，降低雙方互踩與過早切換風險。
  - IRS 有本機待傳資料時，不會太快打斷對方，先給對方完成傳輸與 ACK 的空間。
- 放寬閒置與 keepalive 行為：
  - 將 IRS inactivity probe 調整為較適合人工聊天的時間尺度。
  - 放寬 keepalive / idle disconnect 判斷，降低雙方仍在線但短時間未互動就斷線的機率。
- 新增 ARQ 佔空比與 timing telemetry，用於觀察 PTT airtime、ACK 等待與酬載速率。

### 自適應速率與頻寬規則

- ARQ 酬載使用 DATAC4 / DATAC3 / DATAC1 階梯式資料模式：
  - DATAC4：最保守，約 87 bps，酬載 54 位元組。
  - DATAC3：中速，約 321 bps，酬載 126 位元組。
  - DATAC1：最快，約 980 bps，酬載 510 位元組。
- DATAC13 固定用於控制訊框，例如 CALL、ACCEPT、ACK、TURN、KEEPALIVE、DISCONNECT、CQ，約 65 bps，酬載 14 位元組。
- 寬頻寬連線不再長時間卡在 DATAC4：
  - 允許短聊天酬載在 SNR 已知後升到 DATAC3。
  - DATAC1 需要乾淨 ACK 歷史與足夠 SNR / 待傳資料量才會啟用。
  - 發生重送或不穩定時會降速，並保留一段維持時間避免反覆跳速。
- 500 Hz / 2300 Hz / 2750 Hz 規則更明確：
  - `BW500` 嚴格維持窄頻寬酬載，固定使用 DATAC4；控制與信標仍使用 DATAC13。
  - `BW2300` 和 `BW2750` 允許 DATAC4 → DATAC3 → DATAC1 自適應切換。
  - `BW2750` 會保留在 CALL / ACCEPT / CONNECTED 回報中，方便 VARA 相容使用者辨識。
- A→B 與 B→A 的模式判斷彼此獨立，因此雙方 GUI 看到的 TX/RX 速率可能不同。

### TNC 介面與狀態回報

- 保留 VARA-style TCP TNC 架構：
  - 控制埠：預設 `8300`。
  - 資料埠：預設 `8301`。
  - 廣播/信標埠：預設 `8100`。
- 新增或強化控制命令：
  - `TXGAIN <0..200>`：即時設定 TX 音訊輸出音量百分比，`100` 為 unity gain。
  - `CALLINT <seconds>`：調整 CALL / ACCEPT 重試間隔，`0` 可還原預設值。
  - `BW2750`：接受並保留 2750 Hz token。
  - `BUFFER`、`BITRATE`：保留可查詢狀態。
  - `COMPRESSION ON/OFF`：為 VARA 相容客戶端保留的 no-op 命令。
- 新增 `TXBITRATE (<level>) <bps> BPS` 非同步狀態，讓客戶端能分開顯示本機 TX 速率與對方 RX 速率。
- `BITRATE (<level>) <bps> BPS` 會回報目前收到的酬載模式速率。

## 速率表

以下速率是 FreeDV DATAC 模式的名目位元速率，不等於聊天文字實際淨吞吐量。實際吞吐量還會受到 ARQ header、ACK、turn-taking、PTT 延遲、重送與 UTF-8 文字位元組數影響。

| TNC 顯示 | FreeDV 模式 | 名目速率 | 每個數據機訊框酬載 | 用途 |
| --- | --- | ---: | ---: | --- |
| `L1` | DATAC1 | 約 980 bps | 510 位元組 | 高 SNR、寬頻寬、大 backlog 的最快資料模式 |
| `L3` | DATAC3 | 約 321 bps | 126 位元組 | 寬頻寬聊天常見的中速資料模式 |
| `L4` | DATAC4 | 約 87 bps | 54 位元組 | 窄頻寬與不穩定鏈路的保守資料模式 |
| 控制 | DATAC13 | 約 65 bps | 14 位元組 | CALL、ACCEPT、ACK、TURN、KEEPALIVE、DISCONNECT、CQ |

## 音效與跨平台修正

- 修正 macOS CoreAudio 裝置解析：
  - 可用裝置名稱解析到實際 CoreAudio 裝置 ID。
  - 改善 A/B 本機回環、BlackHole、AetherSDR 類音效裝置設定時的可用性。
- 修正音效回環 sample handling，避免 sample 格式與取樣率處理造成波形不正確。
- Windows 音效裝置名稱解析：
  - WASAPI 可接受易讀裝置名稱，並轉成 MMDevice ID。
  - DirectSound 可接受易讀裝置名稱，並轉成 GUID。
  - 避免 CAT/PTT 正常但音訊沒有真正送到指定音效卡的情況。
- TX 音訊輸出音量控制：
  - 命令列新增 `-Y <0..200>`。
  - INI 設定新增 `tx_audio_gain_percent`。
  - TNC 控制命令新增 `TXGAIN <0..200>`，可在不重啟數據機的情況下即時調整輸出音量。

## 建置與打包修正

- 修正 macOS build portability。
- `Makefile` 支援使用設定的 archiver，改善 FreeDV 靜態函式庫打包。
- 新增 fork 用 GitHub Actions：
  - Debian 13.5 amd64 建置、測試、打包。
  - Windows x64 MinGW 交叉編譯與 zip 打包。
- Windows zip 會包含 `mercury.exe`、`mercury.ini.example` 與 Hamlib 相關 DLL。

## 基本使用

列出音效裝置：

```sh
./mercury -z
```

列出 FreeDV 模式：

```sh
./mercury -l
```

啟動數據機，使用 CoreAudio、指定輸入/輸出裝置、設定 TNC 基礎連接埠：

```sh
./mercury -x coreaudio -i "<input-device>" -o "<output-device>" -p 8300 -b 8100
```

設定 TX 音訊輸出音量為 80%：

```sh
./mercury -Y 80
```

更多啟動參數可用：

```sh
./mercury -h
```

## 設定檔

Mercury 會讀取 INI 格式設定檔。預設為目前目錄下的 `mercury.ini`，也可使用 `-C <path>` 指定。命令列參數優先於設定檔。

範例設定請看：

- [mercury.ini.example](mercury.ini.example)

## 相關文件

- [ARQ 架構與協定說明](docs/ARQ.md)
- [TNC 指令參考](docs/TNC.md)
- [原始 Rhizomatica Mercury](https://github.com/Rhizomatica/mercury)
- [Mercury Chat](https://github.com/pepefrog1234/mercury-chat)

## 授權

本 fork 延續上游授權。請參考：

- [LICENSE](LICENSE)
- [LICENSE-freedv](LICENSE-freedv)
