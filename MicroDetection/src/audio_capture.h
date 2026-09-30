/*
 * ============================================================================
 * Captura de áudio do microfone digital INMP441 via I2S
 *
 * O INMP441 entrega amostras de 24 bits (complemento de 2, MSB primeiro)
 * alinhadas à esquerda em slots de 32 bits, no formato I2S Philips. Com o
 * pino L/R ligado ao GND, o microfone transmite no slot esquerdo.
 * ============================================================================
 */
#ifndef AUDIO_CAPTURE_H
#define AUDIO_CAPTURE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/i2s_std.h"

typedef struct {
    i2s_chan_handle_t rx_handle;    // Canal RX do I2S
    int32_t *raw;                   // Buffer das palavras de 32 bits lidas via DMA
    size_t raw_capacity;            // Capacidade do buffer (amostras)
    bool enabled;                   // Canal habilitado
    uint64_t samples_read;          // Total de amostras lidas desde o start
    int64_t start_us;               // Instante do start (esp_timer)
} AudioCapture;

/** @brief Inicializa o I2S em modo mestre RX (Philips, 32 bits, mono esquerdo). */
bool audio_capture_init(AudioCapture *cap, size_t max_samples_per_read);

/** @brief Habilita o DMA e começa a captar. */
bool audio_capture_start(AudioCapture *cap);

/** @brief Desabilita o canal. */
void audio_capture_stop(AudioCapture *cap);

/**
 * @brief Lê exatamente `n` amostras e converte para float em [-1, 1),
 *        aplicando o ganho de entrada.
 * @return Quantidade de amostras lidas (0 em caso de erro/timeout).
 */
size_t audio_capture_read(AudioCapture *cap, float *out, size_t n, uint32_t timeout_ms);

/** @brief Instante (esp_timer, µs) da primeira amostra do próximo bloco. */
int64_t audio_capture_next_sample_time_us(const AudioCapture *cap);

/** @brief Libera os recursos do I2S. */
void audio_capture_deinit(AudioCapture *cap);

#endif // AUDIO_CAPTURE_H
