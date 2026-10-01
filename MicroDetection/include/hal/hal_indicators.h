/*
 * ============================================================================
 * hal/hal_indicators – indicadores físicos
 *
 *  - LED de status (RFE01 / RU01) – LED embutido da placa:
 *      piscando rápido   -> conectando ao Wi-Fi
 *      2 piscadas curtas -> rede própria (modo AP) ativa
 *      piscando lento    -> Wi-Fi conectado, aguardando o app
 *      aceso             -> app pareado
 *  - LED RGB (RFE03): metrônomo visual
 *  - Buzzer passivo (RFE02): metrônomo sonoro
 * ============================================================================
 */
#ifndef HAL_INDICATORS_H
#define HAL_INDICATORS_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    STATUS_OFF,
    STATUS_CONNECTING,
    STATUS_AP_MODE,
    STATUS_WIFI_OK,
    STATUS_PAIRED,
    STATUS_ERROR,
} StatusPattern;

/** @brief Configura GPIO, PWM e a tarefa de animação do LED de status. */
bool hal_indicators_init(void);

/** @brief Altera o padrão do LED de status. */
void hal_status_led_set(StatusPattern pattern);
StatusPattern hal_status_led_get(void);

/** @brief Acende o LED RGB (0..255 por canal) durante `ms` milissegundos. */
void hal_rgb_flash(uint8_t r, uint8_t g, uint8_t b, uint32_t ms);

/** @brief Emite um bipe no buzzer passivo durante `ms` milissegundos. */
void hal_buzzer_beep(uint32_t freq_hz, uint32_t ms);

#endif // HAL_INDICATORS_H
