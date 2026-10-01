/*
 * ============================================================================
 * drivers/drv_wifi – implementação com esp_wifi / esp_netif do ESP-IDF
 * ============================================================================
 */
#include "drv_wifi.h"

#include <string.h>

#include "esp_event.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"

static drv_wifi_event_cb s_callback = NULL;

static void event_handler(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg;
    if (!s_callback) return;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *ev = (wifi_event_sta_disconnected_t *)data;
        s_callback(DRV_WIFI_EVENT_STA_DISCONNECTED, ev->reason);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        s_callback(DRV_WIFI_EVENT_STA_GOT_IP, ev->ip_info.ip.addr);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STACONNECTED) {
        s_callback(DRV_WIFI_EVENT_AP_CLIENT_JOINED, 0);
    }
}

bool drv_wifi_init(drv_wifi_event_cb callback) {
    s_callback = callback;
    if (esp_netif_init() != ESP_OK) return false;
    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return false;
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    if (esp_wifi_init(&init) != ESP_OK) return false;
    esp_wifi_set_ps(WIFI_PS_NONE);                          // Sem economia de energia: menor latência

    if (esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, event_handler, NULL) != ESP_OK ||
        esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, event_handler, NULL) != ESP_OK) {
        return false;
    }
    return esp_wifi_set_mode(WIFI_MODE_STA) == ESP_OK;
}

bool drv_wifi_start(void) {
    return esp_wifi_start() == ESP_OK;
}

void drv_wifi_set_station(const char *ssid, const char *password) {
    wifi_config_t cfg = {0};
    strncpy((char *)cfg.sta.ssid, ssid, sizeof(cfg.sta.ssid) - 1);
    strncpy((char *)cfg.sta.password, password, sizeof(cfg.sta.password) - 1);
    cfg.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    cfg.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    esp_wifi_set_config(WIFI_IF_STA, &cfg);
}

void drv_wifi_enable_ap(const char *ssid, const char *password, uint8_t channel) {
    wifi_config_t ap = {0};
    strncpy((char *)ap.ap.ssid, ssid, sizeof(ap.ap.ssid) - 1);
    ap.ap.ssid_len = strlen(ssid);
    strncpy((char *)ap.ap.password, password, sizeof(ap.ap.password) - 1);
    ap.ap.channel = channel;
    ap.ap.max_connection = 2;
    ap.ap.authmode = strlen(password) >= 8 ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    esp_wifi_set_mode(WIFI_MODE_APSTA);
    esp_wifi_set_config(WIFI_IF_AP, &ap);
}

void drv_wifi_disable_ap(void) {
    esp_wifi_set_mode(WIFI_MODE_STA);
}

void drv_wifi_connect(void) {
    esp_wifi_connect();
}

void drv_wifi_disconnect(void) {
    esp_wifi_disconnect();
}

int drv_wifi_scan(DrvWifiAp *out, int max) {
    wifi_scan_config_t scan = {0};
    scan.show_hidden = false;
    if (esp_wifi_scan_start(&scan, true) != ESP_OK) return -1;     // Bloqueante
    uint16_t count = 0;
    esp_wifi_scan_get_ap_num(&count);
    if (count > max) count = max;
    wifi_ap_record_t records[count > 0 ? count : 1];
    if (esp_wifi_scan_get_ap_records(&count, records) != ESP_OK) return -1;
    for (int i = 0; i < count; i++) {
        strncpy(out[i].ssid, (const char *)records[i].ssid, sizeof(out[i].ssid) - 1);
        out[i].ssid[sizeof(out[i].ssid) - 1] = '\0';
        out[i].rssi = records[i].rssi;
        out[i].channel = records[i].primary;
    }
    return count;
}

int8_t drv_wifi_rssi(void) {
    wifi_ap_record_t info;
    if (esp_wifi_sta_get_ap_info(&info) == ESP_OK) return info.rssi;
    return 0;
}

void drv_wifi_mac(uint8_t mac[6]) {
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
}

void drv_wifi_deinit(void) {
    esp_wifi_disconnect();
    esp_wifi_stop();                                        // Para o rádio
    esp_wifi_deinit();                                      // Libera o driver
}
