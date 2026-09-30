/*
 * ============================================================================
 * Implementação do gerenciador de Wi-Fi
 * ============================================================================
 */
#include "wifi_manager.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"

#include "app_state.h"
#include "config.h"
#include "indicators.h"

static const char *TAG = "wifi";

#define STA_RETRY_PERIOD_US     (15 * 1000000LL)    // Nova tentativa no modo AP a cada 15 s
#define MAX_SCAN_RESULTS        20

static esp_netif_t *s_sta_netif = NULL;
static esp_netif_t *s_ap_netif = NULL;
static esp_timer_handle_t s_retry_timer = NULL;
static WifiCredentials s_cred;
static volatile bool s_connected = false;
static volatile bool s_ap_active = false;
static volatile bool s_stopping = false;
static int s_retries = 0;
static char s_device_name[32];
static esp_netif_ip_info_t s_ip_info;

void wifi_manager_refresh_status(void) {
    if (s_connected) {
        indicators_set_status(app_state_is_paired() ? STATUS_PAIRED : STATUS_WIFI_OK);
    } else if (s_ap_active) {
        indicators_set_status(app_state_is_paired() ? STATUS_PAIRED : STATUS_AP_MODE);
    } else {
        indicators_set_status(STATUS_CONNECTING);
    }
}

/* Ativa a rede própria para que o app possa se conectar diretamente. */
static void start_ap_fallback(void) {
    if (s_ap_active) return;
    wifi_config_t ap = {0};
    strncpy((char *)ap.ap.ssid, s_device_name, sizeof(ap.ap.ssid) - 1);
    ap.ap.ssid_len = strlen(s_device_name);
    strncpy((char *)ap.ap.password, WIFI_AP_PASS, sizeof(ap.ap.password) - 1);
    ap.ap.channel = WIFI_AP_CHANNEL;
    ap.ap.max_connection = 2;
    ap.ap.authmode = strlen(WIFI_AP_PASS) >= 8 ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    esp_wifi_set_mode(WIFI_MODE_APSTA);
    esp_wifi_set_config(WIFI_IF_AP, &ap);
    s_ap_active = true;
    ESP_LOGW(TAG, "Rede \"%s\" indisponível. Rede própria ativa: \"%s\" (senha: %s, IP 192.168.4.1)",
             s_cred.ssid, s_device_name, WIFI_AP_PASS);
    esp_timer_start_periodic(s_retry_timer, STA_RETRY_PERIOD_US);
    wifi_manager_refresh_status();
}

static void retry_timer_cb(void *arg) {
    (void)arg;
    if (!s_connected && !s_stopping) esp_wifi_connect();
}

static void event_handler(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_connected = false;
        if (s_stopping) return;
        wifi_event_sta_disconnected_t *ev = (wifi_event_sta_disconnected_t *)data;
        if (!s_ap_active) {
            if (++s_retries <= WIFI_MAX_RETRIES) {
                ESP_LOGW(TAG, "Falha na conexão (motivo %d). Tentativa %d de %d...",
                         ev->reason, s_retries, WIFI_MAX_RETRIES);
                esp_wifi_connect();
            } else {
                start_ap_fallback();
            }
        }
        wifi_manager_refresh_status();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        s_ip_info = ev->ip_info;
        s_connected = true;
        s_retries = 0;
        esp_timer_stop(s_retry_timer);
        ESP_LOGI(TAG, "Wi-Fi conectado, IP: " IPSTR, IP2STR(&ev->ip_info.ip));
        wifi_manager_refresh_status();
        indicators_flash_rgb(0, 255, 0, 400);              // Confirmação visual verde
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STACONNECTED) {
        ESP_LOGI(TAG, "Um dispositivo entrou na rede própria");
    }
}

static void apply_sta_config(void) {
    wifi_config_t cfg = {0};
    strncpy((char *)cfg.sta.ssid, s_cred.ssid, sizeof(cfg.sta.ssid) - 1);
    strncpy((char *)cfg.sta.password, s_cred.password, sizeof(cfg.sta.password) - 1);
    cfg.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    cfg.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    esp_wifi_set_config(WIFI_IF_STA, &cfg);
}

static void log_printf(const char *fmt, ...) {
    char buf[160];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    ESP_LOGI(TAG, "%s", buf);
}

bool wifi_manager_start(const WifiCredentials *cred) {
    s_cred = *cred;
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_sta_netif = esp_netif_create_default_wifi_sta();
    s_ap_netif = esp_netif_create_default_wifi_ap();

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    if (esp_wifi_init(&init) != ESP_OK) return false;
    esp_wifi_set_ps(WIFI_PS_NONE);                          // Sem economia de energia: menor latência

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, event_handler, NULL));

    const esp_timer_create_args_t targs = {.callback = retry_timer_cb, .name = "wifi_retry"};
    ESP_ERROR_CHECK(esp_timer_create(&targs, &s_retry_timer));

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_device_name, sizeof(s_device_name), "%s-%02X%02X", DEVICE_NAME_PREFIX, mac[4], mac[5]);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    indicators_set_status(STATUS_CONNECTING);

    // RFE01: varre as redes antes de conectar
    int found = wifi_manager_scan(log_printf);
    ESP_LOGI(TAG, "%d redes encontradas", found);

    if (strlen(s_cred.ssid) == 0 || strcmp(s_cred.ssid, "SSID") == 0) {
        ESP_LOGW(TAG, "Nenhuma rede configurada (use o menu serial, opção 5)");
        start_ap_fallback();
        return true;
    }
    apply_sta_config();
    esp_wifi_connect();
    return true;
}

void wifi_manager_reconnect(const WifiCredentials *cred) {
    s_cred = *cred;
    s_retries = 0;
    if (s_ap_active) esp_timer_stop(s_retry_timer);
    s_ap_active = false;
    esp_wifi_disconnect();
    esp_wifi_set_mode(WIFI_MODE_STA);
    apply_sta_config();
    wifi_manager_refresh_status();
    esp_wifi_connect();
}

int wifi_manager_scan(wifi_print_fn print) {
    wifi_scan_config_t scan = {0};
    scan.show_hidden = false;
    esp_err_t err = esp_wifi_scan_start(&scan, true);       // Varredura bloqueante (~2 s)
    if (err != ESP_OK) {
        print("Varredura indisponível agora (%s)\n", esp_err_to_name(err));
        return 0;
    }
    uint16_t count = MAX_SCAN_RESULTS;
    wifi_ap_record_t records[MAX_SCAN_RESULTS];
    esp_wifi_scan_get_ap_records(&count, records);
    for (int i = 0; i < count; i++) {
        print("  %2d) %-32s %4d dBm  canal %2d%s\n", i + 1, (char *)records[i].ssid,
              records[i].rssi, records[i].primary,
              strcmp((char *)records[i].ssid, s_cred.ssid) == 0 ? "  <- configurada" : "");
    }
    return count;
}

bool wifi_manager_is_connected(void) { return s_connected; }
bool wifi_manager_ap_active(void) { return s_ap_active; }

int8_t wifi_manager_rssi(void) {
    wifi_ap_record_t info;
    if (s_connected && esp_wifi_sta_get_ap_info(&info) == ESP_OK) return info.rssi;
    return 0;
}

void wifi_manager_ip_str(char *buf, int len) {
    if (s_connected) {
        snprintf(buf, len, IPSTR, IP2STR(&s_ip_info.ip));
    } else if (s_ap_active) {
        snprintf(buf, len, "192.168.4.1 (AP)");
    } else {
        snprintf(buf, len, "sem IP");
    }
}

void wifi_manager_mac(uint8_t mac[6]) {
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
}

const char *wifi_manager_device_name(void) {
    return s_device_name;
}

void wifi_manager_stop(void) {
    s_stopping = true;
    if (s_retry_timer) esp_timer_stop(s_retry_timer);
    esp_wifi_disconnect();
    esp_wifi_stop();                                        // Para o Wi-Fi
    esp_wifi_deinit();                                      // Desinicializa o subsistema Wi-Fi
    s_connected = false;
    s_ap_active = false;
    indicators_set_status(STATUS_OFF);
}
