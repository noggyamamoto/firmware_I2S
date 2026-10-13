/*
 * ============================================================================
 * hal/hal_storage – memória não volátil (NVS) e rede Wi-Fi padrão
 *
 * A NVS precisa estar iniciada para o rádio Wi-Fi funcionar. A rede usada
 * é informada a cada inicialização (app/wifi_setup); a rede padrão do
 * config.h é a reserva quando a informada não conecta.
 * ============================================================================
 */
#ifndef HAL_STORAGE_H
#define HAL_STORAGE_H

#include <stdbool.h>

#define HAL_SSID_MAX    32
#define HAL_PASS_MAX    64

typedef struct {
    char ssid[HAL_SSID_MAX + 1];
    char password[HAL_PASS_MAX + 1];
} WifiCredentials;

/** @brief Inicializa o armazenamento não volátil. */
bool hal_storage_init(void);

/** @brief Rede padrão (WIFI_DEFAULT_SSID / WIFI_DEFAULT_PASS do config.h). */
void hal_storage_default_wifi(WifiCredentials *out);

#endif // HAL_STORAGE_H
