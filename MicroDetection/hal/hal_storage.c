/*
 * ============================================================================
 * hal/hal_storage – implementação sobre drivers/drv_nvs
 * ============================================================================
 */
#include "hal_storage.h"

#include <string.h>

#include "config.h"
#include "drv_nvs.h"

bool hal_storage_init(void) {
    return drv_nvs_init();
}

void hal_storage_default_wifi(WifiCredentials *out) {
    memset(out, 0, sizeof(*out));
    strncpy(out->ssid, WIFI_DEFAULT_SSID, HAL_SSID_MAX);
    strncpy(out->password, WIFI_DEFAULT_PASS, HAL_PASS_MAX);
}
