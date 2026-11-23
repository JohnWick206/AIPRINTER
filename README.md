# AIPRINTER

> **2026-11-23 起本项目停止开发，转交接手。请先读 [HANDOVER.md](HANDOVER.md)**——那里有当前真实状态、三个遗留故障和图集格式规范。本文以下内容描述的是早期"串口生图"阶段，已不是最新形态。

### 本仓库**故意**不包含的两个文件

克隆后会发现 `demo/v5-grok.html` 跑不起来、`main/grokbot.c` 没有图集可读，这是有意的：

| 缺什么 | 为什么 | 怎么补 |
|---|---|---|
| `demo/grok-ref/` | 第三方角色引擎（blessonism/grok-icon-study），原作者明确要求**勿再分发** | 自己从该仓库拉取，原样放进 `demo/grok-ref/` |
| `main/grokbot.atlas` | 4.4 MB 派生数据，由上面的引擎烘焙而成 | 装好 `tools/grokbot` 工具链后 `node bake.mjs` 重新生成 |

其余代码、文档、工具链、三个 demo 页面均完整可用。

串口输入一句提示词 → 云端文生图 → 下载解码 → 显示到 SPI 小屏。

一个 ESP-IDF 工程，没有 Arduino 层。核心难点不在"能出图"，而在**小屏、弱内存、强限流**这三个约束下把链路做稳。

> 本文档面向**接手的 AI / 开发者**：读完应当能直接定位改动点、复现构建、判断故障属于哪一层。第 8、10 节是踩过的坑和操作约定，改动前务必看。

- 仓库：https://github.com/JohnWick206/AIPRINTER
- 分支：`main`，首次提交 `2dbf356`
- IDF：v5.5.1，目标 `esp32s3`

---

## 1. 功能边界

**做**：串口收一行提示词，调云端出图，等比缩放后整屏显示。
**不做**：本地图片、多轮对话、屏幕触摸交互、历史记录持久化。

当前屏只有 160×80，所以界面被压到极简：一行状态 + 一条进度条 + 一行小字。

---

## 2. 硬件

| 项 | 型号 / 参数 |
|---|---|
| 开发板 | 正点原子 DNESP32S3M（核心模组 ATK-MWS3S） |
| SoC | ESP32-S3，双核 240 MHz |
| Flash | 16 MB，QIO 80 MHz |
| PSRAM | 8 MB，**Octal** 模式 80 MHz |
| 屏 | ST7735，160×80，RGB565，SPI |
| 屏显存偏移 | `(1, 26)` —— 面板可视区在 132×162 显存中的起点 |
| 屏方向 | `MADCTL = 0xA8`（MY\|MV\|BGR），横屏、红蓝正确 |
| 串口 | 芯片内置 **USB-Serial-JTAG**，枚举为 COM4（`VID_303A&PID_1001`） |

### 引脚（`main/display.c`）

| 信号 | GPIO | 说明 |
|---|---|---|
| SCK | 12 | 模块 IOMUX 的 `FSPICLK` |
| MOSI | 11 | 模块 IOMUX 的 `FSPID` |
| CS | 39 | 模块丝印 `LCD_CS` |
| DC | 40 | 模块丝印 `LCD_DC` |
| RST | 38 | 模块丝印 `LCD_RST` |
| BL | 41 | 模块丝印 `LCD_BL`，高电平点亮 |

> 这 6 根是模组原理图上**专门分配给 LCD 的脚**，不是随便挑的。
> 板载 LCD 排座如果插着屏，会和外屏共用 CS，两块屏同时被选中导致双方花屏。

### 绝对不要用的引脚

| 引脚 | 原因 |
|---|---|
| IO35 / IO36 / IO37 | 被 Octal PSRAM 占用，接了必挂且报错难查 |
| IO0 | 启动模式，拉低进下载模式 |
| IO45 / IO46 | strapping，影响 VDD_SPI 和启动日志 |
| IO3 | JTAG 选择脚，上电采样 |
| IO19 / IO20 | USB D-/D+ |
| IO2 | 接了板载 TF 卡座（`TF_CS`） |

---

## 3. 数据流

```
串口一行 UTF-8
   │
   ▼
input_task            main.c
   │  过滤控制字符 / 退格 / \r\n
   ▼
run_once(prompt)      main.c
   │  ① 冷却检查（30 s）
   │  ② json_escape：\" \\ 转义，丢控制字符，UTF-8 原样透传
   │  ③ 拼请求体：prompt + IMG_STYLE（固定风格后缀）
   ▼
http_post_json        http.c         → POST api.siliconflow.cn
   │  返回 images[0].url（预签名，约 1575 B）
   ▼
http_get_all          http.c         → GET S3 预签名地址
   │  整块读进 PSRAM（实测 240~390 KB）
   ▼
image_decode_fit      image.cpp      → 面积平均等比缩放
   │  输出紧凑 RGB565 **大端**（即 LV_COLOR_16_SWAP=1 的内存布局）
   ▼
ui_show_image         ui.c           → lv_img，TRUE_COLOR
   │
   ▼
LVGL 渲染 → s_buf（全屏 draw buffer）
   │
   ▼
flush_cb              display.c      → set_window + SPI DMA 整块推屏
```

### 状态机

| 阶段 | 屏上文字 | 进度 | 屏色（历史） |
|---|---|---|---|
| 上电 | 启动 | — | — |
| 连 WiFi | 连接中 | — | — |
| 就绪 | 就绪 | — | — |
| 请求生成 | 生成中 | 20 | — |
| 下载 | 下载中 | 55 | — |
| 解码 | 解码中 | 82 | — |
| 完成 | （出图，隐藏文字） | — | — |
| 失败 | 出错 + 原因 | — | — |
| 限流 | 限流 | — | — |
| 冷却中 | 等待 + 剩余秒 | — | — |

> 早期版本用纯色填屏表示阶段，已废弃。现在背景恒为黑，反馈靠文字。

---

## 4. 目录结构

```
net_speed/
├── CMakeLists.txt              项目入口
├── partitions.csv              自定义分区表（默认 1MB 装不下 PNGdec）
├── sdkconfig                   含 LVGL 全部 Kconfig 配置
├── sdkconfig.defaults          PSRAM/Flash/控制台等的可复现基线
├── components/
│   ├── lvgl/                   submodule → github.com/lvgl/lvgl @ b0c4cb3 (v8.4.1)
│   └── pngdec/                 第三方 PNG 解码（C++）
└── main/
    ├── main.c      (290) 主流程：WiFi、限流、run_once、串口输入任务
    ├── display.c   (182) ST7735 驱动 + LVGL 显示驱动注册 + 刷新任务
    ├── ui.c        (112) 界面对象：状态标签 / 进度条 / 提示小字 / 图片
    ├── image.cpp   (166) PNG 解码 + 面积平均缩放（C++，对外 C 接口）
    ├── http.c      (153) HTTPS POST / GET
    ├── app.h        (52) 模块间接口（C/C++ 混编，全部 extern "C"）
    ├── config.h           ★ 本地凭据，已 gitignore
    ├── config.h.example   占位符模板
    ├── font_cn_24.c      lv_font_conv 生成的 24px 4bpp 中文字模
    ├── font_cn_16.c      16px 4bpp
    └── font_cn.h         旧 32×32 1bpp 点阵，已被上面两个取代，当前无任何引用
```

### 文件职责边界

- **`display.c`** 只管"怎么把像素送到玻璃上"，不含任何业务逻辑。换屏只改这一个文件。
- **`ui.c`** 只管"屏幕上有哪些控件、显示什么文字"。不含网络和时序。
- **`main.c`** 是唯一的流程编排者，串行阻塞式：一次生成走完才回提示符。

---

## 5. 关键参数

### 编译期（`main/config.h`）

| 宏 | 当前值 | 含义 / 改动影响 |
|---|---|---|
| `WIFI_SSID` / `WIFI_PASS` | — | **本地凭据，不入库** |
| `API_KEY` | `sk-...` | 硅基流动密钥，**不入库** |
| `API_URL` | `.../v1/images/generations` | 换供应商时改这里 |
| `MODEL_NAME` | `Kwai-Kolors/Kolors` | 换模型需同步改 `IMG_SIZE`（各模型支持的分辨率不同） |
| `IMG_SIZE` | `480x640` | 竖构图 3:4。2.4" 屏（240×320）正好 1/2 |
| `IMG_STEPS` | `20` | 推理步数，越高越慢越好 |
| `IMG_STYLE` | 65 汉字 / 191 B | **无条件拼在用户提示词后面** |
| `DEFAULT_PROMPT` | `a cute orange cat...` | 仅作为「就绪」状态的小字提示 |
| `MIN_INTERVAL_S` | `30` | 客户端硬限速，见下节 |

### 运行时常量

| 位置 | 值 | 说明 |
|---|---|---|
| `display.c` | `SPI_PCLK_HZ = 20 MHz` | ST7735 时序下限 66 ns≈15 MHz，20 MHz 留了余量 |
| `display.c` | `lvgl_task` 优先级 4，栈 8192 | `vTaskDelay(1)` —— **不能写 `pdMS_TO_TICKS(5)`**，见第 8 节 |
| `sdkconfig` | `LV_MEM_SIZE_KILOBYTES=32` | LVGL 内部堆 |
| `sdkconfig` | `LV_COLOR_16_SWAP=y` | LVGL 直接按大端写内存，flush 时零拷贝、不做字节交换 |
| `sdkconfig` | `LV_DISP_DEF_REFR_PERIOD=20` | 20 ms 刷新周期 |
| `main.c` | `line[256]` | 提示词上限 **255 字节**，超出静默丢弃 |

### 请求体结构

```json
{"model":"Kwai-Kolors/Kolors","prompt":"<用户提示词><IMG_STYLE>","image_size":"480x640","num_inference_steps":20}
```

固定开销 90 B + `IMG_STYLE` 191 B + 转义后的提示词。
提示词最多 255 B → 转义最坏 510 B → 总计 ≤ 791 B，缓冲 1024 B 够用，`body too long` 分支实际走不到。

---

## 6. 串口协议

波特率 115200，8N1，无流控。

| 输入 | 行为 |
|---|---|
| `<文本>` + 回车 | 生成。`\r` 和 `\r\n` 都认（会吃掉 `\r\n` 的后半截） |
| 空行 | 打印 `(empty line ignored)` |
| `retry` / `again` | 重发上一条提示词 |
| 退格 `0x7F` / `0x08` | 擦除一个字符 |
| 其它 `< 0x20` | 忽略（挡方向键、Tab 的转义序列） |

就绪时打印：

```
==============================================
 READY - type a prompt, then press Enter
 commands: retry = resend last prompt
 quota: 2 images/min, 30 s between requests
==============================================
>
```

> **所有日志都是纯 ASCII**。固件输出 UTF-8，而 Windows 控制台默认 GBK，中文日志会显示成乱码。
> 屏幕上的中文走 LVGL 字模，和串口日志无关。

---

## 7. 构建与烧录

```bash
# 首次
cp main/config.h.example main/config.h    # 填 WiFi 和 API Key
git submodule update --init --recursive   # 拉 LVGL，忘了会报找不到 lvgl.h

# 编译
idf.py build

# 烧录（COM4 是芯片内置 USB-Serial-JTAG）
idf.py -p COM4 flash

# 监视
idf.py -p COM4 monitor      # 退出用 Ctrl + ]
```

**资源占用**（实测）：

| 项 | 数值 |
|---|---|
| 固件 | 1,270,256 B ≈ 1.21 MiB |
| app 分区 | 4 MB，剩余 75% |
| 内部 RAM（WiFi+LVGL 运行中） | 96,911 B 空闲 |
| PSRAM | 8.4 MB 空闲 |
| 全屏推屏耗时 | 25.6 KB @ 20 MHz ≈ 12.8 ms |

`partitions.csv` 是自定义的，因为默认单 app 分区只有 1 MB，加 PNGdec 后只剩 10 KB，必须扩。

---

## 8. 已知限制与坑

按"踩坑成本"排序，改动前逐条确认。

### 8.1 限流是账户级的，换 key 无效

硅基流动图像模型 **IPM = 2**（每分钟 2 张）、**IPD = 400**（每天）。
超限返回 `HTTP 429` + `code 50604` + `IPM limit reached`。

- 限额挂在**账户**上，不是 API Key，多建 key 共享同一份额度
- 客户端已按 `MIN_INTERVAL_S = 30` 硬限速（60 ÷ 2）
- 命中 429 时屏幕显示「限流」，日志提示 `type: retry`

**注意**：一次粘贴多行文本会被逐行拆成多次独立请求，瞬间打满额度。这是最容易触发 429 的操作方式。

### 8.2 `pdMS_TO_TICKS(5)` 在 100 Hz tick 下等于 0

IDF 默认 `CONFIG_FREERTOS_HZ = 100`，`pdMS_TO_TICKS(5)` 向下取整为 **0**。

`vTaskDelay(0)` **不阻塞**，LVGL 任务变成忙等，饿死 CPU0 的 IDLE，5 秒后 task_wdt 报警，`app_main` 卡在 `ui_init()` 拿不到锁，串口连 `app:` 日志都不出。

**必须写 `vTaskDelay(1)`**（1 tick = 10 ms）。要 5 ms 精度得把 `CONFIG_FREERTOS_HZ` 改到 1000。

### 8.3 HTTP 响应缓冲必须每轮置零

`http.c` 的 `on_post_data` 累积完数据后要写 `s_resp[s_resp_len] = 0`。
漏了这行，错误日志的 `%.200s` 会**读到上一轮残留的响应**——表现为日志里冒出跟本次无关的 S3 预签名 URL 片段。

### 8.4 全屏 draw buffer 与字节序

- `s_buf` 是 `lv_color_t[LCD_W*LCD_H]`，同时是 LVGL 的绘制缓冲和 SPI 的源
- `LV_COLOR_16_SWAP=1` 让 LVGL 按大端写内存，正好是 ST7735 要的 MSB-first → **flush 时不做任何转换**
- `image_decode_fit` 输出的也是大端，所以能直接当 `LV_IMG_CF_TRUE_COLOR` 用
- 如果哪天把 `LV_COLOR_16_SWAP` 关掉，**红蓝或整屏颜色会立刻出错**，且图片和 UI 会同时坏

### 8.5 全屏缓冲放不进内部 RAM（换 240×320 屏时）

160×80×2 = 25.6 KB 放得下。但 240×320×2 = **153.6 KB**，内部 RAM 只剩约 95 KB，装不下。
换大屏时必须改成分块 draw buffer（如 240×40 = 19.2 KB），不能直接放大 `s_buf`。

### 8.6 构建时的假死现象

在 PowerShell 里这样写会**反向压住 ninja 的 stdout**：

```powershell
& idf.py build *>&1 | ForEach-Object { Add-Content -Path $log -Value $_ }
```

症状：gcc 进程 CPU 时间恒为 0、长时间没有新 `.obj`、看起来像链接卡死。
改用单次重定向 `& idf.py build *> $log` 即可。

### 8.7 Windows PowerShell 5.1 按 GBK 解析 `.ps1`

`.ps1` 文件里写中文（哪怕只是注释）会被拆坏，连带引号失配，报 `TerminatorExpectedAtEndOfString`。
**给这台机器写 `.ps1` 一律纯 ASCII。**

---

## 9. 演进方向

### 9.1 换 2.4" ST7789 240×320（已确定方案，未实施）

| 项 | 变化 |
|---|---|
| 分辨率 | 160×80 → 240×320，`VIS_X/VIS_Y` → 0/0 |
| 初始化 | ST7735 六条命令 → ST7789 约 15 条（`COLMOD=0x55`、porch 参数） |
| draw buffer | 全屏 → **240×40 分块**（19.2 KB，内部 RAM） |
| SPI | 20 MHz → 40 MHz（全屏 30.7 ms ≈ 30 fps） |
| 引脚 | **不变**，仍用 IO11/12/38/39/40/41 |

收益：出图尺寸 480×640 → 240×320 正好 1/2，面积平均变成精确 2×2 采样，画质明显提升；中文排版空间从 5 字/行 扩到 10 字/行。

### 9.2 其他候选

- **提示词超长告警**：255 B 截断现在是静默的，应打印 `prompt truncated`
- **`/raw` 命令**：跳过 `IMG_STYLE`，用于试写实/水彩等其它画风
- **中文提示词回显**：现在小字只支持字模子集内的汉字，自由中文会显示为空白
- **局部刷新**：状态文字变化时只推变化区域，而非整屏 25.6 KB

---

## 10. 给接手 AI 的约定

### 读代码顺序

`app.h`（接口全貌）→ `main.c` 的 `run_once`（主流程）→ `display.c` 的 `flush_cb`（像素怎么出去）→ `ui.c`（界面）→ `image.cpp`（缩放算法）。

### 改动前必须确认

1. **不要动 `main/config.h`** 的凭据格式，它被 `.gitignore` 排除，改动会破坏"克隆后填模板"的流程
2. **不要为了加功能而给 `run_once` 加并发**。当前是串行阻塞模型，`input_task` 在其中同步等待，这是刻意的简化；改成异步要同时处理 LVGL 锁和 PSRAM 生命周期
3. **改字体必须重新生成**，不能手写：`npx lv_font_conv --font simhei.ttf --size 24 --bpp 4 --format lvgl --no-compress --symbols "<汉字>" --range 0x20-0x7E -o main/font_cn_24.c`
   字模只含子集，新增文案要先补字符，否则屏上是空白
4. **`LV_COLOR_16_SWAP` 与图片字节序是绑定的**，改一个必须改另一个

### 验证手段（无法看到屏幕时）

```bash
# 1. 编译产物与体积
idf.py build          # 看 binary size，正常约 1.21 MiB（0x1361f0）

# 2. 启动是否正常（关键行）
idf.py -p COM4 monitor
#   应出现  lcd: ST7735 160x80 ... LVGL 8.4
#   应出现  app: IP x.x.x.x
#   应出现  app: heap internal <N>  psram <M>
#   不应出现 task_wdt / Guru Meditation

# 3. 端到端跑一轮
#   串口发 "a red car"，等约 6~8 秒
#   期望：POST 200 → download xxx KB → decoded 480x640 -> 60x80 → total x.xx s → 提示符回归

# 4. 脚本化发送（不依赖交互终端）
python -c "
import serial,time
s=serial.Serial('COM4',115200,timeout=0.2); s.dtr=False; s.rts=False
s.write(b'a red car\r'); time.sleep(8); print(s.read(4096).decode('utf-8','replace'))
"
```

### 常见故障定位

| 现象 | 大概率原因 |
|---|---|
| 串口无任何输出 | DTR/RTS 被拉高把板子按在复位态；按 RST 或放开两根线 |
| 只有 `lcd:` 没有 `app:` | `app_main` 卡在 `ui_init` 抢不到锁 → 查 `vTaskDelay` 参数（第 8.2 节） |
| 屏全黑但日志正常 | 背光 BL 没拉高；或 `MADCTL` 写错导致全黑 |
| 屏有图案但颜色不对 | `LV_COLOR_16_SWAP` 与图片字节序不匹配（第 8.4 节） |
| 出图红蓝反色 | 同上；或 `MADCTL` 的 BGR 位 |
| `POST 429` | 撞 IPM 限额，等 60 s 后 `retry` |
| `decode failed` | 服务端返回的不是 PNG；或 PSRAM 分配失败 |
| `task_wdt` 反复报 | 某处 `vTaskDelay(0)` 或长循环未喂狗 |

### 提交约定

- 凭据只存在于 `main/config.h`，永不提交
- 改 `sdkconfig` 后同步更新 `sdkconfig.defaults`，否则新克隆的构建结果不一致
- 提交信息用中文，说明**改了什么 + 为什么**，不要只写 "fix bug"
