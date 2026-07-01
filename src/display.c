/*
 * Minimal ESP-IDF (no Arduino) driver for the ST7735 SPI TFT panel used on
 * this board. Talks to the panel directly over driver/spi_master.h — no
 * TFT_eSPI, no esp_lcd component required.
 */

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "display.h"
#include "tasks.h"

static const char *TAG = "display";

#define DISPLAY_SPI_HOST SPI2_HOST

static spi_device_handle_t s_spi;

// ---- ST7735 command set (subset we actually use) --------------------------
#define ST7735_SWRESET 0x01
#define ST7735_SLPOUT  0x11
#define ST7735_COLMOD  0x3A
#define ST7735_MADCTL  0x36
#define ST7735_CASET   0x2A
#define ST7735_RASET   0x2B
#define ST7735_RAMWR   0x2C
#define ST7735_INVON   0x21
#define ST7735_NORON   0x13
#define ST7735_DISPON  0x29
#define ST7735_FRMCTR1 0xB1
#define ST7735_FRMCTR2 0xB2
#define ST7735_FRMCTR3 0xB3
#define ST7735_INVCTR  0xB4
#define ST7735_PWCTR1  0xC0
#define ST7735_PWCTR2  0xC1
#define ST7735_PWCTR3  0xC2
#define ST7735_PWCTR4  0xC3
#define ST7735_PWCTR5  0xC4
#define ST7735_VMCTR1  0xC5
#define ST7735_GMCTRP1 0xE0
#define ST7735_GMCTRN1 0xE1

// Init table: { cmd, n_args, args[n_args], delay_ms }, delay_ms may be 0.
static const uint8_t s_init_seq[] = {
    ST7735_SWRESET, 0,                                                          150,
    ST7735_SLPOUT,  0,                                                          200,
    ST7735_FRMCTR1, 3, 0x01, 0x2C, 0x2D,                                        0,
    ST7735_FRMCTR2, 3, 0x01, 0x2C, 0x2D,                                        0,
    ST7735_FRMCTR3, 6, 0x01, 0x2C, 0x2D, 0x01, 0x2C, 0x2D,                      0,
    ST7735_INVCTR,  1, 0x07,                                                    0,
    ST7735_PWCTR1,  3, 0xA2, 0x02, 0x84,                                        0,
    ST7735_PWCTR2,  1, 0xC5,                                                    0,
    ST7735_PWCTR3,  2, 0x0A, 0x00,                                              0,
    ST7735_PWCTR4,  2, 0x8A, 0x2A,                                              0,
    ST7735_PWCTR5,  2, 0x8A, 0xEE,                                              0,
    ST7735_VMCTR1,  1, 0x0E,                                                    0,
    ST7735_INVON,   0,                                                          0,
    ST7735_COLMOD,  1, 0x05,                                                    0,
    ST7735_GMCTRP1, 16, 0x02,0x1c,0x07,0x12,0x37,0x32,0x29,0x2d,
                        0x29,0x25,0x2B,0x39,0x00,0x01,0x03,0x10,               0,
    ST7735_GMCTRN1, 16, 0x03,0x1d,0x07,0x06,0x2E,0x2C,0x29,0x2D,
                        0x2E,0x2E,0x37,0x3F,0x00,0x00,0x02,0x10,               0,
    ST7735_NORON,   0,                                                          10,
    ST7735_DISPON,  0,                                                          100,
};

// ---- low-level transfers ---------------------------------------------------

static void tft_write_cmd(uint8_t cmd)
{
    gpio_set_level(TFT_DC_PIN, 0);
    spi_transaction_t t = {
        .length = 8,
        .tx_buffer = &cmd,
    };
    spi_device_polling_transmit(s_spi, &t);
}

static void tft_write_data(const uint8_t *data, size_t len)
{
    if (len == 0) return;
    gpio_set_level(TFT_DC_PIN, 1);
    spi_transaction_t t = {
        .length = len * 8,
        .tx_buffer = data,
    };
    spi_device_polling_transmit(s_spi, &t);
}

static void tft_set_addr_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
    uint8_t caset[4] = { (uint8_t)(x0 >> 8), (uint8_t)(x0 & 0xFF),
                          (uint8_t)(x1 >> 8), (uint8_t)(x1 & 0xFF) };
    uint8_t raset[4] = { (uint8_t)(y0 >> 8), (uint8_t)(y0 & 0xFF),
                          (uint8_t)(y1 >> 8), (uint8_t)(y1 & 0xFF) };
    tft_write_cmd(ST7735_CASET);
    tft_write_data(caset, 4);
    tft_write_cmd(ST7735_RASET);
    tft_write_data(raset, 4);
    tft_write_cmd(ST7735_RAMWR);
}

// ---- public API -------------------------------------------------------------

void display_init(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << TFT_DC_PIN) | (1ULL << TFT_RES_PIN) | (1ULL << TFT_LEDA_PIN),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&io_conf);

    gpio_set_level(TFT_LEDA_PIN, 1); // backlight off while we init (active low)

    spi_bus_config_t buscfg = {
        .mosi_io_num = TFT_SDA_PIN,
        .miso_io_num = -1,
        .sclk_io_num = TFT_SCL_PIN,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = DISPLAY_WIDTH * 2 * 16, // headroom for chunked pushes
    };
    ESP_ERROR_CHECK(spi_bus_initialize(DISPLAY_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO));

    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = 26 * 1000 * 1000,
        .mode = 0,
        .spics_io_num = TFT_CS_PIN,
        .queue_size = 4,
    };
    ESP_ERROR_CHECK(spi_bus_add_device(DISPLAY_SPI_HOST, &devcfg, &s_spi));

    // Hardware reset pulse
    gpio_set_level(TFT_RES_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(TFT_RES_PIN, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(TFT_RES_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(120));

    size_t i = 0;
    while (i < sizeof(s_init_seq)) {
        uint8_t cmd = s_init_seq[i++];
        uint8_t n   = s_init_seq[i++];
        tft_write_cmd(cmd);
        if (n) {
            tft_write_data(&s_init_seq[i], n);
            i += n;
        }
        uint8_t delay_ms = s_init_seq[i++];
        if (delay_ms) vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }

    // MADCTL: landscape rotation (equivalent to tft.setRotation(1)), with
    // optional BGR bit. If your colors look swapped, flip DISPLAY_BGR_ORDER
    // in display.h.
    uint8_t madctl = 0x60 | (DISPLAY_BGR_ORDER ? 0x08 : 0x00);
    tft_write_cmd(ST7735_MADCTL);
    tft_write_data(&madctl, 1);

    display_fill_screen(0x0000);

    gpio_set_level(TFT_LEDA_PIN, 0); // backlight on
    ESP_LOGI(TAG, "display initialized");
}

void display_fill_rect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color565)
{
    tft_set_addr_window(x + DISPLAY_COL_OFFSET, y + DISPLAY_ROW_OFFSET,
                         x + DISPLAY_COL_OFFSET + w - 1, y + DISPLAY_ROW_OFFSET + h - 1);

    uint8_t hi = color565 >> 8, lo = color565 & 0xFF;
    uint8_t line[DISPLAY_WIDTH * 2];
    for (int16_t i = 0; i < w; i++) {
        line[i * 2] = hi;
        line[i * 2 + 1] = lo;
    }

    gpio_set_level(TFT_DC_PIN, 1);
    for (int16_t row = 0; row < h; row++) {
        spi_transaction_t t = {
            .length = (size_t)w * 16,
            .tx_buffer = line,
        };
        spi_device_polling_transmit(s_spi, &t);
    }
}

void display_fill_screen(uint16_t color565)
{
    display_fill_rect(0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT, color565);
}

void display_push_image(int16_t x, int16_t y, int16_t w, int16_t h, const uint16_t *data)
{
    tft_set_addr_window(x + DISPLAY_COL_OFFSET, y + DISPLAY_ROW_OFFSET,
                         x + DISPLAY_COL_OFFSET + w - 1, y + DISPLAY_ROW_OFFSET + h - 1);

    gpio_set_level(TFT_DC_PIN, 1);

    const size_t total_px = (size_t)w * (size_t)h;
    static uint8_t chunk[512]; // 256 px per chunk
    size_t done = 0;
    while (done < total_px) {
        size_t this_batch = total_px - done;
        if (this_batch > 256) this_batch = 256;
        for (size_t i = 0; i < this_batch; i++) {
            uint16_t px = data[done + i];
            chunk[i * 2]     = px >> 8;
            chunk[i * 2 + 1] = px & 0xFF;
        }
        spi_transaction_t t = {
            .length = this_batch * 16,
            .tx_buffer = chunk,
        };
        spi_device_polling_transmit(s_spi, &t);
        done += this_batch;
    }
}

// ---- demo task --------------------------------------------------------------
// Cycles solid colors so you can confirm the panel is alive and correctly
// oriented before wiring up real image data with display_push_image().
void TaskDisplayDemo(void *pvParameters)
{
    (void) pvParameters;

    display_init();

    static const uint16_t colors[] = {
        RGB565(255, 0, 0),
        RGB565(0, 255, 0),
        RGB565(0, 0, 255),
        RGB565(255, 255, 0),
    };
    size_t i = 0;
    while (1) {
        display_fill_screen(colors[i]);
        i = (i + 1) % (sizeof(colors) / sizeof(colors[0]));
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}
