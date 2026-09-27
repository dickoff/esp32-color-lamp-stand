#include "lamp.h"

#include <math.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_output.h"
#include "nvs.h"

#define LAMP_NAMESPACE "lamp"
#define LAMP_STATE_KEY "state"
#define LAMP_STATE_VERSION 1
#define LAMP_SAVE_DELAY_US 2000000
#define LAMP_RENDER_TASK_STACK_SIZE 4096
#define LAMP_RENDER_TASK_PRIORITY 6
#define LAMP_FRAME_INTERVAL_MS 16
#define LAMP_IDLE_WAKE_MS 500
#define LAMP_FADE_US 500000
#define LAMP_STARTUP_FADE_US 1000000
// Matches the original stock indicator's update animation.
#define LAMP_STOCK_CHANGE_US 3000000
// Level 204 with the quadratic curve reproduces the original stock colors'
// brightness of 20/31 on a 3 A source.
#define LAMP_DEFAULT_LEVEL 204
#define LAMP_DEFAULT_COLOR_TEMPERATURE_MIREDS 370

typedef enum {
    TRANSITION_FADE,
    TRANSITION_THROUGH_BLACK,
} transition_style_t;

typedef struct {
    float red;
    float green;
    float blue;
    float brightness;
} lamp_frame_t;

// Stored layout; bump LAMP_STATE_VERSION when it changes.
typedef struct __attribute__((packed)) {
    uint8_t version;
    uint8_t on;
    uint8_t level;
    uint8_t color_mode;
    uint8_t hue;
    uint8_t saturation;
    uint16_t color_temperature_mireds;
    uint8_t stock_mode;
} persisted_state_t;

typedef struct {
    lamp_change_callback_t callback;
    void *context;
} change_listener_t;

static const char *LOG_TAG = "lamp";
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static lamp_state_t s_state;
static transition_style_t s_next_transition = TRANSITION_FADE;
static bool s_save_pending;
static int64_t s_last_change_us;
static TaskHandle_t s_render_task;
static change_listener_t s_listeners[LAMP_MAX_CHANGE_CALLBACKS];
static int s_listener_count;

static lamp_state_t default_state(void)
{
    return (lamp_state_t){
        .on = true,
        .level = LAMP_DEFAULT_LEVEL,
        .color_mode = LAMP_COLOR_MODE_HUE_SATURATION,
        .hue = 0,
        .saturation = 0,
        .color_temperature_mireds = LAMP_DEFAULT_COLOR_TEMPERATURE_MIREDS,
        .stock_mode = true,
        .stock_indication = LAMP_STOCK_UNKNOWN,
    };
}

static uint8_t clamp_u8(int value, int minimum, int maximum)
{
    return value < minimum ? minimum : value > maximum ? maximum : value;
}

static uint16_t clamp_mireds(int mireds)
{
    if (mireds < LAMP_COLOR_TEMPERATURE_MIN_MIREDS) {
        return LAMP_COLOR_TEMPERATURE_MIN_MIREDS;
    }
    if (mireds > LAMP_COLOR_TEMPERATURE_MAX_MIREDS) {
        return LAMP_COLOR_TEMPERATURE_MAX_MIREDS;
    }
    return mireds;
}

static void load_state(void)
{
    s_state = default_state();

    nvs_handle_t handle;
    if (nvs_open(LAMP_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        ESP_LOGI(LOG_TAG, "No saved lamp state; starting in stock mode");
        return;
    }
    persisted_state_t stored;
    size_t size = sizeof(stored);
    esp_err_t err = nvs_get_blob(handle, LAMP_STATE_KEY, &stored, &size);
    nvs_close(handle);
    if (err != ESP_OK || size != sizeof(stored) || stored.version != LAMP_STATE_VERSION) {
        ESP_LOGW(LOG_TAG, "Ignoring missing or incompatible saved lamp state");
        return;
    }

    s_state.on = stored.on != 0;
    s_state.level = clamp_u8(stored.level, LAMP_LEVEL_MIN, LAMP_LEVEL_MAX);
    s_state.color_mode = stored.color_mode == LAMP_COLOR_MODE_TEMPERATURE
                             ? LAMP_COLOR_MODE_TEMPERATURE
                             : LAMP_COLOR_MODE_HUE_SATURATION;
    s_state.hue = clamp_u8(stored.hue, 0, LAMP_HUE_MAX);
    s_state.saturation = clamp_u8(stored.saturation, 0, LAMP_SATURATION_MAX);
    s_state.color_temperature_mireds = clamp_mireds(stored.color_temperature_mireds);
    s_state.stock_mode = stored.stock_mode != 0;
}

static void save_state(const lamp_state_t *state)
{
    persisted_state_t stored = {
        .version = LAMP_STATE_VERSION,
        .on = state->on,
        .level = state->level,
        .color_mode = state->color_mode,
        .hue = state->hue,
        .saturation = state->saturation,
        .color_temperature_mireds = state->color_temperature_mireds,
        .stock_mode = state->stock_mode,
    };

    nvs_handle_t handle;
    esp_err_t err = nvs_open(LAMP_NAMESPACE, NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        err = nvs_set_blob(handle, LAMP_STATE_KEY, &stored, sizeof(stored));
        if (err == ESP_OK) {
            err = nvs_commit(handle);
        }
        nvs_close(handle);
    }
    if (err != ESP_OK) {
        ESP_LOGE(LOG_TAG, "Unable to save lamp state: %s", esp_err_to_name(err));
    }
}

static void hue_saturation_to_rgb(uint8_t hue, uint8_t saturation, lamp_frame_t *frame)
{
    float h = hue * 360.0f / LAMP_HUE_MAX / 60.0f;
    float s = (float)saturation / LAMP_SATURATION_MAX;
    int sector = (int)h % 6;
    float fraction = h - floorf(h);
    float p = 1.0f - s;
    float q = 1.0f - s * fraction;
    float t = 1.0f - s * (1.0f - fraction);
    switch (sector) {
        case 0: frame->red = 1; frame->green = t; frame->blue = p; break;
        case 1: frame->red = q; frame->green = 1; frame->blue = p; break;
        case 2: frame->red = p; frame->green = 1; frame->blue = t; break;
        case 3: frame->red = p; frame->green = q; frame->blue = 1; break;
        case 4: frame->red = t; frame->green = p; frame->blue = 1; break;
        default: frame->red = 1; frame->green = p; frame->blue = q; break;
    }
}

static float clamp_unit(float value)
{
    return value < 0.0f ? 0.0f : value > 1.0f ? 1.0f : value;
}

// Tanner Helland's blackbody approximation, normalized to 0.0-1.0 channels.
static void color_temperature_to_rgb(uint16_t mireds, lamp_frame_t *frame)
{
    float temperature = 1000000.0f / mireds / 100.0f;
    float red;
    float green;
    float blue;
    if (temperature <= 66.0f) {
        red = 255.0f;
        green = 99.4708025861f * logf(temperature) - 161.1195681661f;
    } else {
        red = 329.698727446f * powf(temperature - 60.0f, -0.1332047592f);
        green = 288.1221695283f * powf(temperature - 60.0f, -0.0755148492f);
    }
    if (temperature >= 66.0f) {
        blue = 255.0f;
    } else if (temperature <= 19.0f) {
        blue = 0.0f;
    } else {
        blue = 138.5177312231f * logf(temperature - 10.0f) - 305.0447927307f;
    }
    frame->red = clamp_unit(red / 255.0f);
    frame->green = clamp_unit(green / 255.0f);
    frame->blue = clamp_unit(blue / 255.0f);
}

static lamp_frame_t frame_for_state(const lamp_state_t *state)
{
    lamp_frame_t frame = {0};
    if (state->stock_mode) {
        switch (state->stock_indication) {
            case LAMP_STOCK_UP:
                frame.green = 1.0f;
                break;
            case LAMP_STOCK_DOWN:
                frame.red = 1.0f;
                break;
            case LAMP_STOCK_UNKNOWN:
            default:
                frame.red = frame.green = frame.blue = 1.0f;
                break;
        }
    } else if (state->color_mode == LAMP_COLOR_MODE_TEMPERATURE) {
        color_temperature_to_rgb(state->color_temperature_mireds, &frame);
    } else {
        hue_saturation_to_rgb(state->hue, state->saturation, &frame);
    }

    // A quadratic curve spreads the level range more evenly to the eye than
    // linear LED output, which saturates visually near the top.
    float level = (float)state->level / LAMP_LEVEL_MAX;
    frame.brightness = state->on ? level * level : 0.0f;
    return frame;
}

static bool frames_equal(lamp_frame_t a, lamp_frame_t b)
{
    return a.red == b.red && a.green == b.green && a.blue == b.blue
        && a.brightness == b.brightness;
}

static float lerp(float from, float to, float x)
{
    return from + (to - from) * x;
}

static lamp_frame_t interpolate_frames(lamp_frame_t from, lamp_frame_t to,
                                       transition_style_t style, float x)
{
    if (style == TRANSITION_THROUGH_BLACK) {
        lamp_frame_t frame = x < 0.5f ? from : to;
        frame.brightness = x < 0.5f ? from.brightness * (1.0f - 2.0f * x)
                                    : to.brightness * (2.0f * x - 1.0f);
        return frame;
    }

    // Fading from or to dark keeps the lit color instead of blending with the
    // meaningless color of an unlit frame.
    if (from.brightness == 0.0f) {
        from.red = to.red;
        from.green = to.green;
        from.blue = to.blue;
    } else if (to.brightness == 0.0f) {
        to.red = from.red;
        to.green = from.green;
        to.blue = from.blue;
    }
    return (lamp_frame_t){
        .red = lerp(from.red, to.red, x),
        .green = lerp(from.green, to.green, x),
        .blue = lerp(from.blue, to.blue, x),
        .brightness = lerp(from.brightness, to.brightness, x),
    };
}

static void render_task(void *unused)
{
    lamp_frame_t displayed = {0};
    lamp_frame_t from = displayed;
    lamp_frame_t to = displayed;
    transition_style_t style = TRANSITION_FADE;
    int64_t transition_started_us = 0;
    int64_t transition_duration_us = LAMP_STARTUP_FADE_US;
    bool animating = false;
    bool first_frame = true;

    while (true) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(animating ? LAMP_FRAME_INTERVAL_MS
                                                         : LAMP_IDLE_WAKE_MS));
        int64_t now_us = esp_timer_get_time();

        portENTER_CRITICAL(&s_lock);
        lamp_state_t state = s_state;
        transition_style_t next_style = s_next_transition;
        s_next_transition = TRANSITION_FADE;
        bool save_due = s_save_pending && now_us - s_last_change_us >= LAMP_SAVE_DELAY_US;
        if (save_due) {
            s_save_pending = false;
        }
        portEXIT_CRITICAL(&s_lock);

        lamp_frame_t target = frame_for_state(&state);
        if (first_frame || !frames_equal(target, to)) {
            from = displayed;
            to = target;
            style = first_frame ? TRANSITION_FADE : next_style;
            transition_duration_us = first_frame ? LAMP_STARTUP_FADE_US
                                   : style == TRANSITION_THROUGH_BLACK ? LAMP_STOCK_CHANGE_US
                                   : LAMP_FADE_US;
            transition_started_us = now_us;
            animating = true;
            first_frame = false;
        }

        if (animating) {
            float x = (float)(now_us - transition_started_us) / transition_duration_us;
            if (x >= 1.0f) {
                x = 1.0f;
                animating = false;
            }
            displayed = interpolate_frames(from, to, style, x);
            led_output_fill_scaled(displayed.red, displayed.green, displayed.blue,
                                   displayed.brightness);
        }

        if (save_due) {
            save_state(&state);
            ESP_LOGI(LOG_TAG, "Saved lamp state");
        }
    }
}

esp_err_t lamp_initialize(void)
{
    load_state();
    ESP_LOGI(LOG_TAG, "Lamp %s, level %u, %s, stock mode %s",
             s_state.on ? "on" : "off", s_state.level,
             s_state.color_mode == LAMP_COLOR_MODE_TEMPERATURE ? "temperature" : "hue/saturation",
             s_state.stock_mode ? "on" : "off");
    return ESP_OK;
}

esp_err_t lamp_start(void)
{
    BaseType_t created = xTaskCreate(render_task, "lamp-render", LAMP_RENDER_TASK_STACK_SIZE,
                                     NULL, LAMP_RENDER_TASK_PRIORITY, &s_render_task);
    return created == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t lamp_add_change_callback(lamp_change_callback_t callback, void *context)
{
    portENTER_CRITICAL(&s_lock);
    esp_err_t err = ESP_ERR_NO_MEM;
    if (s_listener_count < LAMP_MAX_CHANGE_CALLBACKS) {
        s_listeners[s_listener_count++] = (change_listener_t){callback, context};
        err = ESP_OK;
    }
    portEXIT_CRITICAL(&s_lock);
    return err;
}

lamp_state_t lamp_get_state(void)
{
    portENTER_CRITICAL(&s_lock);
    lamp_state_t state = s_state;
    portEXIT_CRITICAL(&s_lock);
    return state;
}

// Call with s_lock held after changing s_state.
static void mark_changed_locked(transition_style_t style, bool persist)
{
    s_next_transition = style;
    if (persist) {
        s_save_pending = true;
        s_last_change_us = esp_timer_get_time();
    }
}

static void notify_changed(const lamp_state_t *state)
{
    if (s_render_task != NULL) {
        xTaskNotifyGive(s_render_task);
    }

    portENTER_CRITICAL(&s_lock);
    int count = s_listener_count;
    change_listener_t listeners[LAMP_MAX_CHANGE_CALLBACKS];
    memcpy(listeners, s_listeners, sizeof(listeners));
    portEXIT_CRITICAL(&s_lock);
    for (int index = 0; index < count; ++index) {
        listeners[index].callback(state, listeners[index].context);
    }
}

void lamp_set_on(bool on)
{
    portENTER_CRITICAL(&s_lock);
    bool changed = s_state.on != on;
    s_state.on = on;
    if (changed) {
        mark_changed_locked(TRANSITION_FADE, true);
    }
    lamp_state_t state = s_state;
    portEXIT_CRITICAL(&s_lock);
    if (changed) {
        notify_changed(&state);
    }
}

void lamp_set_level(uint8_t level)
{
    level = clamp_u8(level, LAMP_LEVEL_MIN, LAMP_LEVEL_MAX);
    portENTER_CRITICAL(&s_lock);
    bool changed = s_state.level != level;
    s_state.level = level;
    if (changed) {
        mark_changed_locked(TRANSITION_FADE, true);
    }
    lamp_state_t state = s_state;
    portEXIT_CRITICAL(&s_lock);
    if (changed) {
        notify_changed(&state);
    }
}

void lamp_set_hue_saturation(uint8_t hue, uint8_t saturation)
{
    hue = clamp_u8(hue, 0, LAMP_HUE_MAX);
    saturation = clamp_u8(saturation, 0, LAMP_SATURATION_MAX);
    portENTER_CRITICAL(&s_lock);
    bool changed = s_state.stock_mode || s_state.color_mode != LAMP_COLOR_MODE_HUE_SATURATION
                || s_state.hue != hue || s_state.saturation != saturation;
    s_state.stock_mode = false;
    s_state.color_mode = LAMP_COLOR_MODE_HUE_SATURATION;
    s_state.hue = hue;
    s_state.saturation = saturation;
    if (changed) {
        mark_changed_locked(TRANSITION_FADE, true);
    }
    lamp_state_t state = s_state;
    portEXIT_CRITICAL(&s_lock);
    if (changed) {
        notify_changed(&state);
    }
}

void lamp_set_color_temperature(uint16_t mireds)
{
    mireds = clamp_mireds(mireds);
    portENTER_CRITICAL(&s_lock);
    bool changed = s_state.stock_mode || s_state.color_mode != LAMP_COLOR_MODE_TEMPERATURE
                || s_state.color_temperature_mireds != mireds;
    s_state.stock_mode = false;
    s_state.color_mode = LAMP_COLOR_MODE_TEMPERATURE;
    s_state.color_temperature_mireds = mireds;
    if (changed) {
        mark_changed_locked(TRANSITION_FADE, true);
    }
    lamp_state_t state = s_state;
    portEXIT_CRITICAL(&s_lock);
    if (changed) {
        notify_changed(&state);
    }
}

void lamp_set_stock_mode(bool enabled)
{
    portENTER_CRITICAL(&s_lock);
    bool changed = s_state.stock_mode != enabled;
    s_state.stock_mode = enabled;
    if (changed) {
        mark_changed_locked(TRANSITION_FADE, true);
    }
    lamp_state_t state = s_state;
    portEXIT_CRITICAL(&s_lock);
    if (changed) {
        notify_changed(&state);
    }
}

void lamp_set_stock_indication(lamp_stock_indication_t indication)
{
    portENTER_CRITICAL(&s_lock);
    bool changed = s_state.stock_indication != indication;
    s_state.stock_indication = indication;
    // The indication is live data, so it is not persisted.
    if (changed) {
        mark_changed_locked(TRANSITION_THROUGH_BLACK, false);
    }
    lamp_state_t state = s_state;
    portEXIT_CRITICAL(&s_lock);
    if (changed) {
        notify_changed(&state);
    }
}

const char *lamp_stock_indication_name(lamp_stock_indication_t indication)
{
    switch (indication) {
        case LAMP_STOCK_UP:
            return "up";
        case LAMP_STOCK_DOWN:
            return "down";
        case LAMP_STOCK_UNKNOWN:
        default:
            return "unknown";
    }
}
