# 管子特性曲線（Koren 參數擬合用）

每支管子一個資料夾。屏極特性曲線從原廠 datasheet 數位化（`verify/digitize_curves.py` 自動擷取，失敗時用 WebPlotDigitizer 手動），`verify/koren_fit.py` 以最小平方法擬合 Koren 參數，結果寫入 `fit.json`。Koren 已發表的參數只做交叉比對，不放進程式或預設檔。

| 檔案 | 內容 |
|---|---|
| `source.json` | datasheet 出處（廠商、文件、URL、頁碼、圖）、模型（`triode` / `pentode`）、圖的座標軸上限（`plot`）、要比對的工作點（`operating_points`）、五極管的 Vg2 |
| `digitize.json` | 自動擷取的設定：頁碼、圖在頁面影像上的範圍、框線兩端的刻度值、格距、由左到右各曲線的柵壓 |
| `vg<Vg>.csv` | 一條柵壓曲線。檔名例：`vg0.csv`、`vg-40.csv`、`vg+5.csv` |
| `vg<Vg>_vg2<Vg2>.csv` | 五極管在另一個簾柵電壓下的曲線（未寫時用 `source.json` 的 `vg2`） |
| `overlay.png` | 自動擷取的點與校正後的格線畫在原圖上，供目視確認 |
| `fit.json` | `koren_fit.py` 的輸出：參數、整體與每條曲線的誤差、工作點的 μ / rp / gm 與 datasheet 的差 |

## CSV 的單位

**第 1 欄：屏極電壓，單位 V。第 2 欄：屏極電流，單位 mA。**

- 第一行可以是標題 `Ep_V,Ip_mA`（自動擷取一律寫這一行）。標題寫成別的單位（例如 `Ip_A`）時，擬合腳本拒絕讀取。
- WebPlotDigitizer 匯出沒有標題時，腳本以 `source.json` 的 `plot`（圖的 x、y 軸上限，V 與 mA）檢查每個檔：最大值超過軸上限 5% 以上、或不到軸上限的 2%，就當成單位錯誤並停止（用 A 或 kV 匯出會差 1000 倍）。
- 逗號、分號、tab 分隔都可以。

## 數位化的規則

- 只取 datasheet 畫出來的範圍，不外插；超過屏極耗散的部分照原圖取。
- 雙三極管（6SN7、ECC83）取一個單元的曲線。
- 填入 `source.json` 的 `page` 與 `figure`，讓每個點都能追溯到原圖。
- 掃描檔本身不進版控：`digitize_curves.py` 把 PDF 下載到 `.cache/`。
