/*
 * ============================================================================
 * Gerenciamento da conexão Wi-Fi (RFE01 / RU01)
 *
 *  1. Varre as redes disponíveis e lista no terminal serial
 *  2. Conecta à rede configurada (credenciais da NVS), com novas tentativas
 *  3. Se não conseguir, ativa uma rede própria (modo AP) "PartituraIoT-XXXX",
 *     permitindo que o celular/computador se conecte diretamente ao dispositivo,
 *     e continua tentando a rede configurada em segundo plano
 *  4. O LED de status indica cada estado
 * ============================================================================
 */
#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include <stdbool.h>
#include <stdint.h>

#include "settings.h"

typedef void (*wifi_print_fn)(const char *fmt, ...);

/** @brief Inicializa netif, eventos e driver Wi-Fi e inicia a conexão. */
bool wifi_manager_start(const WifiCredentials *cred);

/** @brief Troca as credenciais e reconecta. */
void wifi_manager_reconnect(const WifiCredentials *cred);

/** @brief Varre e imprime as redes disponíveis. @return quantidade de redes. */
int wifi_manager_scan(wifi_print_fn print);

bool wifi_manager_is_connected(void);
bool wifi_manager_ap_active(void);
int8_t wifi_manager_rssi(void);
void wifi_manager_ip_str(char *buf, int len);
void wifi_manager_mac(uint8_t mac[6]);
const char *wifi_manager_device_name(void);

/** @brief Atualiza o LED de status conforme Wi-Fi e pareamento. */
void wifi_manager_refresh_status(void);

/** @brief Desliga o Wi-Fi e libera os recursos. */
void wifi_manager_stop(void);

#endif // WIFI_MANAGER_H
