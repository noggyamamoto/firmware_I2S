/*
 * ============================================================================
 * app/diagnostics – problemas atuais explicados ao usuário
 *
 * Reúne a situação do Wi-Fi (hal_wifi), do microfone (audio_pipeline) e do
 * enlace com o app (link_service / app_state) e descreve cada problema em
 * linguagem simples: o que aconteceu e o que o usuário deve fazer, citando
 * a opção do menu serial que resolve.
 * ============================================================================
 */
#ifndef DIAGNOSTICS_H
#define DIAGNOSTICS_H

#include <stdint.h>

#include "audio_pipeline.h"
#include "hal_wifi.h"
#include "ui_events.h"

#define DIAG_MAX_ACTIONS    3

typedef struct {
    UiEventLevel level;                     // UI_EV_WARN ou UI_EV_ERROR (UI_EV_INFO = orientação)
    char title[80];                         // O que aconteceu
    char actions[DIAG_MAX_ACTIONS][96];     // O que fazer
    int  n_actions;
} DiagProblem;

/** @brief Problemas atuais, do mais grave para o menos grave. */
int diagnostics_collect(DiagProblem *out, int max);

/** @brief Frase curta com a causa de uma falha de Wi-Fi (para os avisos). */
void diagnostics_wifi_error_text(const HalWifiDiag *d, char *buf, int len);

/** @brief Nome da segurança de uma rede (WPA2, WPA3...). */
const char *diagnostics_auth_name(DrvWifiAuth auth);

/** @brief Texto curto da situação do microfone. */
const char *diagnostics_mic_text(MicStatus status);

#endif // DIAGNOSTICS_H
