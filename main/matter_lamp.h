#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Starts Matter with two endpoints mirroring the lamp: an extended color light
// and an on/off plug-in unit for stock mode. Call after lamp_initialize() and
// once Wi-Fi is connected; the Wi-Fi manager keeps control of the station.
esp_err_t matter_lamp_start(void);

#ifdef __cplusplus
}
#endif
