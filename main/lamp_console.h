#pragma once

#include "esp_err.h"

// Starts a serial console on the USB port with a `lamp` command for testing
// the lamp state without a smart-home controller.
esp_err_t lamp_console_start(void);
