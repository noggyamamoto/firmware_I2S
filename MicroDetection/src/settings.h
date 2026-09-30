/*
 * ============================================================================
 * Configurações persistentes na NVS (credenciais do Wi-Fi)
 * ============================================================================
 */
#ifndef SETTINGS_H
#define SETTINGS_H

#include <stdbool.h>
#include <stddef.h>

#define SETTINGS_SSID_MAX   32
#define SETTINGS_PASS_MAX   64

typedef struct {
    char ssid[SETTINGS_SSID_MAX + 1];
    char password[SETTINGS_PASS_MAX + 1];
} WifiCredentials;

/** @brief Inicializa a NVS (apaga e recria se a partição estiver corrompida). */
void settings_init(void);

/** @brief Carrega as credenciais salvas ou os valores padrão do config.h. */
void settings_load_wifi(WifiCredentials *out);

/** @brief Salva novas credenciais. */
bool settings_save_wifi(const WifiCredentials *cred);

#endif // SETTINGS_H
