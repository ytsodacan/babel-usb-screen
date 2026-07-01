#pragma once

#include <stdint.h>

// Pin mapping (from your board's pinout — T-Dongle-S3 style)
#define TFT_CS_PIN     4
#define TFT_SDA_PIN    3   // MOSI
#define TFT_SCL_PIN    5   // SCLK
#define TFT_DC_PIN     2
#define TFT_RES_PIN    1
#define TFT_LEDA_PIN   38  // backlight, active LOW

#define DISPLAY_WIDTH  160
#define DISPLAY_HEIGHT 80

// ST7735 "tab" panels vary in their internal RAM offset between manufacturing
// batches. This panel is natively 80x160 (portrait); since we're driving it
// rotated into 160x80 landscape here, the offset pair is swapped relative to
// portrait mode. If the image is still shifted, cropped, or wrapped around
// an edge, try swapping these two values, or try 0/0.
#define DISPLAY_COL_OFFSET 1
#define DISPLAY_ROW_OFFSET 26

// If colors come out swapped (red looks blue, etc.), flip this to 0.
#define DISPLAY_BGR_ORDER 1

// Convert 8-bit RGB into the 16-bit RGB565 format the panel expects.
#define RGB565(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))

// Initializes SPI bus + GPIOs, resets and configures the ST7735, turns on
// the backlight. Call once before any of the draw functions below.
void display_init(void);

// Fills the whole screen with a single RGB565 color.
void display_fill_screen(uint16_t color565);

// Fills a rectangular region with a single RGB565 color.
void display_fill_rect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color565);

// Pushes a w*h buffer of RGB565 pixels (row-major, no padding) to the screen
// at (x, y). Equivalent to TFT_eSPI's tft.pushImage().
void display_push_image(int16_t x, int16_t y, int16_t w, int16_t h, const uint16_t *data);
