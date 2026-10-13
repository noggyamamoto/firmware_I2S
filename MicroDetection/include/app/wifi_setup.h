/*
 * ============================================================================
 * app/wifi_setup – escolha da rede Wi-Fi na inicializacao
 *
 * Logo ao ligar, o terminal serial pede o nome (SSID) e a senha da rede a
 * que o dispositivo deve se conectar. ENTER sem digitar, ESC ou nenhuma
 * tecla durante WIFI_SETUP_TIMEOUT_MS usam a rede padrao do config.h. A rede
 * padrao tambem e a reserva: se a rede digitada nao conectar, o dispositivo
 * passa para ela (hal_wifi).
 *
 * Camada: app (usa hal_console e hal_wifi)
 * ============================================================================
 */
#ifndef WIFI_SETUP_H
#define WIFI_SETUP_H

#include <stdbool.h>

#include "hal_storage.h"

/**
 * @brief Pergunta a rede no terminal (bloqueia ate a resposta ou o tempo limite).
 * @param out recebe a rede digitada (vazia quando o usuario escolhe a padrao).
 * @return true se o usuario digitou uma rede.
 */
bool wifi_setup_ask(WifiCredentials *out);

#endif // WIFI_SETUP_H
