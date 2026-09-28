# stock_watch — A股行情监看小电视

为板子 **ESP32-S3-LCD-1.3**（ESP32-S3R8 + 240×240 ST7789VW + QMI8658 IMU + WS2812 + PL4054 + CH343）写的一个 A 股行情看盘固件，架构复用自 `small_tv`（同一块板、同一套 LVGL 9 + `esp_lvgl_port` + App 管理器）。

## 功能

- **第 1 页 · A股大盘**：上证指数 / 深证成指 / 创业板指 / 沪深300 的点位、涨跌幅、涨跌额。
- **第 2~5 页 · 个股**：由 SD 卡 `/stock/code.txt`（一行一个股票代码）动态生成，每页显示
  - 名称 / 代码
  - 现价 + 涨跌额 / 涨跌幅（红涨绿跌）
  - 今开 / 最高 / 最低 / 昨收 / 换手率
  - 最近 40 根日 K 线蜡烛图
- **定时刷新**：行情 30s 一轮，日 K 线 5min 一轮。
- **页面上限 5 个**：1 页大盘 + 最多 4 只个股（`code.txt` 里超出 4 个的代码会被忽略）。
- 倾斜翻页（QMI8658，相对**运行时自动跟踪的参考姿态**倾斜 > 22° 左/右切换；短促摇一摇 = 下一页），底部圆点指示当前页。

> **完整文档（结构、启动流程、数据源、每一处修改的位置与原因）见 [`项目说明与修改记录.md`](项目说明与修改记录.md)**，
> 其中第 10 章是"开机姿态导致手势不灵敏/失效"的专项分析与修复。

## 数据源（免费接口，无需鉴权，UTF-8 JSON）

| 数据 | 接口 |
|---|---|
| 大盘指数批量 | `http://push2.eastmoney.com/api/qt/ulist.np/get?secids=...` |
| 个股实时 | `http://push2.eastmoney.com/api/qt/stock/get?secid=...` |
| 个股日K线（主源） | `http://push2his.eastmoney.com/api/qt/stock/kline/get?secid=...&klt=101&fqt=1&lmt=40` |
| 个股日K线（兜底） | `https://web.ifzq.gtimg.cn/appstock/app/fqkline/get?param=<sym>,day,,,80,qfq` |

说明：东财 `push2his` 是 http、**不跳转**，实测最稳，故作为主源；
腾讯的 http 入口会 **302 跳到 https**（`esp_http_client` 默认跟随重定向），
所以兜底直接用 https，并在 `net.c` 里挂了 `esp_crt_bundle_attach`（需 `CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=y`）。

## SD 卡准备

把 Micro-SD 卡（FAT32）插进板子，卡里放一个股票代码清单：

```
/stock/code.txt
```

内容示例（一行一个，支持 `600000`、`sh600000`、`SZ000001` 等写法，`#` 开头为注释）：

```
# 浦发银行
600000
# 贵州茅台
sh600519
# 宁德时代
sz300750
# 中国平安
601318
```

## 编译 & 烧录（ESP-IDF v5.4+，Windows/VSCode 或命令行）

```bat
:: 首次
idf.py set-target esp32s3
idf.py build

:: 烧录 & 看串口
idf.py -p COMx flash monitor
```

第一次编译会通过 `idf_component.yml` 从组件注册中心拉取 `lvgl/lvgl ^9.2` 与 `espressif/esp_lvgl_port ^2.2`。国内环境拉不下来时：

```bat
set IDF_COMPONENT_REPO=https://idf.espressif.com/registry
idf.py reconfigure
```

两个注意点：

- **换过芯片/板子一定要 `idf.py set-target esp32s3`**：`build/` 里残留别的 target 的
  sdkconfig/CMake 缓存会报一堆莫名错误。
- 根 `CMakeLists.txt` 里有一段 **GCC ICE 绕过**（把 `esp_lcd_panel_rgb.c` 单独编成 `-O2`）。
  本板是 SPI 屏、用不到 RGB 驱动，但 esp32s3 目标会把它带进编译，
  而 `xtensa-esp-elf` GCC 14.2 编它会段错误。IDF 5.5.4 需要这段，5.5.5 不需要但留着无害；
  升级 IDF 后可删。详见 `项目说明与修改记录.md` 9.4。

## WiFi

修改 `main/app_config.h`：

```c
#define WIFI_SSID  "HT"
#define WIFI_PASS  "kyjk123456"
```

## 目录结构

```
stock_watch/
├─ CMakeLists.txt               # 工程定义 + esp_lcd_panel_rgb.c 的 GCC ICE 绕过
├─ partitions.csv               # factory 4M + spiffs 12M
├─ sdkconfig.defaults           # ESP32-S3R8 + PSRAM + LVGL 字体 + TLS bundle
├─ README.md                    # 本文件(快速上手)
├─ 项目说明与修改记录.md         # 完整文档: 结构/流程/每一处修改的位置与原因
└─ main/
   ├─ app_main.c                # 启动 + 动态注册个股页 + 行情刷新任务
   ├─ app_config.h              # 引脚 & WiFi & 刷新周期 & 页数上限
   ├─ idf_component.yml
   ├─ screen/                   # ST7789 + LVGL 端口
   ├─ hw/                       # 电池 + WS2812 + QMI8658 + SD
   ├─ net/                      # WiFi + NTP + 通用 HTTP GET
   ├─ stock/                    # 行情数据层(东财/腾讯解析)
   ├─ appman/                   # HoloCubic 式 App 管理器(带 ctx 多实例)
   └─ apps/                     # app_market 大盘 + app_stock 个股 + 中文字体
```

## 中文字体

`apps/app_font_cn_16.c` 是**全量 GB2312（6763 字）**的 16px 4bpp 字体（含全角 ASCII），
用 `lv_font_conv` 从 SourceHanSansSC 生成，保证任意 A 股名称（浦发银行、贵州茅台等）都能显示。
重新生成命令见文件头部注释。编译后约 793KB，故 app 分区扩到 4M。
