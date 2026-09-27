#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <math.h>

#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_app_desc.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "lamp.h"
#include "lamp_console.h"
#include "led_output.h"
#include "matter_lamp.h"
#include "pixel.h"
#include "usb_source.h"
#include "wifi_manager.h"

// Test mode uses a random number instead of performing an HTTP request
#define LED_TEST_MODE 0

#define DATA_QUERY_TASK_STACK_SIZE 12288
#define DATA_QUERY_TASK_PRIORITY 5
#define WIFI_SETUP_WAVE_PERIOD_US 4000000
#define WIFI_SETUP_WAVE_MIN_INTENSITY 0.15
#define VERSION_DISPLAY_HOLD_MS 1200
#define VERSION_DISPLAY_PAUSE_MS 350

/* app state machine */
typedef enum application_state {
    application_state_start,
    application_state_offline,
    application_state_online,
} application_state;

#define ENUM_TO_STRING_CASE(x) case x: return #x
char *application_state_label_for_value(application_state state);
char *application_state_label_for_value(application_state state) {
    switch (state) {
        ENUM_TO_STRING_CASE(application_state_start);
        ENUM_TO_STRING_CASE(application_state_offline);
        ENUM_TO_STRING_CASE(application_state_online);
    }
    return NULL;
}

/* app state data */
typedef struct application_data {
    application_state state;
    esp_timer_handle_t periodic_timer;
    TaskHandle_t data_query_task;
    bool timer_is_armed;
} application_data_t;

static application_data_t application_data;

typedef struct stock_data {
    bool have_valid_value;
    double value;
} stock_data_t;

/* prototypes */
static void periodic_timer_callback(void* arg);
static void data_query_task(void *arg);
static void request_stock_data_update(application_data_t *app_data);
static void start_timer(application_data_t *app_data);
static void stop_timer(application_data_t *app_data);
esp_err_t _http_event_handle_nasdaq(esp_http_client_event_t *evt);
static void query_stock_data_and_update_lamp(void);
static void lamp_changed(const lamp_state_t *state, void *context);
static void wifi_status_changed(wifi_setup_status_t status);
static void show_usb_power_capability(usb_source_measurement_t measurement);
static void show_firmware_version(void);
void application_transition_to_state(application_state *current_state, application_state new_state);
void get_stock_data_nasdaq(stock_data_t *stock_data);

static const char *LOG_TAG = "color-lamp-stand-app";

void application_transition_to_state(application_state *current_state, application_state new_state) {
    ESP_LOGI(LOG_TAG, "*** Application state transition from %s to %s\n", application_state_label_for_value(*current_state), application_state_label_for_value(new_state));
    *current_state = new_state;
}


void wifi_event_handler(void *event_handler_arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
    ESP_LOGI(LOG_TAG, "Wifi event handler, event id %d, event data %p", event_id, event_data);
    application_data_t *app_data = event_handler_arg;

    if (event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(LOG_TAG, "WIFI_EVENT_STA_START");
    } else if (event_id == WIFI_EVENT_STA_CONNECTED) {
        ESP_LOGI(LOG_TAG, "WIFI_EVENT_STA_CONNECTED");
    } else if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGI(LOG_TAG, "WIFI_EVENT_STA_DISCONNECTED");
        application_transition_to_state(&app_data->state, application_state_offline);
        stop_timer(app_data);
    }    
}

void ip_event_handler(void *event_handler_arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
    ESP_LOGI(LOG_TAG, "IP event handler, event id %d, event data %p", event_id, event_data);
    application_data_t *app_data = event_handler_arg;

    if (event_id == IP_EVENT_STA_GOT_IP) {
        ESP_LOGI(LOG_TAG, "IP_EVENT_STA_GOT_IP");

        application_transition_to_state(&app_data->state, application_state_online);
        request_stock_data_update(app_data);
        start_timer(app_data);
    } else if (0) {

    }    

}


void app_main(void)
{
    ESP_LOGI(LOG_TAG, "Starting color lamp stand");

    usb_source_measurement_t usb_source = usb_source_measure();

    ESP_ERROR_CHECK(led_output_initialize(usb_source.maximum_led_brightness));
    led_output_fill(pixel_color_black);
    show_usb_power_capability(usb_source);
    show_firmware_version();

    const esp_timer_create_args_t periodic_timer_args = {
            .callback = &periodic_timer_callback,
            .arg = (void *) &application_data,
            .name = "periodic"
    };

    ESP_ERROR_CHECK(esp_timer_create(&periodic_timer_args, &application_data.periodic_timer));
    BaseType_t task_created = xTaskCreate(data_query_task, "data_query",
                                         DATA_QUERY_TASK_STACK_SIZE, &application_data,
                                         DATA_QUERY_TASK_PRIORITY,
                                         &application_data.data_query_task);
    ESP_ERROR_CHECK(task_created == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);

    ESP_ERROR_CHECK(wifi_manager_initialize_and_connect(wifi_status_changed));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               wifi_event_handler, &application_data));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               ip_event_handler, &application_data));
    wifi_manager_start_reset_button_monitor(wifi_status_changed);

    // The Wi-Fi manager has initialized NVS by the time it returns.
    ESP_ERROR_CHECK(lamp_initialize());
    ESP_ERROR_CHECK(lamp_add_change_callback(lamp_changed, &application_data));
    ESP_ERROR_CHECK(lamp_start());
    ESP_ERROR_CHECK(lamp_console_start());
    // The lamp and stock indicator keep working if Matter cannot start.
    esp_err_t matter_err = matter_lamp_start();
    if (matter_err != ESP_OK) {
        ESP_LOGE(LOG_TAG, "Matter unavailable: %s", esp_err_to_name(matter_err));
    }

    application_transition_to_state(&application_data.state, application_state_online);
    request_stock_data_update(&application_data);
    start_timer(&application_data);

}

static void wifi_status_changed(wifi_setup_status_t status)
{
    static int64_t portal_started_us = -1;
    if (status != WIFI_SETUP_STATUS_PORTAL) {
        portal_started_us = -1;
    }
    pixel_color_t color = pixel_color_black;
    switch (status) {
        case WIFI_SETUP_STATUS_CONNECTING:
            color = (pixel_color_t){.brightness = 6, .b = 0xff};
            break;
        case WIFI_SETUP_STATUS_PORTAL: {
            int64_t now_us = esp_timer_get_time();
            if (portal_started_us < 0) {
                portal_started_us = now_us;
            }
            double phase = (double)((now_us - portal_started_us) % WIFI_SETUP_WAVE_PERIOD_US)
                           / WIFI_SETUP_WAVE_PERIOD_US;
            // Keep a faint floor so the ring remains visible behind the crest.
            // Scale channels, not the five-bit brightness, to avoid visible steps
            // and a clipped, flat peak on low-power USB sources.
            pixel_color_t pixels[LED_COUNT];
            for (int pixel = 0; pixel < LED_COUNT; ++pixel) {
                // Divide by LED_COUNT, not LED_COUNT - 1, so the wrap from
                // D32 to D1 has the same phase spacing as every other pair.
                double position = (double)pixel / LED_COUNT;
                double intensity = WIFI_SETUP_WAVE_MIN_INTENSITY +
                    (1.0 - WIFI_SETUP_WAVE_MIN_INTENSITY) *
                    (0.5 + 0.5 * cos((position - phase) * 2.0 * M_PI));
                pixels[pixel] = (pixel_color_t){.brightness = 6,
                                                .r = (uint8_t)lround(255 * intensity),
                                                .g = (uint8_t)lround(96 * intensity)};
            }
            led_output_show_pixels(pixels);
            return;
        }
        case WIFI_SETUP_STATUS_TESTING:
            color = (pixel_color_t){.brightness = 12, .b = 0xff};
            break;
        case WIFI_SETUP_STATUS_TESTING_DIM:
            color = (pixel_color_t){.brightness = 3, .b = 0xff};
            break;
        case WIFI_SETUP_STATUS_FAILED:
            color = (pixel_color_t){.brightness = 8, .r = 0xff};
            break;
        case WIFI_SETUP_STATUS_CONNECTED:
            color = (pixel_color_t){.brightness = 8, .g = 0xff};
            break;
        case WIFI_SETUP_STATUS_RESETTING:
            color = (pixel_color_t){.brightness = 15, .r = 0xff};
            break;
    }
    led_output_fill(color);
}

static void lamp_changed(const lamp_state_t *state, void *context)
{
    static bool stock_mode_was_on = true;
    application_data_t *app_data = context;
    if (state->stock_mode && !stock_mode_was_on) {
        request_stock_data_update(app_data);
    }
    stock_mode_was_on = state->stock_mode;
}

static void start_timer(application_data_t *app_data) {
    if (!app_data->timer_is_armed) {
        ESP_LOGI(LOG_TAG, "Starting timer");
#if LED_TEST_MODE
        ESP_ERROR_CHECK(esp_timer_start_periodic(app_data->periodic_timer, 5 * 1000000));
#else
        ESP_ERROR_CHECK(esp_timer_start_periodic(app_data->periodic_timer, 5 * 60 * 1000000));
#endif
        app_data->timer_is_armed = true;
    }
}

static void stop_timer(application_data_t *app_data) {
    if (app_data->timer_is_armed) {
        ESP_LOGI(LOG_TAG, "Stopping timer");
        ESP_ERROR_CHECK(esp_timer_stop(app_data->periodic_timer));
        app_data->timer_is_armed = false;
    }
}

static void periodic_timer_callback(void* arg)
{
    int64_t time_since_boot = esp_timer_get_time();
    ESP_LOGI(LOG_TAG, "Periodic timer called, time since boot: %lld us", time_since_boot);
    application_data_t *app_data = arg;
    request_stock_data_update(app_data);
}

static void request_stock_data_update(application_data_t *app_data)
{
    xTaskNotifyGive(app_data->data_query_task);
}

static void data_query_task(void *arg)
{
    (void)arg;
    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        query_stock_data_and_update_lamp();
        ESP_LOGI(LOG_TAG, "Data query task minimum free stack: %u bytes",
                 (unsigned int)uxTaskGetStackHighWaterMark(NULL));
    }
}

static void query_stock_data_and_update_lamp(void) {
    if (!lamp_get_state().stock_mode) {
        ESP_LOGI(LOG_TAG, "Stock mode off, skipping query");
        return;
    }
    ESP_LOGI(LOG_TAG, "Querying data");

    stock_data_t data;
    bzero(&data, sizeof(data));

#if LED_TEST_MODE
    data.have_valid_value = true;
    data.value = esp_random() % 2 ? -1.0 : 1.0;
#else
    get_stock_data_nasdaq(&data);
#endif

    lamp_stock_indication_t indication = LAMP_STOCK_UNKNOWN;
    if (data.have_valid_value) {
        ESP_LOGI(LOG_TAG, "New value %f", data.value);
        indication = data.value < 0 ? LAMP_STOCK_DOWN : LAMP_STOCK_UP;
    } else {
        ESP_LOGE(LOG_TAG, "Unable to get valid value");
    }
    lamp_set_stock_indication(indication);
}

static void show_usb_power_capability(usb_source_measurement_t measurement)
{
    if (!measurement.measurement_valid) {
        pixel_color_t error = {.brightness = 10, .r = 0xff};
        for (int flash = 0; flash < 2; ++flash) {
            led_output_fill(error);
            vTaskDelay(pdMS_TO_TICKS(180));
            led_output_fill(pixel_color_black);
            vTaskDelay(pdMS_TO_TICKS(180));
        }
    }

    int illuminated_pixels;
    pixel_color_t gauge_color;
    switch (measurement.current) {
        case USB_SOURCE_CURRENT_3_A:
            illuminated_pixels = LED_COUNT;
            gauge_color = (pixel_color_t){.brightness = 10, .g = 0xff};
            break;
        case USB_SOURCE_CURRENT_1_5_A:
            illuminated_pixels = LED_COUNT / 2;
            gauge_color = (pixel_color_t){.brightness = 10, .g = 0x80, .b = 0xff};
            break;
        case USB_SOURCE_CURRENT_DEFAULT:
        default:
            illuminated_pixels = LED_COUNT / 4;
            gauge_color = (pixel_color_t){.brightness = 10, .r = 0xff, .g = 0x60};
            break;
    }

    pixel_color_t pixels[LED_COUNT] = {0};
    TickType_t sweep_delay = pdMS_TO_TICKS(500 / illuminated_pixels);
    for (int pixel = 0; pixel < illuminated_pixels; ++pixel) {
        pixels[pixel] = gauge_color;
        led_output_show_pixels(pixels);
        vTaskDelay(sweep_delay);
    }
    vTaskDelay(pdMS_TO_TICKS(1000));

    uint8_t starting_brightness = gauge_color.brightness;
    if (starting_brightness > led_output_maximum_brightness()) {
        starting_brightness = led_output_maximum_brightness();
    }
    for (int brightness = starting_brightness; brightness >= 0; --brightness) {
        for (int pixel = 0; pixel < illuminated_pixels; ++pixel) {
            pixels[pixel].brightness = brightness;
        }
        led_output_show_pixels(pixels);
        vTaskDelay(pdMS_TO_TICKS(70));
    }
}

static void show_firmware_version(void)
{
    const char *version = esp_app_get_description()->version;
    unsigned int parts[3];
    char trailing;
    if (sscanf(version, "%u.%u.%u%c", &parts[0], &parts[1], &parts[2], &trailing) != 3) {
        ESP_LOGW(LOG_TAG, "Skipping LED version display: invalid version '%s'", version);
        return;
    }
    for (int part = 0; part < 3; ++part) {
        if (parts[part] > LED_COUNT) {
            ESP_LOGW(LOG_TAG, "Skipping LED version display: '%s' exceeds %d LEDs",
                     version, LED_COUNT);
            return;
        }
    }

    const pixel_color_t backgrounds[3] = {
        // Dim channels rather than brightness so the contrast survives the USB cap.
        {.brightness = 6, .b = 0x55},
        {.brightness = 6, .r = 0x2b, .b = 0x55},
        {.brightness = 6, .r = 0x55, .g = 0x20},
    };
    const pixel_color_t white = {.brightness = 6, .r = 0xff, .g = 0xff, .b = 0xff};
    pixel_color_t pixels[LED_COUNT];

    ESP_LOGI(LOG_TAG, "Displaying firmware version %s", version);
    vTaskDelay(pdMS_TO_TICKS(VERSION_DISPLAY_PAUSE_MS));
    for (int part = 0; part < 3; ++part) {
        // The colored remainder identifies the part even when its count is zero.
        for (int pixel = 0; pixel < LED_COUNT; ++pixel) {
            pixels[pixel] = (unsigned int)pixel < parts[part] ? white : backgrounds[part];
        }
        led_output_show_pixels(pixels);
        vTaskDelay(pdMS_TO_TICKS(VERSION_DISPLAY_HOLD_MS));
        led_output_fill(pixel_color_black);
        vTaskDelay(pdMS_TO_TICKS(VERSION_DISPLAY_PAUSE_MS));
    }
}

void get_stock_data_nasdaq(stock_data_t *stock_data) {
    esp_http_client_config_t config = {
        .url = "https://api.nasdaq.com/api/quote/AAPL/info?assetclass=stocks",
        .event_handler = _http_event_handle_nasdaq,
        .user_data = stock_data,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_http_client_set_header(client, "Accept", "application/json, text/plain, */*");
    esp_http_client_set_header(client, "Origin", "https://www.nasdaq.com");
    esp_http_client_set_header(client, "Host", "api.nasdaq.com");
    esp_http_client_set_header(client, "Referer", "https://www.nasdaq.com/market-activity/stocks/aapl/real-time");
    esp_http_client_set_header(client, "Connection", "keep-alive");
    esp_http_client_set_header(client, "User-Agent", "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/13.0.2 Safari/605.1.15");
    esp_http_client_set_header(client, "Accept-Language", "en-us");

    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        ESP_LOGI(LOG_TAG, "Status = %d, content_length = %d", esp_http_client_get_status_code(client), esp_http_client_get_content_length(client));
    } else {
        ESP_LOGE(LOG_TAG, "Error performing HTTP request: %d", err);
    }
    esp_http_client_cleanup(client);
}

esp_err_t _http_event_handle_nasdaq(esp_http_client_event_t *evt) {
    stock_data_t *data = evt->user_data;

    switch(evt->event_id) {
        case HTTP_EVENT_HEADER_SENT:
        case HTTP_EVENT_ON_HEADER:
        case HTTP_EVENT_ON_HEADERS_COMPLETE:
        case HTTP_EVENT_ON_STATUS_CODE:
        case HTTP_EVENT_ON_CONNECTED:
        case HTTP_EVENT_ON_FINISH:
        case HTTP_EVENT_DISCONNECTED:
        case HTTP_EVENT_REDIRECT:
            break;

        case HTTP_EVENT_ERROR:
            ESP_LOGE(LOG_TAG, "HTTP_EVENT_ERROR");
            break;
        case HTTP_EVENT_ON_DATA:
            ESP_LOGI(LOG_TAG, "HTTP_EVENT_ON_DATA, len=%d chunked=%d", evt->data_len, esp_http_client_is_chunked_response(evt->client));
            ESP_LOGI(LOG_TAG, "%.*s\n", evt->data_len, (char*)evt->data);
            if (!(data->have_valid_value)) {
                char *match = memmem(evt->data, evt->data_len, "\"deltaIndicator\":", strlen("\"deltaIndicator\":"));
                if (match) {
                    bool up_match = memmem(evt->data, evt->data_len, "\"deltaIndicator\":\"up\"", strlen("\"deltaIndicator\":\"up\"")) != NULL;
                    bool down_match = memmem(evt->data, evt->data_len, "\"deltaIndicator\":\"down\"", strlen("\"deltaIndicator\":\"down\"")) != NULL;
                    if (up_match) {
                        ESP_LOGI(LOG_TAG, "Found 'up' indicator");
                        data->have_valid_value = true;
                        data->value = 1.0;
                    } else if (down_match) {
                        ESP_LOGI(LOG_TAG, "Found 'down' indicator");
                        data->have_valid_value = true;
                        data->value = -1.0;
                    } else {
                        ESP_LOGE(LOG_TAG, "Unable to find 'up' or 'down' value:");
                        ESP_LOGE(LOG_TAG, "%.*s", evt->data_len, (char*)evt->data);
                    }
                }                
            }
            break;
    }
    return ESP_OK;
}
