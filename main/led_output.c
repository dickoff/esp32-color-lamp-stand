#include "led_output.h"

#include <math.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "led_strip.h"

#define LED_DATA_GPIO 13
#define FULL_BRIGHTNESS 31

static led_strip_handle_t s_led_strip;
static SemaphoreHandle_t s_led_mutex;
static uint8_t s_maximum_brightness = 3;

esp_err_t led_output_initialize(uint8_t maximum_brightness)
{
    s_maximum_brightness = maximum_brightness;

    led_strip_config_t strip_config = {
        .strip_gpio_num = LED_DATA_GPIO,
        .max_leds = LED_COUNT,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .flags.invert_out = false,
    };
    led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
        .mem_block_symbols = 0,
        .flags.with_dma = true,
    };
    esp_err_t err = led_strip_new_rmt_device(&strip_config, &rmt_config, &s_led_strip);
    if (err != ESP_OK) {
        return err;
    }

    s_led_mutex = xSemaphoreCreateMutex();
    return s_led_mutex == NULL ? ESP_ERR_NO_MEM : ESP_OK;
}

uint8_t led_output_maximum_brightness(void)
{
    return s_maximum_brightness;
}

static uint8_t apply_brightness(uint8_t channel, uint8_t brightness)
{
    return ((uint16_t)channel * brightness + 15) / FULL_BRIGHTNESS;
}

void led_output_show_pixels(const pixel_color_t pixels[LED_COUNT])
{
    xSemaphoreTake(s_led_mutex, portMAX_DELAY);

    for (int pixel = 0; pixel < LED_COUNT; ++pixel) {
        uint8_t brightness = pixels[pixel].brightness;
        if (brightness > s_maximum_brightness) {
            brightness = s_maximum_brightness;
        }
        uint8_t red = apply_brightness(pixels[pixel].r, brightness);
        uint8_t green = apply_brightness(pixels[pixel].g, brightness);
        uint8_t blue = apply_brightness(pixels[pixel].b, brightness);
        ESP_ERROR_CHECK(led_strip_set_pixel(s_led_strip, pixel, red, green, blue));
    }
    ESP_ERROR_CHECK(led_strip_refresh(s_led_strip));
    xSemaphoreGive(s_led_mutex);
}

void led_output_fill(pixel_color_t color)
{
    pixel_color_t pixels[LED_COUNT];
    for (int pixel = 0; pixel < LED_COUNT; ++pixel) {
        pixels[pixel] = color;
    }
    led_output_show_pixels(pixels);
}

static uint8_t scale_channel(float channel, float scale)
{
    float value = channel * scale * 255.0f;
    if (value <= 0.0f) {
        return 0;
    }
    // Keep a channel the color needs lit at the lowest levels instead of
    // letting it round away, which would shift the hue or turn the lamp off.
    if (channel > 0.02f && value < 1.0f) {
        return 1;
    }
    return value >= 255.0f ? 255 : (uint8_t)lroundf(value);
}

void led_output_fill_scaled(float red, float green, float blue, float brightness)
{
    if (brightness < 0.0f) {
        brightness = 0.0f;
    } else if (brightness > 1.0f) {
        brightness = 1.0f;
    }
    float scale = brightness * s_maximum_brightness / FULL_BRIGHTNESS;
    uint8_t scaled_red = brightness > 0.0f ? scale_channel(red, scale) : 0;
    uint8_t scaled_green = brightness > 0.0f ? scale_channel(green, scale) : 0;
    uint8_t scaled_blue = brightness > 0.0f ? scale_channel(blue, scale) : 0;

    xSemaphoreTake(s_led_mutex, portMAX_DELAY);
    for (int pixel = 0; pixel < LED_COUNT; ++pixel) {
        ESP_ERROR_CHECK(led_strip_set_pixel(s_led_strip, pixel, scaled_red, scaled_green,
                                            scaled_blue));
    }
    ESP_ERROR_CHECK(led_strip_refresh(s_led_strip));
    xSemaphoreGive(s_led_mutex);
}
