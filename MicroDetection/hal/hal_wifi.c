/*
 * ============================================================================
 * hal/hal_wifi – política de conexão sobre drivers/drv_wifi
 *
 * Os eventos do rádio chegam pela tarefa de eventos do ESP-IDF, os
 * temporizadores pela tarefa do esp_timer e os comandos (varredura, nova
 * rede) pela tarefa do menu. O estado compartilhado é protegido por um
 * spinlock (seções curtas, sem chamadas bloqueantes dentro).
 * ============================================================================
 */
#include "hal_wifi.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_wifi_types.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "config.h"
#include "drv_wifi.h"
#include "hal_time.h"

static const char *TAG = "wifi";

#define AP_RETRY_PERIOD_US      (15 * 1000000LL)    // Nova tentativa no modo AP a cada 15 s
#define MAX_SCAN_RESULTS        20
#define WEAK_RSSI_DBM           (-80)               // Abaixo disso a conexão fica instável

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
#define LOCK()   portENTER_CRITICAL(&s_lock)
#define UNLOCK() portEXIT_CRITICAL(&s_lock)

static HalTimer s_retry_timer = NULL;               // Backoff entre tentativas / modo AP
static HalTimer s_dhcp_timer = NULL;                // Associado, mas sem IP
static WifiCredentials s_cred;
static hal_wifi_state_cb s_on_state = NULL;
static volatile bool s_connected = false;
static volatile bool s_ap_active = false;
static volatile bool s_stopping = false;
static volatile bool s_scanning = false;
static volatile bool s_dhcp_failed = false;
static int s_retries = 0;
static int s_ap_clients = 0;
static HalWifiError s_error = HAL_WIFI_ERR_NONE;
static uint16_t s_reason = 0;
static char s_device_name[32];
static uint32_t s_ip = 0;                           // IPv4 (ordem de rede)

// Última varredura e o que ela diz sobre a rede configurada
static DrvWifiAp s_scan[MAX_SCAN_RESULTS];
static int s_scan_count = 0;
static bool s_seen = false;
static int8_t s_seen_rssi = 0;
static uint8_t s_seen_channel = 0;
static DrvWifiAuth s_seen_auth = DRV_WIFI_AUTH_OTHER;
static char s_similar[33] = "";

static void notify(void) {
    if (s_on_state) s_on_state(hal_wifi_state());
}

static bool configured(void) {
    return s_cred.ssid[0] != '\0' && strcmp(s_cred.ssid, WIFI_DEFAULT_SSID) != 0;
}

/* Compara ignorando maiúsculas/minúsculas e espaços nas pontas. */
static bool similar_name(const char *a, const char *b) {
    while (*a == ' ') a++;
    while (*b == ' ') b++;
    size_t la = strlen(a), lb = strlen(b);
    while (la > 0 && a[la - 1] == ' ') la--;
    while (lb > 0 && b[lb - 1] == ' ') lb--;
    if (la != lb || la == 0) return false;
    for (size_t i = 0; i < la; i++) {
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) return false;
    }
    return true;
}

/* Procura a rede configurada no resultado da última varredura. */
static void update_scan_info(void) {
    bool seen = false;
    int8_t rssi = -127;
    uint8_t channel = 0;
    DrvWifiAuth auth = DRV_WIFI_AUTH_OTHER;
    char similar[33] = "";
    for (int i = 0; i < s_scan_count; i++) {
        if (strcmp(s_scan[i].ssid, s_cred.ssid) == 0) {
            if (!seen || s_scan[i].rssi > rssi) {           // Repetidores: o mais forte
                rssi = s_scan[i].rssi;
                channel = s_scan[i].channel;
                auth = s_scan[i].auth;
            }
            seen = true;
        } else if (similar[0] == '\0' && similar_name(s_scan[i].ssid, s_cred.ssid)) {
            strncpy(similar, s_scan[i].ssid, sizeof(similar) - 1);
        }
    }
    LOCK();
    s_seen = seen;
    s_seen_rssi = seen ? rssi : 0;
    s_seen_channel = channel;
    s_seen_auth = auth;
    memcpy(s_similar, seen ? "" : similar, sizeof(s_similar));
    UNLOCK();
}

/* Traduz o código de desconexão do ESP-IDF em uma causa compreensível. */
static HalWifiError classify(uint16_t reason, bool was_connected) {
    bool weak = s_seen && s_seen_rssi < WEAK_RSSI_DBM;
    switch (reason) {
        case WIFI_REASON_NO_AP_FOUND:
            return HAL_WIFI_ERR_SSID_NOT_FOUND;
        case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
        case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
        case WIFI_REASON_802_1X_AUTH_FAILED:
        case WIFI_REASON_AKMP_INVALID:
        case WIFI_REASON_PAIRWISE_CIPHER_INVALID:
        case WIFI_REASON_GROUP_CIPHER_INVALID:
        case WIFI_REASON_CIPHER_SUITE_REJECTED:
        case WIFI_REASON_BAD_CIPHER_OR_AKM:
            return HAL_WIFI_ERR_SECURITY;
        case WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD:
            return HAL_WIFI_ERR_WEAK_SIGNAL;
        case WIFI_REASON_AUTH_FAIL:
        case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
        case WIFI_REASON_HANDSHAKE_TIMEOUT:
        case WIFI_REASON_MIC_FAILURE:
            // O handshake falha tanto com senha errada quanto com sinal péssimo
            return weak ? HAL_WIFI_ERR_WEAK_SIGNAL : HAL_WIFI_ERR_WRONG_PASSWORD;
        case WIFI_REASON_AUTH_EXPIRE:
        case WIFI_REASON_ASSOC_TOOMANY:
        case WIFI_REASON_ASSOC_FAIL:
        case WIFI_REASON_CONNECTION_FAIL:
        case WIFI_REASON_ASSOC_NOT_AUTHED:
        case WIFI_REASON_NOT_AUTHORIZED_THIS_LOCATION:
            return weak ? HAL_WIFI_ERR_WEAK_SIGNAL : HAL_WIFI_ERR_REJECTED;
        case WIFI_REASON_BEACON_TIMEOUT:
        case WIFI_REASON_AUTH_LEAVE:
        case WIFI_REASON_DISASSOC_DUE_TO_INACTIVITY:
        case WIFI_REASON_AP_TSF_RESET:
        case WIFI_REASON_TIMEOUT:
            return was_connected ? HAL_WIFI_ERR_CONNECTION_LOST : HAL_WIFI_ERR_WEAK_SIGNAL;
        default:
            return was_connected ? HAL_WIFI_ERR_CONNECTION_LOST : HAL_WIFI_ERR_OTHER;
    }
}

/* Causas que não se resolvem tentando de novo: vale ativar logo a rede própria. */
static bool persistent(HalWifiError err) {
    return err == HAL_WIFI_ERR_WRONG_PASSWORD || err == HAL_WIFI_ERR_SSID_NOT_FOUND ||
           err == HAL_WIFI_ERR_SECURITY || err == HAL_WIFI_ERR_INVALID_PASSWORD ||
           err == HAL_WIFI_ERR_NOT_CONFIGURED;
}

static int max_attempts(HalWifiError err) {
    return persistent(err) ? WIFI_FATAL_RETRIES : WIFI_MAX_RETRIES;
}

/* Ativa a rede própria para que o app possa se conectar diretamente. */
static void start_ap_fallback(void) {
    if (s_ap_active) return;
    drv_wifi_enable_ap(s_device_name, WIFI_AP_PASS, WIFI_AP_CHANNEL);
    s_ap_active = true;
    ESP_LOGW(TAG, "Rede \"%s\" indisponível. Rede própria ativa: \"%s\" (senha: %s, IP 192.168.4.1)",
             s_cred.ssid, s_device_name, WIFI_AP_PASS);
    if (configured()) hal_timer_start_periodic(s_retry_timer, AP_RETRY_PERIOD_US);
}

static void retry_timer_cb(void *arg) {
    (void)arg;
    if (s_connected || s_stopping || s_scanning || !configured()) return;
    // Não troca de canal enquanto um aparelho usa a rede própria
    if (s_ap_active && s_ap_clients > 0) return;
    drv_wifi_connect();
}

/* Associado ao roteador, mas o DHCP não entregou IP: desfaz e tenta de novo. */
static void dhcp_timer_cb(void *arg) {
    (void)arg;
    if (s_connected || s_stopping) return;
    ESP_LOGW(TAG, "Associado a \"%s\", mas sem IP após %d s (DHCP)", s_cred.ssid,
             WIFI_DHCP_TIMEOUT_MS / 1000);
    s_dhcp_failed = true;
    drv_wifi_disconnect();                          // Gera STA_DISCONNECTED -> nova tentativa
}

static void handle_disconnect(uint16_t reason) {
    hal_timer_stop(s_dhcp_timer);
    bool was_connected = s_connected;
    s_connected = false;
    if (s_stopping) return;

    HalWifiError err;
    if (s_dhcp_failed) {
        s_dhcp_failed = false;
        err = HAL_WIFI_ERR_NO_IP;
    } else if (reason == WIFI_REASON_ASSOC_LEAVE || s_scanning) {
        notify();                                   // Desconexão pedida pelo próprio firmware
        return;                                     // (nova rede, varredura) – não conta
    } else {
        err = classify(reason, was_connected);
    }

    LOCK();
    s_error = err;
    s_reason = reason;
    if (was_connected) s_retries = 0;               // Queda: recomeça a contagem
    if (!s_ap_active) s_retries++;                  // No modo AP as tentativas são periódicas
    int attempt = s_retries;
    UNLOCK();

    ESP_LOGW(TAG, "Falha na conexão com \"%s\" (motivo %u, causa %d), tentativa %d de %d",
             s_cred.ssid, reason, err, attempt, max_attempts(err));

    if (s_ap_active) {                              // Modo AP: o timer periódico cuida
        notify();
        return;
    }
    if (attempt < max_attempts(err)) {
        int64_t delay_ms = attempt * 1000LL;        // Backoff: 1 s, 2 s, ... até 5 s
        if (delay_ms > 5000) delay_ms = 5000;
        hal_timer_start_once(s_retry_timer, (uint64_t)delay_ms * 1000);
    } else {
        start_ap_fallback();
    }
    notify();
}

static void on_event(DrvWifiEvent event, uint32_t arg) {
    switch (event) {
        case DRV_WIFI_EVENT_STA_CONNECTED:
            s_dhcp_failed = false;
            hal_timer_start_once(s_dhcp_timer, (uint64_t)WIFI_DHCP_TIMEOUT_MS * 1000);
            ESP_LOGI(TAG, "Associado a \"%s\", aguardando IP", s_cred.ssid);
            break;

        case DRV_WIFI_EVENT_STA_DISCONNECTED:
            handle_disconnect((uint16_t)arg);
            break;

        case DRV_WIFI_EVENT_STA_GOT_IP:
            hal_timer_stop(s_dhcp_timer);
            hal_timer_stop(s_retry_timer);
            LOCK();
            s_ip = arg;
            s_retries = 0;
            s_error = HAL_WIFI_ERR_NONE;
            s_reason = 0;
            UNLOCK();
            s_connected = true;
            // A rede própria só continua se alguém ainda a estiver usando
            if (s_ap_active && s_ap_clients == 0) {
                drv_wifi_disable_ap();
                s_ap_active = false;
            }
            ESP_LOGI(TAG, "Wi-Fi conectado, IP: %u.%u.%u.%u", (unsigned)(arg & 0xFF),
                     (unsigned)((arg >> 8) & 0xFF), (unsigned)((arg >> 16) & 0xFF),
                     (unsigned)((arg >> 24) & 0xFF));
            notify();
            break;

        case DRV_WIFI_EVENT_AP_CLIENT_JOINED:
            LOCK();
            s_ap_clients++;
            UNLOCK();
            ESP_LOGI(TAG, "Um dispositivo entrou na rede própria");
            break;

        case DRV_WIFI_EVENT_AP_CLIENT_LEFT:
            LOCK();
            if (s_ap_clients > 0) s_ap_clients--;
            UNLOCK();
            ESP_LOGI(TAG, "Um dispositivo saiu da rede própria");
            break;
    }
}

/* Varredura que guarda o resultado (usada no boot e pelo menu). */
static int scan_and_store(void) {
    int count = drv_wifi_scan(s_scan, MAX_SCAN_RESULTS);
    s_scan_count = count > 0 ? count : 0;
    update_scan_info();
    return count;
}

HalWifiError hal_wifi_check_password(const char *password) {
    size_t len = strlen(password);
    if (len == 0) return HAL_WIFI_ERR_NONE;                    // Rede aberta
    if (len < 8 || len > 64) return HAL_WIFI_ERR_INVALID_PASSWORD;
    if (len == 64) {                                            // Chave PSK em hexadecimal
        for (size_t i = 0; i < len; i++) {
            if (!isxdigit((unsigned char)password[i])) return HAL_WIFI_ERR_INVALID_PASSWORD;
        }
    }
    return HAL_WIFI_ERR_NONE;
}

/* Começa a conectar à rede de s_cred (ou explica por que não dá). */
static void begin_connect(void) {
    HalWifiError err = HAL_WIFI_ERR_NONE;
    if (!configured()) {
        err = HAL_WIFI_ERR_NOT_CONFIGURED;
    } else if (hal_wifi_check_password(s_cred.password) != HAL_WIFI_ERR_NONE) {
        err = HAL_WIFI_ERR_INVALID_PASSWORD;
    }
    LOCK();
    s_error = err;
    s_reason = 0;
    s_retries = 0;
    UNLOCK();
    if (err != HAL_WIFI_ERR_NONE) {
        ESP_LOGW(TAG, "Wi-Fi não iniciado (causa %d): use o menu serial, opção Rede Wi-Fi", err);
        start_ap_fallback();
        notify();
        return;
    }
    drv_wifi_set_station(s_cred.ssid, s_cred.password);
    notify();
    drv_wifi_connect();
}

bool hal_wifi_start(const WifiCredentials *cred, hal_wifi_state_cb on_state) {
    s_cred = *cred;
    s_on_state = on_state;
    if (!drv_wifi_init(on_event)) return false;

    s_retry_timer = hal_timer_create(retry_timer_cb, NULL, "wifi_retry");
    s_dhcp_timer = hal_timer_create(dhcp_timer_cb, NULL, "wifi_dhcp");
    if (!s_retry_timer || !s_dhcp_timer) return false;

    uint8_t mac[6];
    drv_wifi_mac(mac);
    snprintf(s_device_name, sizeof(s_device_name), "%s-%02X%02X", DEVICE_NAME_PREFIX, mac[4], mac[5]);

    if (!drv_wifi_start()) return false;
    notify();

    // RFE01: varre as redes antes de conectar (o resultado ajuda no diagnóstico)
    int found = scan_and_store();
    ESP_LOGI(TAG, "%d redes encontradas; \"%s\" %s", found, s_cred.ssid,
             s_seen ? "visível" : "não encontrada");

    begin_connect();
    return true;
}

void hal_wifi_reconnect(const WifiCredentials *cred) {
    hal_timer_stop(s_retry_timer);
    hal_timer_stop(s_dhcp_timer);
    LOCK();
    s_cred = *cred;
    s_dhcp_failed = false;
    UNLOCK();
    update_scan_info();
    drv_wifi_disconnect();                          // Evento ASSOC_LEAVE é ignorado
    if (s_ap_active) {
        drv_wifi_disable_ap();
        s_ap_active = false;
        LOCK();
        s_ap_clients = 0;
        UNLOCK();
    }
    begin_connect();
}

int hal_wifi_scan(DrvWifiAp *out, int max) {
    // Uma tentativa de conexão em andamento impede a varredura: pausa as tentativas
    s_scanning = true;
    hal_timer_stop(s_retry_timer);
    if (!s_connected) {
        hal_timer_stop(s_dhcp_timer);
        drv_wifi_disconnect();
        vTaskDelay(pdMS_TO_TICKS(150));
    }
    int count = scan_and_store();
    if (count < 0) {                                // Rádio ocupado: mais uma vez
        vTaskDelay(pdMS_TO_TICKS(500));
        count = scan_and_store();
    }
    s_scanning = false;

    // Retoma as tentativas
    if (!s_connected && configured() && !s_stopping) {
        if (s_ap_active) {
            hal_timer_start_periodic(s_retry_timer, AP_RETRY_PERIOD_US);
        } else {
            hal_timer_start_once(s_retry_timer, 500 * 1000);
        }
    }
    if (count > 0 && out) {
        if (count > max) count = max;
        memcpy(out, s_scan, (size_t)count * sizeof(DrvWifiAp));
    }
    notify();
    return count;
}

void hal_wifi_get_diag(HalWifiDiag *out) {
    memset(out, 0, sizeof(*out));
    out->state = hal_wifi_state();
    out->scanning = s_scanning;
    LOCK();
    out->error = s_error;
    out->reason = s_reason;
    out->max_attempts = (uint8_t)max_attempts(s_error);
    out->attempt = (uint8_t)(s_retries < out->max_attempts ? s_retries + 1 : out->max_attempts);
    memcpy(out->ssid, s_cred.ssid, sizeof(out->ssid));
    out->ssid_seen = s_seen;
    out->ssid_rssi = s_seen_rssi;
    out->ssid_channel = s_seen_channel;
    out->ssid_auth = s_seen_auth;
    memcpy(out->similar_ssid, s_similar, sizeof(out->similar_ssid));
    out->ap_clients = (uint8_t)s_ap_clients;
    UNLOCK();
    if (!configured()) out->ssid[0] = '\0';
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
    LOCK();
    uint32_t ip = s_ip;
    UNLOCK();
    if (s_connected) {
        snprintf(buf, len, "%u.%u.%u.%u", (unsigned)(ip & 0xFF), (unsigned)((ip >> 8) & 0xFF),
                 (unsigned)((ip >> 16) & 0xFF), (unsigned)((ip >> 24) & 0xFF));
    } else if (s_ap_active) {
        snprintf(buf, len, "192.168.4.1");
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
    if (s_dhcp_timer) hal_timer_stop(s_dhcp_timer);
    drv_wifi_deinit();
    s_connected = false;
    s_ap_active = false;
    notify();
}
