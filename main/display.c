// GMT032-8P 3.2" 240x320 SPI 屏驱动 + LVGL 端口
//
// 模块丝印：GND / VCC / SCL / SDA / RES / DC / CS / BL
//           (1)   (2)   (3)   (4)   (5)  (6)  (7)  (8)
//
// 初始化序列按模块厂商 STM32 例程（GMT032-8p-spi.C 的 TFT_init）原样搬来，已实测点亮。
// 这套序列里 0xCF/0xED/0xE8/0xF6/0xCB/0xF7/0xEA 属于 ILI9341 命令集，
// 芯片会静默忽略不认识的命令，剩下 0x11/0x3A/0x36/0xE0/0xE1/0x29 足以点亮。
//
// 之前连续白屏的真因是 CS 杜邦线内部断线：命令一条都没被锁存，
// 控制器停在未初始化状态，背光一亮就是纯白，和「驱动写错」的现象完全一样。
//
// 缓冲分两种，别混：
//   s_buf    整屏 153.6 KB，放 PSRAM，给点阵动画直接写像素用（display_fb）
//   s_lv_buf LVGL 的分块绘制缓冲 240x40 = 19.2 KB，放内部 RAM，DMA 推屏快

#include "app.h"

#include <string.h>

#include "lvgl.h"

#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_io_spi.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "lcd";

// ───── 接线：模块 3-7 针 → 下排 3-7 位，笔直一排，零交叉 ─────
// 板子下排丝印（从左数）：GND 5V 14 13 12 11 10 9 46 3 8 18 17 16 15 7 6 5 4 RST 3V3 3V3
//                         1   2  3  4  5  6  7 8  9 10 11 12 13 14 15 16 17 18 19 20  21  22
#define PIN_SCK  14   // 下排第 3 位 ← 模块 3 SCL
#define PIN_MOSI 13   // 下排第 4 位 ← 模块 4 SDA
#define PIN_RST  12   // 下排第 5 位 ← 模块 5 RES
#define PIN_DC   11   // 下排第 6 位 ← 模块 6 A0（A0 就是 DC）
#define PIN_CS   10   // 下排第 7 位 ← 模块 7 CS

// 背光：模块背面没有背光驱动管，厂家 STM32 例程也只控制 SCL/SDA/RST/DC/CS 五根，
// 根本不控制 BL。说明 BL 就是背光 LED 的阳极（对应 FPC 的 A），必须直接接 3.3V 常亮。
// 接 GPIO 会因为驱动电流不足（背光需 20~60 mA，GPIO 到不了）把电压拉垮，表现就是全黑。
#define PIN_BL  (-1)

// 板载 0.96" 屏占 IO38-41。它和外部屏在物理上不是同一组线，但为了保险仍然按住：
// RST 拉低保持复位，CS 拉高永不选中。
#define PIN_ONBOARD_RST 38
#define PIN_ONBOARD_CS  39

// 这块模组可视区就是整屏，没有偏移
#define VIS_X 0
#define VIS_Y 0

// 厂家例程用的是 0x40（MY=1）。红绿蓝三条竖条实测位置正确，说明这个值对。
#define MADCTL_VAL 0x40

// 40 MHz：ST7789 的写周期下限 16 ns（≈62.5 MHz），40 MHz 留了一倍余量。
// 整屏 153.6 KB 在 40 MHz 下约 31 ms，是这块屏在 SPI 上能拿到的实用上限。
#define SPI_PCLK_HZ (40 * 1000 * 1000)

// LVGL 分块绘制缓冲的高度。240x40x2 = 19.2 KB，内部 RAM 静态分配
#define LVGL_BAND_H 40

static esp_lcd_panel_io_handle_t s_io;
static SemaphoreHandle_t s_flush_done;
static SemaphoreHandle_t s_mtx;

// 整屏缓冲（PSRAM）：点阵动画直接往这里写像素
static lv_color_t *s_buf;

// LVGL 的分块绘制缓冲（内部 RAM）
static lv_color_t s_lv_buf[LCD_W * LVGL_BAND_H];

static bool on_color_done(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *edata, void *ctx)
{
    BaseType_t hp = pdFALSE;
    xSemaphoreGiveFromISR(s_flush_done, &hp);
    return hp == pdTRUE;
}

static void lcd_cmd(uint8_t cmd, const uint8_t *param, size_t n, uint32_t delay_ms)
{
    esp_lcd_panel_io_tx_param(s_io, cmd, param, n);
    if (delay_ms) vTaskDelay(pdMS_TO_TICKS(delay_ms));
}

static void lcd_cmd1(uint8_t cmd, uint8_t p0)
{
    esp_lcd_panel_io_tx_param(s_io, cmd, &p0, 1);
}

static void set_window(int x0, int y0, int x1, int y1)
{
    uint8_t c[4] = { (uint8_t)(x0 >> 8), (uint8_t)x0, (uint8_t)(x1 >> 8), (uint8_t)x1 };
    uint8_t r[4] = { (uint8_t)(y0 >> 8), (uint8_t)y0, (uint8_t)(y1 >> 8), (uint8_t)y1 };
    esp_lcd_panel_io_tx_param(s_io, 0x2A, c, 4);
    esp_lcd_panel_io_tx_param(s_io, 0x2B, r, 4);
}

// 把整屏刷成一个颜色（host 序 RGB565）
static void raw_fill(uint16_t rgb565)
{
    uint16_t *p = (uint16_t *)s_buf;
    uint16_t  be = (uint16_t)((rgb565 >> 8) | (rgb565 << 8));   // 屏要 MSB first
    for (int i = 0; i < LCD_W * LCD_H; i++) p[i] = be;

    set_window(VIS_X, VIS_Y, VIS_X + LCD_W - 1, VIS_Y + LCD_H - 1);
    xSemaphoreTake(s_flush_done, 0);
    esp_lcd_panel_io_tx_color(s_io, 0x2C, s_buf, (size_t)LCD_W * LCD_H * 2);
    xSemaphoreTake(s_flush_done, pdMS_TO_TICKS(3000));
}

// ───────── LVGL flush ─────────
static void flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *px)
{
    int w = (int)lv_area_get_width(area);
    int h = (int)lv_area_get_height(area);

    set_window(VIS_X + area->x1, VIS_Y + area->y1,
               VIS_X + area->x2, VIS_Y + area->y2);

    xSemaphoreTake(s_flush_done, 0);
    esp_lcd_panel_io_tx_color(s_io, 0x2C, px, (size_t)w * h * 2);
    xSemaphoreTake(s_flush_done, pdMS_TO_TICKS(3000));

    lv_disp_flush_ready(drv);
}

void display_lock(void)   { xSemaphoreTake(s_mtx, portMAX_DELAY); }
void display_unlock(void) { xSemaphoreGive(s_mtx); }

uint16_t *display_fb(void)
{
    return (uint16_t *)s_buf;
}

void display_present(void)
{
    set_window(VIS_X, VIS_Y, VIS_X + LCD_W - 1, VIS_Y + LCD_H - 1);
    xSemaphoreTake(s_flush_done, 0);
    esp_lcd_panel_io_tx_color(s_io, 0x2C, s_buf, (size_t)LCD_W * LCD_H * 2);
    xSemaphoreTake(s_flush_done, pdMS_TO_TICKS(3000));
}

void display_push_band(int y0, int h, const uint16_t *buf)
{
    set_window(VIS_X, VIS_Y + y0, VIS_X + LCD_W - 1, VIS_Y + y0 + h - 1);
    // 异步：排队后立刻返回，DMA 完成时给 s_flush_done
    esp_lcd_panel_io_tx_color(s_io, 0x2C, (void *)buf, (size_t)LCD_W * h * 2);
}

void display_wait_flush(void)
{
    xSemaphoreTake(s_flush_done, pdMS_TO_TICKS(3000));
}

static void lvgl_task(void *arg)
{
    uint32_t last = (uint32_t)(esp_timer_get_time() / 1000);
    for (;;) {
        display_lock();
        // 点阵动画期间屏幕归 anim 任务所有，这里必须让开
        if (!anim_active()) {
            uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
            lv_tick_inc(now - last);
            last = now;
            lv_timer_handler();
        } else {
            last = (uint32_t)(esp_timer_get_time() / 1000);
        }
        display_unlock();
        // tick 已经是 1000 Hz（见 sdkconfig 的 CONFIG_FREERTOS_HZ），
        // pdMS_TO_TICKS(5) 现在真的是 5 ms，不会再取整成 0。
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

// ───── 初始化序列：厂家 STM32 例程原样照搬 ─────
static void panel_init_sequence(void)
{
    gpio_set_level(PIN_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(80));
    gpio_set_level(PIN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(120));

    lcd_cmd(0x01, NULL, 0, 120);            // Software Reset
    lcd_cmd(0x11, NULL, 0, 120);            // Sleep Out

    { const uint8_t p[] = { 0x00, 0xC1, 0x30 };       lcd_cmd(0xCF, p, 3, 0); }
    { const uint8_t p[] = { 0x64, 0x03, 0x12, 0x81 }; lcd_cmd(0xED, p, 4, 0); }
    { const uint8_t p[] = { 0x85, 0x11, 0x78 };       lcd_cmd(0xE8, p, 3, 0); }
    { const uint8_t p[] = { 0x01, 0x30, 0x00 };       lcd_cmd(0xF6, p, 3, 0); }
    { const uint8_t p[] = { 0x39, 0x2C, 0x00, 0x34, 0x05 }; lcd_cmd(0xCB, p, 5, 0); }
    lcd_cmd1(0xF7, 0x20);
    { const uint8_t p[] = { 0x00, 0x00 };  lcd_cmd(0xEA, p, 2, 0); }

    lcd_cmd1(0xC0, 0x20);                   // Power Control 1
    lcd_cmd1(0xC1, 0x11);                   // Power Control 2
    { const uint8_t p[] = { 0x31, 0x3C }; lcd_cmd(0xC5, p, 2, 0); }
    lcd_cmd1(0xC7, 0xA9);

    lcd_cmd1(0x3A, 0x55);                   // COLMOD 16bit
    lcd_cmd1(0x36, MADCTL_VAL);             // MADCTL

    { const uint8_t p[] = { 0x00, 0x18 }; lcd_cmd(0xB1, p, 2, 0); }
    { const uint8_t p[] = { 0x00, 0x00 }; lcd_cmd(0xB4, p, 2, 0); }
    lcd_cmd1(0xF2, 0x00);
    lcd_cmd1(0x26, 0x01);

    { const uint8_t p[] = { 0x0F, 0x17, 0x14, 0x09, 0x0C, 0x06, 0x43,
                            0x75, 0x36, 0x08, 0x13, 0x05, 0x10, 0x0B, 0x08 };
      lcd_cmd(0xE0, p, sizeof(p), 0); }
    { const uint8_t p[] = { 0x00, 0x1F, 0x23, 0x03, 0x0E, 0x04, 0x39,
                            0x25, 0x4D, 0x06, 0x0D, 0x0B, 0x33, 0x37, 0x0F };
      lcd_cmd(0xE1, p, sizeof(p), 0); }

    lcd_cmd(0x29, NULL, 0, 120);            // Display ON
}

// 上电确认：红 → 绿 → 蓝 各 0.25 s。
// 看到三色依次闪过 = 初始化、色序、整屏寻址都对，之后交给应用。
static void bringup_flash(void)
{
    raw_fill(0xF800); vTaskDelay(pdMS_TO_TICKS(250));
    raw_fill(0x07E0); vTaskDelay(pdMS_TO_TICKS(250));
    raw_fill(0x001F); vTaskDelay(pdMS_TO_TICKS(250));
    raw_fill(0x0000);
}

void display_init(void)
{
    // 点阵动画用的整屏缓冲放 PSRAM：153.6 KB，内部 RAM 放不下
    s_buf = heap_caps_malloc((size_t)LCD_W * LCD_H * sizeof(lv_color_t),
                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_buf) {
        ESP_LOGE(TAG, "PSRAM alloc %u failed", (unsigned)(LCD_W * LCD_H * 2));
        abort();
    }
    memset(s_buf, 0, (size_t)LCD_W * LCD_H * 2);

    // RST / 板载屏静默（BL 由硬件直接接 3.3V，不占 GPIO）
    gpio_config_t g = {
        .pin_bit_mask = (1ULL << PIN_RST) |
                        (1ULL << PIN_ONBOARD_RST) | (1ULL << PIN_ONBOARD_CS),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&g);
    gpio_set_level(PIN_ONBOARD_RST, 0);    // 板载屏保持复位
    gpio_set_level(PIN_ONBOARD_CS, 1);     // 板载屏永不选中

    spi_bus_config_t bus = {
        .mosi_io_num = PIN_MOSI,
        .miso_io_num = -1,
        .sclk_io_num = PIN_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_W * LCD_H * 2 + 128,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO));

    s_flush_done = xSemaphoreCreateBinary();
    s_mtx        = xSemaphoreCreateMutex();

    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = PIN_CS,
        .dc_gpio_num = PIN_DC,
        .spi_mode = 0,
        .pclk_hz = SPI_PCLK_HZ,
        .trans_queue_depth = 4,
        .on_color_trans_done = on_color_done,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_cfg, &s_io));

    panel_init_sequence();

    ESP_LOGI(TAG, "GMT032 3.2\" %dx%d  offset(%d,%d)  MADCTL=0x%02X  SPI %d MHz  band %d rows",
             LCD_W, LCD_H, VIS_X, VIS_Y, MADCTL_VAL, SPI_PCLK_HZ / 1000000, LVGL_BAND_H);
    ESP_LOGI(TAG, "pins SCL=%d SDA=%d CS=%d DC=%d RST=%d | BL hardwired to 3V3",
             PIN_SCK, PIN_MOSI, PIN_CS, PIN_DC, PIN_RST);

    bringup_flash();

#if SCREEN_TEST
    // 自检模式：刷彩条后停住，不初始化 LVGL
    ESP_LOGW(TAG, "SCREEN TEST: 8 color bars");
    static const uint16_t bars[8] = {
        0xF800, 0x07E0, 0x001F, 0xFFFF, 0x0000, 0x07FF, 0xF81F, 0xFFE0
    };
    int bw = LCD_W / 8;
    for (int i = 0; i < 8; i++) {
        uint16_t *p = (uint16_t *)s_buf;
        uint16_t  be = (uint16_t)((bars[i] >> 8) | (bars[i] << 8));
        for (int k = 0; k < bw * LCD_H; k++) p[k] = be;
        set_window(VIS_X + i * bw, VIS_Y, VIS_X + (i + 1) * bw - 1, VIS_Y + LCD_H - 1);
        xSemaphoreTake(s_flush_done, 0);
        esp_lcd_panel_io_tx_color(s_io, 0x2C, s_buf, (size_t)bw * LCD_H * 2);
        xSemaphoreTake(s_flush_done, pdMS_TO_TICKS(3000));
    }
    return;
#endif

    // ───── LVGL ─────
    lv_init();

    static lv_disp_draw_buf_t draw_buf;
    lv_disp_draw_buf_init(&draw_buf, s_lv_buf, NULL, LCD_W * LVGL_BAND_H);

    static lv_disp_drv_t drv;
    lv_disp_drv_init(&drv);
    drv.hor_res  = LCD_W;
    drv.ver_res  = LCD_H;
    drv.flush_cb = flush_cb;
    drv.draw_buf = &draw_buf;
    // 不开 full_refresh：分块缓冲就够，全屏重绘在这个尺寸上要 61 ms
    lv_disp_drv_register(&drv);

    ESP_LOGI(TAG, "LVGL %d.%d ready, draw buf %d KB internal RAM",
             LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR, (LCD_W * LVGL_BAND_H * 2) / 1024);

    xTaskCreate(lvgl_task, "lvgl", 8192, NULL, 4, NULL);
}
