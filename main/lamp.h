#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Units follow the Matter OnOff, LevelControl, and ColorControl clusters so
// the Matter layer can pass attribute values through without conversion.
#define LAMP_LEVEL_MIN 1
#define LAMP_LEVEL_MAX 254
#define LAMP_HUE_MAX 254  // 360 degrees
#define LAMP_SATURATION_MAX 254
#define LAMP_COLOR_TEMPERATURE_MIN_MIREDS 153  // ~6500 K
#define LAMP_COLOR_TEMPERATURE_MAX_MIREDS 500  // 2000 K

typedef enum {
    LAMP_COLOR_MODE_HUE_SATURATION,
    LAMP_COLOR_MODE_TEMPERATURE,
} lamp_color_mode_t;

typedef enum {
    LAMP_STOCK_UNKNOWN,
    LAMP_STOCK_UP,
    LAMP_STOCK_DOWN,
} lamp_stock_indication_t;

typedef struct {
    bool on;
    uint8_t level;
    lamp_color_mode_t color_mode;
    uint8_t hue;
    uint8_t saturation;
    uint16_t color_temperature_mireds;
    // Stock mode replaces the chosen color with the stock indication. Choosing
    // a color or temperature leaves stock mode; on/off and level apply to both.
    bool stock_mode;
    lamp_stock_indication_t stock_indication;
} lamp_state_t;

typedef void (*lamp_change_callback_t)(const lamp_state_t *state, void *context);

// Requires NVS to be initialized. Loads the saved state, or defaults to stock
// mode so an unpaired stand behaves like the original stock indicator.
esp_err_t lamp_initialize(void);
// Starts drawing the lamp state; before this, other code owns the LEDs.
esp_err_t lamp_start(void);
// Callbacks run on the setter's task after the state has changed. Up to
// LAMP_MAX_CHANGE_CALLBACKS may be registered, before or after lamp_start().
#define LAMP_MAX_CHANGE_CALLBACKS 4
esp_err_t lamp_add_change_callback(lamp_change_callback_t callback, void *context);

lamp_state_t lamp_get_state(void);
void lamp_set_on(bool on);
void lamp_set_level(uint8_t level);
void lamp_set_hue_saturation(uint8_t hue, uint8_t saturation);
void lamp_set_color_temperature(uint16_t mireds);
void lamp_set_stock_mode(bool enabled);
void lamp_set_stock_indication(lamp_stock_indication_t indication);

// Blinks the whole ring so a person can find this lamp, overriding the normal
// display for duration_ms; 0 stops it early. Not saved and not reported.
void lamp_identify(uint32_t duration_ms);

const char *lamp_stock_indication_name(lamp_stock_indication_t indication);

#ifdef __cplusplus
}
#endif
