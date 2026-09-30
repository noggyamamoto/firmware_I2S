/*
 * ============================================================================
 * Indicadores físicos
 *
 *  - LED de status (RFE01 / RU01): informa o estado da conexão
 *      piscando rápido  -> conectando ao Wi-Fi
 *      2 piscadas curtas -> rede própria (modo AP) ativa
 *      piscando lento   -> Wi-Fi conectado, aguardando o app
 *      aceso            -> app pareado
 *  - LED RGB (RFE03): metrônomo visual
 *  - Buzzer passivo (RFE02): metrônomo sonoro
 * ============================================================================
 */
#ifndef INDICATORS_H
#define INDICATORS_H

#include <stdint.h>

typedef enum {
    STATUS_OFF,
    STATUS_CONNECTING,
    STATUS_AP_MODE,
    STATUS_WIFI_OK,
    STATUS_PAIRED,
    STATUS_ERROR,
} StatusPattern;

/** @brief Configura GPIO, LEDC e a tarefa de animação do LED de status. */
void indicators_init(void);

/** @brief Altera o padrão do LED de status. */
void indicators_set_status(StatusPattern pattern);
StatusPattern indicators_status(void);

/** @brief Acende o LED RGB (0..255 por canal) durante `ms` milissegundos. */
void indicators_flash_rgb(uint8_t r, uint8_t g, uint8_t b, uint32_t ms);

/** @brief Emite um bipe no buzzer passivo durante `ms` milissegundos. */
void indicators_beep(uint32_t freq_hz, uint32_t ms);

#endif // INDICATORS_H
