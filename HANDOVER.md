# AIPRINTER 交接文档

> 写给接手的 AI / 工程师。写于 2026-11-23，项目在此日**停止开发**。
> 本文只记录**事实**与**已验证的结论**；凡是推测，都会明确标注「推测」。

---

## 0. 三十秒读完

这是一台**单按键、语音输入、云端生图、热敏打印**的掌上设备，硬件是 ESP32-S3 模组 + 3.2 寸 ST7789 竖屏。

代码能编译、屏幕能点亮、Grok 表情动画的**图集管线已经跑通并验证过**（53 个状态 / 4.40 MB，PC 端解码抽样正确），但**设备端播放器从未在真机上跑过一次**——最后一次编译被卡在构建系统死锁，项目就停在这里。

三个必须先解决的遗留问题：

| # | 问题 | 严重度 | 见 §6 |
|---|---|---|---|
| 1 | ESP-IDF 的 CMake **重新配置后卡死在 generate 阶段**（不是慢，是死锁） | 阻断编译 | 6.1 |
| 2 | 图集烘焙脚本偶发**卡在同一帧**（截图字节完全相同） | 阻断重烘焙 | 6.2 |
| 3 | 12 状态精简版图集**没做出来**，仓库里是 53 状态的 4.40 MB 版 | 不阻断，但占 flash | 6.3 |

---

## 1. 产品定义（这部分已经定稿，不要重新发明）

用户口述的流程，共四幕：

1. 用户**按住**唯一的按键 → 屏幕出现录音界面，说话内容逐字显示
2. **松手** → 播放加载动画（等待云端出图）
3. 图到了 → 转场，图片**铺满整屏**
4. 用户**轻触**按键 → 播放打印动画，热敏机出纸

**硬件约束：全机只有一个外部按键。** 这条约束决定了一切交互设计，早期版本按"两个键"做的方案全部作废。

**交互上已经死掉的方案，不要回头捡**：

- ❌ 用 0.25 / 0.7 / 1.2 秒三个阈值区分"录音、打印、取消、弃图"——需要解释的控件就是坏控件
- ❌ 百分比进度数字——云端是同步接口，数字是编的，慢的时候会反噬
- ❌ 独立按键的"打印"暗示——没有第二个键，一切认知来自屏幕

**当前定稿的单键语义**（`demo/v5-grok.html` 实现）：

| 状态 | 按下 | 松开 |
|---|---|---|
| 待机 / 完成 | 立刻进录音 | 不足 0.32 s 算误触；否则开始生成 |
| 生成中 | 不响应 | 不响应 |
| 出图 | 纸变暗（暗示要换掉它） | 不到 0.4 s = 打印；超过 = 换下一张 |
| 打印中 | **完全锁死**（纸已进仓，不能假装能取消） | — |

---

## 2. 硬件与接线

| 项 | 值 |
|---|---|
| 模组 | ESP32-S3，**Octal PSRAM 8 MB**，flash 按 **16 MB** 配置（`CONFIG_ESPTOOLPY_FLASHSIZE="16MB"`） |
| 屏幕 | **GMT032-8P，3.2 寸，ST7789，240×320 竖屏**，8 针 SPI 模块，无显存偏移 |
| SPI | 40 MHz（`SPI_PCLK_HZ`，`main/display.c:64`） |
| RTOS tick | `CONFIG_FREERTOS_HZ=1000`（**必须**，100 Hz 下 `pdMS_TO_TICKS(5)` 会取整为 0） |

接线（`main/display.c:39-48`，对应模块丝印）：

| 屏 | GPIO | 说明 |
|---|---|---|
| 3 SCL | GPIO14 | 开发板下排第 3 位 |
| 4 SDA | GPIO13 | 下排第 4 位 |
| 5 RES | GPIO12 | 下排第 5 位 |
| 6 A0/DC | GPIO11 | 下排第 6 位 |
| 7 CS | GPIO10 | 下排第 7 位 |
| BL 背光 | **直接接 3.3 V** | 不占 GPIO。背光要 20~60 mA，GPIO 驱动会拉垮电压导致全黑 |
| VCC / GND | 3.3 V / GND | |

板载那块小屏被主动禁用：`GPIO38`（板载 RST）拉低、`GPIO39`（板载 CS）拉高，永不选中。

> **踩过的最大一个坑**：曾经连续多次"全白"，最后万用表量出是 **CS 杜邦线内部断线**。屏幕全白且画面毫无变化时，**先量线通断**，不要先怀疑初始化序列。

---

## 3. 代码地图

```
net_speed/
├── build.ps1                 固定构建入口（纯 ASCII，必须保持）
├── partitions.csv            16MB 布局：app 4M @0x10000，atlas 6M @0x410000
├── HANDOVER.md               本文
├── main/
│   ├── app.h                 所有开关与模块接口（先看这个）
│   ├── main.c                业务主流程：串口输入 → 云端出图 → 显示；以及 demo 分支
│   ├── display.c             ST7789 驱动 + LVGL flush + 条带推屏（已充分验证）
│   ├── ui.c                  LVGL 状态文字 / 进度条 / 图片显示
│   ├── anim.c                DotField 点阵动画（旧方案，仅 demo）
│   ├── refine.c              RefineFrame 等待态（旧方案，仅 demo）
│   ├── grokbot.c / .h        ★ GrokBot 表情播放器（新，未上机验证）
│   └── config.h.example      WiFi / API Key 模板（真的 config.h 已 gitignore）
├── tools/grokbot/
│   ├── bake.mjs              浏览器逐帧截图 → PNG 序列（需 puppeteer-core）
│   ├── encode_atlas.py       PNG 序列 → GBZ1 图集（需 numpy + Pillow）
│   ├── check_atlas.py        PC 端解码图集抽帧，验证格式（自写，很有用）
│   └── node_modules/         已装好（53 MB），不用再 npm install
├── third_party/grokbot/
│   ├── grok-player.js        原仓库浏览器播放器（未改）
│   └── grok-recipes.json     状态 → 动画配方表
└── demo/                     交互原型（浏览器，非固件）
    ├── v5-grok.html          ★ 当前定稿的角色版原型
    ├── v4-line.html          Emil Kowalski 风格版（一根线贯穿四幕）
    ├── index.html            v0.3 单键四幕（点阵 + 百分比 + 半调转场）
    └── grok-ref/             第三方引擎副本，**已 gitignore，禁止入库/再分发**
```

**`main/display.c` 提供的三个关键接口**（`app.h`）：

```c
uint16_t *display_fb(void);                               // 整屏缓冲（PSRAM）
void display_present(void);                               // 整屏推给面板
void display_push_band(int y0, int h, const uint16_t *buf); // 条带推屏，可流水
void display_wait_flush(void);                            // 等上一次 DMA 完成
```

条带推屏是跑 30 fps 的关键：把绘制藏进 DMA 传输里。调用前必须持有 `display_lock()`。

---

## 4. 当前编译开关（`main/app.h`）

```c
#define LCD_W 240
#define LCD_H 320
#define SCREEN_TEST   0   // 1 = 只跑红绿蓝+彩条自检，验证接线
#define ANIM_DEMO     1   // 1 = 只跑动画，不连 WiFi、不读串口
#define GROKBOT_DEMO  1   // 1 = 播 GrokBot 表情（优先级最高）
#define DEMO_REFINE   1   // 仅 GROKBOT_DEMO=0 时有效
```

也就是说，**现在烧进去的行为是**：开机 → 初始化屏幕 → mmap 图集 → 53 个状态依次正放一遍，然后 2 秒后从头再来。**不是完整产品流程**，只是一个看动画的 demo。

要恢复到业务主流程：`ANIM_DEMO` 改 0。

---

## 5. 已验收 / 未验收

### 已验证（有实测依据）

- ST7789 240×320 点亮，颜色正确，无偏移
- LVGL 8.4.1 + 4bpp 抗锯齿中文字库正常显示
- 串口输入 → 云端（硅基流动 Kolors）→ 下载解码 → 上屏，全链路跑通过
- `display_push_band` 条带推屏与 DMA 流水正常（此前 240×320 上测到 25 fps）
- **图集格式与解码**：`tools/grokbot/check_atlas.py` 解开第 0 帧和第 60 帧，得到 240×240 灰度图，白色团子 + 黑底 + 双眼，无撕裂
- 图集烘焙整条链路**成功跑完过一次**：53 个状态 / 240×240 / 30 fps / 4.40 MiB

### 未验证（接手第一件事）

- `main/grokbot.c` 播放器**从未在真机跑过**。逻辑写完了（mmap 分区 → 逐帧 zlib 解压 → XOR 还原 → 4bit 展开 RGB565 → 条带推屏），但一次都没上机。
- 因此下列都不确定：实际帧率、是否有花屏、解压栈是否够（`tinfl` 结构 11 KB，任务栈已设 16 KB）、PSRAM 带宽是否够双缓冲。
- 分区表改动后 **bootloader 必须重烧**，这一步也没做过。

---

## 6. 三个遗留问题

### 6.1 构建系统死锁（最严重）

**症状**

- 只要 `partitions.csv` / `main/CMakeLists.txt` / `sdkconfig` 任一被改动，ninja 会先触发 CMake 重配
- 日志能走到 `-- Configuring done (357.1s)`，然后**完全停住**
- 实测 `cmake` 进程的 CPU 时间冻结在 2.8 s 不动，`ninja` 为 0 → **不是慢，是死锁在 generate 阶段**

**已排除**

- 不是磁盘满（D 盘 175 MB 构建目录，剩余充足；C 盘还有 31 GB）
- 不是 CPU 抢占（无其他编译进程，headless Chrome 已清理）
- 不是 git 卡住（`git status` 正常，`.git/index.lock` 不存在）

**推测原因**（未证实）

- Windows 下杀毒软件实时扫描 175 MB 的 `build/` 目录，generate 阶段反复 stat 大量文件被拖死
- 或此前被强制 kill 的 cmake 留下了半写的 scratch 文件

**建议做法（按优先级）**

1. **整体删掉 `build/` 目录，从零配置一次**（最可靠）。首次配置约 6~8 分钟，之后增量编译很快。删之前先确认磁盘够（`build/` 175 MB）。
2. 把 `D:\sticker\idf\net_speed` 加入 Windows Defender 排除目录，再试。
3. 若只想验证已有产物：当前 `build/build.ninja`（11-23 09:59）**已经包含 atlas 分区配置**，只是 mtime 比源码旧。用 `ninja -d explain` 确认它是否仍要重配。

**注意**：`build.ps1` 内部走的是 `ninja` 而不是 `idf.py build`，因为 `idf.py build` 在这台机器上会卡在 `[3/9] Completed 'bootloader'`（ninja 存活但 CPU 为 0）。别再改回去。

### 6.2 烘焙脚本卡帧

**症状**：`bake.mjs` 跑到某帧后，`page.screenshot()` 每次返回**完全相同的字节**，每帧耗时从 100 ms 劣化到 6 s，偶尔写出 0 字节 PNG。曾卡在 idle 状态第 16 帧。

**已知**：整条链路在更早的一次运行里**完整成功过**，所以不是设计问题，是稳定性问题。最后一次读 `third_party/grokbot/grok-player.js` 找 `step(2)` 的推进逻辑时被中断——**这是最可疑的点：`step(n)` 可能没有真正推进 requestAnimationFrame 驱动的内部时钟**。

**建议**：先读完 `grok-player.js` 里 `step()` / `debugFrame` / `frozenAt` 的实现，确认帧是否真的在推进；若确属 Chrome 合成器卡死，改为一状态一进程，或每次重开 browser。

### 6.3 图集只有 53 状态版

仓库里 `main/grokbot.atlas` 是 **53 状态 / 4.40 MiB / 240×240 / 30 fps**，从头部读出来的：

```
magic=GBZ1  ver=1  w=240  h=240  fps=30  clips=53
```

用户已确认：为了省 flash，**只保留 12 个状态**，删掉 38 个百分百用不到的。这轮重烘焙没做完。

保留白名单（按产品流程排好序）：

```
idle, listening, dictating, thinking, happy, working,
proud, celebrate, failed, blocked, sleeping, waking
```

预期体积约 **0.7 MB**。重烘焙命令（在 `tools/grokbot/` 下）：

```bash
ATLAS_SIZE=240 \
ATLAS_STATES="idle,listening,dictating,thinking,happy,working,proud,celebrate,failed,blocked,sleeping,waking" \
CHROME_BIN="C:\Program Files\Google\Chrome\Application\chrome.exe" \
PYTHON_BIN="D:\sticker\idf\net_speed\tools\.venv\Scripts\python.exe" \
node bake.mjs
```

产物落在 `main/grokbot.atlas`，然后必须再跑一次 `encode_atlas.py`（`bake.mjs` 末尾会自动调用）。烧录由 `build.ps1 -Flash` 自动处理（图集变更才写，写 `0x410000`）。

---

## 7. 图集格式规范（GBZ1）

自己实现解码器时照这个来，**小端序**。

### 全局头（16 字节）

```c
struct GlobalHeader {      // "<4sHHHBBI"
    char     magic[4];     // "GBZ1"
    uint16_t version;      // 1
    uint16_t width;        // 240
    uint16_t height;       // 240
    uint8_t  fps;          // 30
    uint8_t  clip_count;   // 状态数（≤255）
    uint32_t reserved;     // 0
};
```

### 状态表（每项 32 字节，紧跟全局头）

```c
struct ClipHeader {        // "<16sHHIII"
    char     name[16];     // ASCII 状态名，如 "idle"
    uint16_t frame_count;
    uint16_t start_frame;  // 全局帧号（仅作记录，播放器用不到）
    uint32_t data_offset;  // 相对文件头的偏移，指向本状态的帧索引
    uint32_t data_size;    // 本状态总字节数
    uint32_t packed_size;  // 每帧打包字节数 = w*h/2
};
```

### 帧索引（每帧 8 字节，位于 `data_offset`）

```c
struct FrameIndex {        // "<II"
    uint32_t payload_offset;             // 相对 data_offset
    uint32_t size_and_flags;             // 低 31 位=压缩后长度，最高位=是否为 XOR 差分
};
```

### 帧数据

```
[4 字节 压缩长度][1 字节 flags][3 字节填充] + zlib 压缩载荷
```

### 解码流程

1. **第 0 帧**：直接 zlib 解压 → 得到 `w*h/2` 字节的打包灰度
2. **后续帧**：zlib 解压 → 与**上一帧的打包结果**逐字节 XOR → 得到本帧
3. **打包格式**：每字节 2 个像素，**高 4 位 = 左像素**，低 4 位 = 右像素
4. **灰度级**：`level * 17`（0 → 0，15 → 255），即 16 级均匀灰度
5. **转 RGB565**（`grokbot.c:74-78`）：
   ```c
   uint16_t c = ((v >> 3) << 11) | ((v >> 2) << 5) | (v >> 3);
   c = (c >> 8) | (c << 8);   // 字节交换，因为面板要大端
   ```

**屏上位置**：图集是 240×240 方图，`grokbot.c:221` 把它垂直居中推到 240×320 的屏上 → `y0 = (320-240)/2 = 40`。上下各留 40 px 黑边。如果之后要利用这块空间（放状态文字/进度），就在这里改。

**线程与内存**：解压需要两块 PSRAM 缓冲（`s_packed` + `s_delta`，各 28.8 KB），`tinfl_decompressor` 结构本身占 11 KB **栈**，任务栈已设 16 KB，别改小。

---

## 8. 环境

| 项 | 值 |
|---|---|
| IDF | `D:\Espressif\esp-idf`（v5.5） |
| Python env | `D:\Espressif\tools\python_env\idf5.5_py3.11_env` |
| 烘焙 venv | `D:\sticker\idf\net_speed\tools\.venv`（numpy 2.5.5 + Pillow 13，已装好） |
| Chrome | `C:\Program Files\Google\Chrome\Application\chrome.exe` |
| 串口 | COM4（烧录用） |
| ccache | `C:\Users\xie\AppData\Local\ccache`，**构建中断后锁残留会永久阻塞**，一律带 `CCACHE_DISABLE=1` |

**构建入口只用 `build.ps1`，不要临场手搓命令**：

```powershell
.\build.ps1           # 只编译
.\build.ps1 -Flash    # 编译 + 烧 app + （图集变了才）烧图集
.\build.ps1 -Port COM5
```

输出约定：`BUILD_OK <bin> 大小 时间` / `BUILD_FAILED` / `FLASH_OK` / `FLASH_FAILED` / `ATLAS_SKIP` / `PORT_BUSY`。

**`.ps1` 必须保持纯 ASCII**：Windows PowerShell 5.1 按 GBK 解析脚本，UTF-8 中文会把引号拆断，报 `TerminatorExpectedAtEndOfString`。

---

## 9. 版权与不可入库项

`demo/grok-ref/` 是从 `github.com/blessonism/grok-icon-study` 原样下载的角色引擎（含 `geometry-data.js`，其几何数据从 xAI 安装包提取）。原作者明确写明**勿商用、勿再分发**。

用户口径是「只作学习、不商用」，所以我按 1:1 照抄、一个字节没改。但**这个目录已经写进 `.gitignore`，绝不能推到公开仓库**。设备端的 53 个状态图集是从这套引擎烘焙出来的，同样不建议公开分发。要复现请让使用者自己去原仓库拉。

其余自己写的代码（`grokbot.c`、`bake.mjs` 的补丁、`check_atlas.py`、四个 demo HTML）没有这个问题。

---

## 10. 下一步建议（按性价比排序）

1. **先修构建**（§6.1）。删 `build/` 重配，这是唯一确定能过的方法。不修好这一步，后面全是空谈。
2. **烧一次 53 状态图集，看屏幕上动画是否丝滑**。用户的原话就是「直接烧录固件我看屏幕就知道动画是否丝滑」。这是第一次真机验证，重点看：帧率、有无花屏、上下 40 px 黑边是否难看。
3. **再决定要不要精简到 12 状态**（§6.3）。先看到动画，再决定砍哪些，比现在盲砍更合理。
4. **接回业务流**：`ANIM_DEMO=0`，把四幕映射到 `grokbot_set_state()`（接口已留：`grokbot_start()` / `grokbot_state_count()`，还缺一个按名切状态的接口，需要补）。
5. **语音输入**目前完全没做。`demo/v5-grok.html` 里的"逐字听写"是按按住时长推进的**假数据**，真机要接 ASR（离线命令词或云端），文字到达节奏就不受控了，那一层要改成"随到随写"。

---

## 11. 一句话给接手的人

屏幕驱动、图集格式、交互设计这三块是**扎实的**，可以直接用；卡住的地方只有一个——**这台 Windows 机器上 ESP-IDF 的重配置会死锁**。绕过去，剩下的都是常规活。
