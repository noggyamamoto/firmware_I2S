/*
 * ============================================================================
 * hal/hal_storage – implementação sobre drivers/drv_nvs
 * ============================================================================
 */
#include "hal_storage.h"

#include <string.h>

#include "config.h"
#include "drv_nvs.h"
#include "esp_log.h"

static const char *TAG = "storage";
static const char *NVS_NAMESPACE = "partitura";

bool hal_storage_init(void) {
    return drv_nvs_init();
}

void hal_storage_load_wifi(WifiCredentials *out) {
    memset(out, 0, sizeof(*out));
    strncpy(out->ssid, WIFI_DEFAULT_SSID, HAL_SSID_MAX);
    strncpy(out->password, WIFI_DEFAULT_PASS, HAL_PASS_MAX);

    char ssid[HAL_SSID_MAX + 1];
    if (drv_nvs_get_str(NVS_NAMESPACE, "ssid", ssid, sizeof(ssid)) && ssid[0] != '\0') {
        strncpy(out->ssid, ssid, HAL_SSID_MAX);
        if (!drv_nvs_get_str(NVS_NAMESPACE, "pass", out->password, sizeof(out->password))) {
            out->password[0] = '\0';
        }
        ESP_LOGI(TAG, "Credenciais carregadas da NVS (rede \"%s\")", out->ssid);
    }
}

bool hal_storage_save_wifi(const WifiCredentials *cred) {
    return drv_nvs_set_str(NVS_NAMESPACE, "ssid", cred->ssid) &&
           drv_nvs_set_str(NVS_NAMESPACE, "pass", cred->password);
}
