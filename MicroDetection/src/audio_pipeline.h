/*
 * ============================================================================
 * Tarefa de aquisição e processamento de áudio (núcleo 1)
 *
 * I2S -> DMA -> passa-banda -> janela deslizante -> YIN -> segmentação
 *      -> fila de eventos (NOTE_ON / NOTE_OFF / PITCH)
 *      -> buffer circular de quadros brutos (modo diagnóstico)
 * ============================================================================
 */
#ifndef AUDIO_PIPELINE_H
#define AUDIO_PIPELINE_H

#include <stdbool.h>
#include <stdint.h>

/** @brief Inicializa o I2S e o DSP. */
bool audio_pipeline_init(void);

/** @brief Cria a tarefa produtora fixada no núcleo 1. */
void audio_pipeline_start_task(void);

/** @brief Ignora a altura do clique do metrônomo até `until_ms` (relógio da sessão). */
void audio_pipeline_mask_click(uint32_t until_ms, float click_hz);

/** @brief Carga média de processamento por janela (µs), para diagnóstico. */
uint32_t audio_pipeline_process_time_us(void);

/** @brief Último nível RMS medido (dBFS) e última altura estimada (Hz). */
float audio_pipeline_level_db(void);
float audio_pipeline_last_pitch_hz(void);

/** @brief Libera os recursos (I2S). */
void audio_pipeline_deinit(void);

#endif // AUDIO_PIPELINE_H
