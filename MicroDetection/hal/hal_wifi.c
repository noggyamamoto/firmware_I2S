/*
 * ============================================================================
 * hal/hal_wifi – política de conexão sobre drivers/drv_wifi
 * ============================================================================
 */
#include "hal_wifi.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"

#include "config.h"
#include "drv_wifi.h"
#include "hal_time.h"

static const char *TAG = "wifi";

#define STA_RETRY_PERIOD_US     (15 * 1000000LL)    // Nova tentativa no modo AP a cada 15 s
#define MAX_SCAN_RESULTS        20

static HalTimer s_retry_timer = NULL;
static WifiCredentials s_cred;
static hal_wifi_state_cb s_on_state = NULL;
static volatile bool s_connected = false;
static volatile bool s_ap_active = false;
static volatile bool s_stopping = false;
static int s_retries = 0;
static char s_device_name[32];
static uint32_t s_ip = 0;                           // IPv4 (ordem de rede)

static void notify(void) {
    if (s_on_state) s_on_state(hal_wifi_state());
}

/* Ativa a rede própria para que o app possa se conectar diretamente. */
static void start_ap_fallback(void) {
    if (s_ap_active) return;
    drv_wifi_enable_ap(s_device_name, WIFI_AP_PASS, WIFI_AP_CHANNEL);
    s_ap_active = true;
    ESP_LOGW(TAG, "Rede \"%s\" indisponível. Rede própria ativa: \"%s\" (senha: %s, IP 192.168.4.1)",
             s_cred.ssid, s_device_name, WIFI_AP_PASS);
    hal_timer_start_periodic(s_retry_timer, STA_RETRY_PERIOD_US);
    notify();
}

static void retry_timer_cb(void *arg) {
    (void)arg;
    if (!s_connected && !s_stopping) drv_wifi_connect();
}

static void on_event(DrvWifiEvent event, uint32_t arg) {
    switch (event) {
        case DRV_WIFI_EVENT_STA_DISCONNECTED:
            s_connected = false;
            if (s_stopping) return;
            if (!s_ap_active) {
                if (++s_retries <= WIFI_MAX_RETRIES) {
                    ESP_LOGW(TAG, "Falha na conexão (motivo %lu). Tentativa %d de %d...",
                             (unsigned long)arg, s_retries, WIFI_MAX_RETRIES);
                    drv_wifi_connect();
                } else {
                    start_ap_fallback();
                }
            }
            notify();
            break;

        case DRV_WIFI_EVENT_STA_GOT_IP:
            s_ip = arg;
            s_connected = true;
            s_retries = 0;
            hal_timer_stop(s_retry_timer);
            ESP_LOGI(TAG, "Wi-Fi conectado, IP: %u.%u.%u.%u", (unsigned)(arg & 0xFF),
                     (unsigned)((arg >> 8) & 0xFF), (unsigned)((arg >> 16) & 0xFF),
                     (unsigned)((arg >> 24) & 0xFF));
            notify();
            break;

        case DRV_WIFI_EVENT_AP_CLIENT_JOINED:
            ESP_LOGI(TAG, "Um dispositivo entrou na rede própria");
            break;
    }
}

static void log_printf(const char *fmt, ...) {
    char buf[160];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    ESP_LOGI(TAG, "%s", buf);
}

bool hal_wifi_start(const WifiCredentials *cred, hal_wifi_state_cb on_state) {
    s_cred = *cred;
    s_on_state = on_state;
    if (!drv_wifi_init(on_event)) return false;

    s_retry_timer = hal_timer_create(retry_timer_cb, NULL, "wifi_retry");
    if (!s_retry_timer) return false;

    uint8_t mac[6];
    drv_wifi_mac(mac);
    snprintf(s_device_name, sizeof(s_device_name), "%s-%02X%02X", DEVICE_NAME_PREFIX, mac[4], mac[5]);

    if (!drv_wifi_start()) return false;
    notify();

    // RFE01: varre as redes antes de conectar
    int found = hal_wifi_scan(log_printf);
    ESP_LOGI(TAG, "%d redes encontradas", found);

    if (strlen(s_cred.ssid) == 0 || strcmp(s_cred.ssid, "SSID") == 0) {
        ESP_LOGW(TAG, "Nenhuma rede configurada (use o menu serial, opção 5)");
        start_ap_fallback();
        return true;
    }
    drv_wifi_set_station(s_cred.ssid, s_cred.password);
    drv_wifi_connect();
    return true;
}

void hal_wifi_reconnect(const WifiCredentials *cred) {
    s_cred = *cred;
    s_retries = 0;
    if (s_ap_active) hal_timer_stop(s_retry_timer);
    s_ap_active = false;
    drv_wifi_disconnect();
    drv_wifi_disable_ap();
    drv_wifi_set_station(s_cred.ssid, s_cred.password);
    notify();
    drv_wifi_connect();
}

int hal_wifi_scan(hal_print_fn print) {
    DrvWifiAp records[MAX_SCAN_RESULTS];
    int count = drv_wifi_scan(records, MAX_SCAN_RESULTS);
    if (count < 0) {
        print("Varredura indisponível agora\n");
        return 0;
    }
    for (int i = 0; i < count; i++) {
        print("  %2d) %-32s %4d dBm  canal %2d%s\n", i + 1, records[i].ssid, records[i].rssi,
              records[i].channel,
              strcmp(records[i].ssid, s_cred.ssid) == 0 ? "  <- configurada" : "");
    }
    return count;
}

HalWifiState hal_wifi_state(void) {
    if (s_connected) return HAL_WIFI_CONNECTED;
    if (s_ap_active) return HAL_WIFI_AP_ONLY;
    return HAL_WIFI_CONNECTING;
}

bool hal_wifi_is_connected(void) { return s_connected; }
bool hal_wifi_ap_active(void) { return s_ap_active; }

int8_t hal_wifi_rssi(void) {
    return s_connected ? drv_wifi_rssi() : 0;
}

void hal_wifi_ip_str(char *buf, int len) {
    if (s_connected) {
        snprintf(buf, len, "%u.%u.%u.%u", (unsigned)(s_ip & 0xFF), (unsigned)((s_ip >> 8) & 0xFF),
                 (unsigned)((s_ip >> 16) & 0xFF), (unsigned)((s_ip >> 24) & 0xFF));
    } else if (s_ap_active) {
        snprintf(buf, len, "192.168.4.1 (AP)");
    } else {
        snprintf(buf, len, "sem IP");
    }
}

void hal_wifi_mac(uint8_t mac[6]) {
    drv_wifi_mac(mac);
}

const char *hal_wifi_device_name(void) {
    return s_device_name;
}

void hal_wifi_stop(void) {
    s_stopping = true;
    if (s_retry_timer) hal_timer_stop(s_retry_timer);
    drv_wifi_deinit();
    s_connected = false;
    s_ap_active = false;
    notify();
}
