/*
 * ============================================================================
 * hal/hal_storage – configurações persistentes (credenciais do Wi-Fi)
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

/** @brief Carrega as credenciais salvas ou os valores padrão do config.h. */
void hal_storage_load_wifi(WifiCredentials *out);

/** @brief Salva novas credenciais. */
bool hal_storage_save_wifi(const WifiCredentials *cred);

#endif // HAL_STORAGE_H
