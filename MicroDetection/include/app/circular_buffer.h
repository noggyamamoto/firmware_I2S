/*
 * ============================================================================
 * Buffer circular sincronizado de quadros de áudio
 *
 * Usado no modo de diagnóstico, em que o áudio bruto é transmitido ao app
 * (fluxo I2S -> DMA -> buffer circular -> empacotamento -> UDP).
 * ============================================================================
 */
#ifndef CIRCULAR_BUFFER_H
#define CIRCULAR_BUFFER_H

#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "config.h"

#define CIRC_BUFFER_CAPACITY 4              // Capacidade máxima (nº de AudioFrames no buffer)

/* Quadro de áudio bruto (1024 amostras PCM de 16 bits). */
typedef struct {
    uint32_t timestamp_ms;                  // Início do quadro (relógio da sessão)
    uint32_t num_samples;                   // Quantidade de amostras válidas
    float    energy;                        // Energia média do quadro
    int16_t  samples[BLOCK_SAMPLES];        // Amostras filtradas
} AudioFrame;

/* Buffer circular com índices de leitura/escrita protegido por mutex. */
typedef struct {
    AudioFrame buffer[CIRC_BUFFER_CAPACITY]; // Array estático com até 4 frames
    int head;                               // Próxima posição de leitura (consumidor)
    int tail;                               // Próxima posição de escrita (produtor)
    int count;                              // Número atual de elementos
    uint32_t overruns;                      // Frames descartados por buffer cheio
    SemaphoreHandle_t mutex;                // Mutex para acesso concorrente seguro
} CircularBuffer;

void circ_buffer_init(CircularBuffer *cb);
bool circ_buffer_push(CircularBuffer *cb, const AudioFrame *frame);
bool circ_buffer_pop(CircularBuffer *cb, AudioFrame *out);
int  circ_buffer_count(CircularBuffer *cb);
void circ_buffer_clear(CircularBuffer *cb);

#endif // CIRCULAR_BUFFER_H
