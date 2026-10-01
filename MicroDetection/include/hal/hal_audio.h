/*
 * ============================================================================
 * hal/hal_audio – entrada de áudio
 *
 * Expõe o microfone como amostras float em [-1, 1) já com o ganho de
 * entrada aplicado e com o instante (relógio do sistema) de cada bloco, sem
 * expor detalhes do I2S/INMP441.
 * ============================================================================
 */
#ifndef HAL_AUDIO_H
#define HAL_AUDIO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** @brief Inicializa o microfone (pinos e taxa do config.h). */
bool hal_audio_init(size_t max_samples_per_read);

/** @brief Começa a captar. */
bool hal_audio_start(void);

/** @brief Para a captação. */
void hal_audio_stop(void);

/**
 * @brief Lê exatamente `n` amostras normalizadas.
 * @return quantidade lida (0 em caso de erro/timeout).
 */
size_t hal_audio_read(float *out, size_t n, uint32_t timeout_ms);

/** @brief Instante (µs, relógio do sistema) da primeira amostra do próximo bloco. */
int64_t hal_audio_next_sample_time_us(void);

/** @brief Libera o microfone. */
void hal_audio_deinit(void);

#endif // HAL_AUDIO_H
