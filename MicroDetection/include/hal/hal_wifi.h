/*
 * ============================================================================
 * hal/hal_wifi – conexão Wi-Fi (RFE01 / RU01)
 *
 *  1. Varre as redes disponíveis e lista no log
 *  2. Conecta à rede configurada, com novas tentativas
 *  3. Se não conseguir, ativa uma rede própria (modo AP) "PartituraIoT-XXXX",
 *     permitindo que o celular/computador se conecte diretamente ao
 *     dispositivo, e continua tentando a rede configurada em segundo plano
 *  4. A cada mudança de estado avisa a aplicação (callback), que atualiza o
 *     LED de status
 * ============================================================================
 */
#ifndef HAL_WIFI_H
#define HAL_WIFI_H

#include <stdbool.h>
#include <stdint.h>

#include "hal_storage.h"

typedef enum {
    HAL_WIFI_CONNECTING,                // Tentando a rede configurada
    HAL_WIFI_CONNECTED,                 // Conectado à rede configurada
    HAL_WIFI_AP_ONLY,                   // Somente a rede própria está ativa
} HalWifiState;

typedef void (*hal_wifi_state_cb)(HalWifiState state);
typedef void (*hal_print_fn)(const char *fmt, ...);

/** @brief Inicializa o rádio e inicia a conexão. */
bool hal_wifi_start(const WifiCredentials *cred, hal_wifi_state_cb on_state);

/** @brief Troca as credenciais e reconecta. */
void hal_wifi_reconnect(const WifiCredentials *cred);

/** @brief Varre e imprime as redes disponíveis. @return quantidade de redes. */
int hal_wifi_scan(hal_print_fn print);

HalWifiState hal_wifi_state(void);
bool hal_wifi_is_connected(void);
bool hal_wifi_ap_active(void);
int8_t hal_wifi_rssi(void);
void hal_wifi_ip_str(char *buf, int len);
void hal_wifi_mac(uint8_t mac[6]);
const char *hal_wifi_device_name(void);

/** @brief Desliga o Wi-Fi e libera os recursos. */
void hal_wifi_stop(void);

#endif // HAL_WIFI_H
