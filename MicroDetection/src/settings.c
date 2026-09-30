/*
 * ============================================================================
 * Implementação das configurações persistentes
 * ============================================================================
 */
#include "settings.h"

#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "config.h"

static const char *TAG = "settings";
static const char *NVS_NAMESPACE = "partitura";

void settings_init(void) {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS inconsistente, apagando partição");
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}

void settings_load_wifi(WifiCredentials *out) {
    memset(out, 0, sizeof(*out));
    strncpy(out->ssid, WIFI_DEFAULT_SSID, SETTINGS_SSID_MAX);
    strncpy(out->password, WIFI_DEFAULT_PASS, SETTINGS_PASS_MAX);

    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return;   // Nada salvo ainda
    size_t len = sizeof(out->ssid);
    char ssid[SETTINGS_SSID_MAX + 1];
    if (nvs_get_str(h, "ssid", ssid, &len) == ESP_OK && ssid[0] != '\0') {
        strncpy(out->ssid, ssid, SETTINGS_SSID_MAX);
        len = sizeof(out->password);
        if (nvs_get_str(h, "pass", out->password, &len) != ESP_OK) out->password[0] = '\0';
        ESP_LOGI(TAG, "Credenciais carregadas da NVS (rede \"%s\")", out->ssid);
    }
    nvs_close(h);
}

bool settings_save_wifi(const WifiCredentials *cred) {
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return false;
    bool ok = nvs_set_str(h, "ssid", cred->ssid) == ESP_OK &&
              nvs_set_str(h, "pass", cred->password) == ESP_OK &&
              nvs_commit(h) == ESP_OK;
    nvs_close(h);
    return ok;
}
