// ST7735 160x80 屏驱动 + LVGL 端口
//
// 只用到 panel_io 层：tx_param 发命令/参数，tx_color 发像素。
// 不用 esp_lcd_panel_* 的厂商驱动，因为 IDF 没内置 ST7735，
// 而我们的初始化序列只有 6 条命令，自己发更直接。
//
// LVGL 侧：s_buf 直接当全屏 draw buffer，flush_cb 整块推 SPI。
// LV_COLOR_16_SWAP=1 让 LVGL 把每个像素按大端写进内存，正好是 ST7735 要的 MSB-first，
// 所以 flush 时不需要再做任何字节交换。

#include "app.h"

#include <string.h>

#include "lvgl.h"

#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_io_spi.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "lcd";

#define PIN_SCK  12
#define PIN_MOSI 11
#define PIN_CS   39
#define PIN_DC   40
#define PIN_RST  38
#define PIN_BL   41

// 面板可视区在 132x162 显存里的起点，已实测
#define VIS_X 1
#define VIS_Y 26

#define MADCTL_VAL 0xA8   // MY|MV|BGR：160x80 横向，红蓝顺序正确

// ST7735 写的时序下限约 66ns（≈15MHz），20MHz 是留了余量的实用值
#define SPI_PCLK_HZ (20 * 1000 * 1000)

static esp_lcd_panel_io_handle_t s_io;
static SemaphoreHandle_t s_flush_done;
static SemaphoreHandle_t s_mtx;

// LVGL 的全屏绘制缓冲，也是推屏的源
static lv_color_t s_buf[LCD_W * LCD_H];

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

static void set_window(int x0, int y0, int x1, int y1)
{
    uint8_t c[4] = { (uint8_t)(x0 >> 8), (uint8_t)x0, (uint8_t)(x1 >> 8), (uint8_t)x1 };
    uint8_t r[4] = { (uint8_t)(y0 >> 8), (uint8_t)y0, (uint8_t)(y1 >> 8), (uint8_t)y1 };
    esp_lcd_panel_io_tx_param(s_io, 0x2A, c, 4);
    esp_lcd_panel_io_tx_param(s_io, 0x2B, r, 4);
}

// ───────── LVGL flush ─────────
static void flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *px)
{
    int w = (int)lv_area_get_width(area);
    int h = (int)lv_area_get_height(area);

    set_window(VIS_X + area->x1, VIS_Y + area->y1,
               VIS_X + area->x2, VIS_Y + area->y2);

    xSemaphoreTake(s_flush_done, 0);                 // 清掉可能残留的信号
    esp_lcd_panel_io_tx_color(s_io, 0x2C, px, (size_t)w * h * 2);
    xSemaphoreTake(s_flush_done, pdMS_TO_TICKS(500));

    lv_disp_flush_ready(drv);
}

void display_lock(void)   { xSemaphoreTake(s_mtx, portMAX_DELAY); }
void display_unlock(void) { xSemaphoreGive(s_mtx); }

static void lvgl_task(void *arg)
{
    uint32_t last = (uint32_t)(esp_timer_get_time() / 1000);
    for (;;) {
        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
        display_lock();
        lv_tick_inc(now - last);
        last = now;
        lv_timer_handler();
        display_unlock();
        // FreeRTOS tick 是 100 Hz，pdMS_TO_TICKS(5) 会取整成 0，
        // 那样 vTaskDelay(0) 不阻塞，任务会空转饿死 IDLE0 触发看门狗。
        // 直接给 1 个 tick（10 ms），配合 20 ms 的 LVGL 刷新周期刚好。
        vTaskDelay(1);
    }
}

void display_init(void)
{
    gpio_config_t g = {
        .pin_bit_mask = (1ULL << PIN_RST) | (1ULL << PIN_BL),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&g);
    gpio_set_level(PIN_BL, 0);

    // 硬复位
    gpio_set_level(PIN_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(PIN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(120));

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

    lcd_cmd(0x01, NULL, 0, 150);          // SWRESET
    lcd_cmd(0x11, NULL, 0, 120);          // SLPOUT
    const uint8_t colmod = 0x05;          // 16bpp
    lcd_cmd(0x3A, &colmod, 1, 0);
    const uint8_t madctl = MADCTL_VAL;
    lcd_cmd(0x36, &madctl, 1, 0);
    lcd_cmd(0x21, NULL, 0, 10);           // INVON
    lcd_cmd(0x13, NULL, 0, 10);           // NORON
    lcd_cmd(0x29, NULL, 0, 120);          // DISPON

    gpio_set_level(PIN_BL, 1);
    memset(s_buf, 0, sizeof(s_buf));

    // ───── LVGL 初始化 ─────
    lv_init();

    static lv_disp_draw_buf_t draw_buf;
    lv_disp_draw_buf_init(&draw_buf, s_buf, NULL, LCD_W * LCD_H);

    static lv_disp_drv_t drv;
    lv_disp_drv_init(&drv);
    drv.hor_res      = LCD_W;
    drv.ver_res      = LCD_H;
    drv.flush_cb     = flush_cb;
    drv.draw_buf     = &draw_buf;
    drv.full_refresh = 1;
    lv_disp_drv_register(&drv);

    ESP_LOGI(TAG, "ST7735 %dx%d  offset(%d,%d)  MADCTL=0x%02X  SPI %d MHz  LVGL %d.%d",
             LCD_W, LCD_H, VIS_X, VIS_Y, MADCTL_VAL, SPI_PCLK_HZ / 1000000,
             LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR);

    xTaskCreate(lvgl_task, "lvgl", 8192, NULL, 4, NULL);
}
