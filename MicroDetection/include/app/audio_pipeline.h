/*
 * ============================================================================
 * app/audio_pipeline – tarefa de aquisição e processamento de áudio (núcleo 1)
 *
 * hal_audio (I2S -> DMA) -> passa-banda -> janela deslizante -> YIN -> segmentação
 *      -> fila de eventos (NOTE_ON / NOTE_OFF / PITCH)
 *      -> buffer circular de quadros brutos (modo diagnóstico)
 * ============================================================================
 */
#ifndef AUDIO_PIPELINE_H
#define AUDIO_PIPELINE_H

#include <stdbool.h>
#include <stdint.h>

#include "note_tracker.h"

/** Situação do microfone, verificada continuamente pela tarefa de áudio. */
typedef enum {
    MIC_STATUS_STARTING,                // Primeiro segundo de leitura
    MIC_STATUS_OK,                      // Recebendo sinal
    MIC_STATUS_NO_SIGNAL,               // Amostras constantes: microfone desligado ou mal ligado
    MIC_STATUS_READ_ERROR,              // O I2S não entrega amostras (timeout do DMA)
    MIC_STATUS_CLIPPING,                // Sinal saturado com frequência (muito perto/alto)
    MIC_STATUS_INIT_FAILED,             // O periférico I2S não pôde ser configurado
} MicStatus;

/** @brief Inicializa a entrada de áudio (hal_audio) e o DSP. */
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

/** @brief Situação atual do microfone (para o diagnóstico do menu). */
MicStatus audio_pipeline_mic_status(void);

/**
 * @brief Últimas notas detectadas (da mais nova para a mais antiga).
 * @param version recebe um contador que muda a cada nova nota (pode ser NULL).
 */
int audio_pipeline_recent_notes(NoteEvent *out, int max, uint32_t *version);

/** @brief Libera a entrada de áudio. */
void audio_pipeline_deinit(void);

#endif // AUDIO_PIPELINE_H
