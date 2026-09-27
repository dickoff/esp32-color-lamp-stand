#pragma once

#include <stdint.h>

#include "esp_err.h"
#include "pixel.h"

#define LED_COUNT 32

// Every LED write passes through this module so the USB-source brightness cap
// always applies, whichever task is drawing.
esp_err_t led_output_initialize(uint8_t maximum_brightness);
uint8_t led_output_maximum_brightness(void);

void led_output_show_pixels(const pixel_color_t pixels[LED_COUNT]);
void led_output_fill(pixel_color_t color);

// Fills every LED with a color whose channels are 0.0-1.0. Brightness 1.0 is
// the most the current power source allows, not full-scale LED output.
void led_output_fill_scaled(float red, float green, float blue, float brightness);
