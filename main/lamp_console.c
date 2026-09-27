#include "lamp_console.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_console.h"
#include "lamp.h"

static const char *LOG_TAG = "lamp-console";

static void print_state(void)
{
    lamp_state_t state = lamp_get_state();
    printf("power: %s\n", state.on ? "on" : "off");
    printf("level: %u/%u (%ld%%)\n", state.level, LAMP_LEVEL_MAX,
           lroundf(state.level * 100.0f / LAMP_LEVEL_MAX));
    if (state.color_mode == LAMP_COLOR_MODE_TEMPERATURE) {
        printf("color: %u mireds (%ld K)\n", state.color_temperature_mireds,
               lroundf(1000000.0f / state.color_temperature_mireds));
    } else {
        printf("color: hue %u (%ld deg), saturation %u (%ld%%)\n", state.hue,
               lroundf(state.hue * 360.0f / LAMP_HUE_MAX), state.saturation,
               lroundf(state.saturation * 100.0f / LAMP_SATURATION_MAX));
    }
    printf("stock mode: %s, indication: %s\n", state.stock_mode ? "on" : "off",
           lamp_stock_indication_name(state.stock_indication));
}

static int usage(void)
{
    printf("usage:\n"
           "  lamp                    show state\n"
           "  lamp on | off\n"
           "  lamp level <1-100>      brightness percent\n"
           "  lamp hs <0-359> <0-100> hue degrees, saturation percent\n"
           "  lamp ct <2000-6500>     color temperature in kelvin\n"
           "  lamp stock on | off\n"
           "  lamp stock up | down | unknown   simulate a stock result\n"
           "  lamp identify [seconds]  blink the ring (default 5, 0 stops)\n");
    return 1;
}

static int lamp_command(int argc, char **argv)
{
    if (argc == 1) {
        print_state();
        return 0;
    }

    const char *action = argv[1];
    if (argc == 2 && strcmp(action, "on") == 0) {
        lamp_set_on(true);
    } else if (argc == 2 && strcmp(action, "off") == 0) {
        lamp_set_on(false);
    } else if (argc == 3 && strcmp(action, "level") == 0) {
        int percent = atoi(argv[2]);
        if (percent < 1 || percent > 100) {
            return usage();
        }
        lamp_set_level(lroundf(percent * LAMP_LEVEL_MAX / 100.0f));
    } else if (argc == 4 && strcmp(action, "hs") == 0) {
        int degrees = atoi(argv[2]);
        int percent = atoi(argv[3]);
        if (degrees < 0 || degrees > 359 || percent < 0 || percent > 100) {
            return usage();
        }
        lamp_set_hue_saturation(lroundf(degrees * LAMP_HUE_MAX / 360.0f),
                                lroundf(percent * LAMP_SATURATION_MAX / 100.0f));
    } else if (argc == 3 && strcmp(action, "ct") == 0) {
        int kelvin = atoi(argv[2]);
        if (kelvin < 2000 || kelvin > 6500) {
            return usage();
        }
        lamp_set_color_temperature(lroundf(1000000.0f / kelvin));
    } else if ((argc == 2 || argc == 3) && strcmp(action, "identify") == 0) {
        int seconds = argc == 3 ? atoi(argv[2]) : 5;
        if (seconds < 0 || seconds > 300) {
            return usage();
        }
        lamp_identify(seconds * 1000);
        return 0;
    } else if (argc == 3 && strcmp(action, "stock") == 0) {
        const char *value = argv[2];
        if (strcmp(value, "on") == 0) {
            lamp_set_stock_mode(true);
        } else if (strcmp(value, "off") == 0) {
            lamp_set_stock_mode(false);
        } else if (strcmp(value, "up") == 0) {
            lamp_set_stock_indication(LAMP_STOCK_UP);
        } else if (strcmp(value, "down") == 0) {
            lamp_set_stock_indication(LAMP_STOCK_DOWN);
        } else if (strcmp(value, "unknown") == 0) {
            lamp_set_stock_indication(LAMP_STOCK_UNKNOWN);
        } else {
            return usage();
        }
    } else {
        return usage();
    }

    print_state();
    return 0;
}

esp_err_t lamp_console_start(void)
{
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_config.prompt = "lamp>";
    esp_console_dev_usb_serial_jtag_config_t device_config =
        ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_console_new_repl_usb_serial_jtag(&device_config, &repl_config, &repl),
                        LOG_TAG, "Unable to create console");

    const esp_console_cmd_t command = {
        .command = "lamp",
        .help = "Show or change the lamp state; run `lamp help` for usage",
        .func = lamp_command,
    };
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&command), LOG_TAG,
                        "Unable to register lamp command");
    ESP_RETURN_ON_ERROR(esp_console_register_help_command(), LOG_TAG,
                        "Unable to register help command");
    return esp_console_start_repl(repl);
}
