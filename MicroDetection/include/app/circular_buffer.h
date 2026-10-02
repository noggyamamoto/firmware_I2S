/*
 * ============================================================================
 * Buffer circular de quadros de áudio (produtor único / consumidor único)
 *
 * Usado no modo de diagnóstico, em que o áudio bruto é transmitido ao app
 * (fluxo I2S -> DMA -> buffer circular -> empacotamento -> UDP).
 *
 * Sincronização: a tarefa de áudio (núcleo 1) só escreve no slot da cauda e
 * a tarefa de rede (núcleo 0) só lê o slot da cabeça. O único dado
 * compartilhado é o contador `count`, alterado dentro de um spinlock
 * (portMUX) por poucas instruções. Diferente do mutex usado no MVP:
 *  - o produtor de tempo real nunca bloqueia nem cede a CPU esperando a
 *    tarefa de rede (que pode estar preemptada pelo Wi-Fi no outro núcleo);
 *  - não há inversão de prioridade nem troca de contexto;
 *  - os quadros não são copiados: o produtor preenche o slot no lugar e a
 *    rede envia direto dele (o cabeçalho do pacote já fica no slot).
 * Buffer cheio: o quadro novo é descartado e contado em `overruns` (o slot
 * em transmissão nunca é sobrescrito).
 * ============================================================================
 */
#ifndef CIRCULAR_BUFFER_H
#define CIRCULAR_BUFFER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"

#include "config.h"
#include "protocol.h"

#define CIRC_BUFFER_CAPACITY 4              // Capacidade máxima (nº de AudioFrames no buffer)

/* Quadro de áudio bruto (1024 amostras PCM de 16 bits) já no formato do
 * pacote PKT_AUDIO_FRAME: cabeçalho seguido das amostras. */
typedef struct {
    uint32_t generation;                    // Sessão em que foi captado
    PktAudioFrameHeader packet;             // timestamp_ms, num_samples, energy
    int16_t  samples[BLOCK_SAMPLES];        // Amostras filtradas
} AudioFrame;

_Static_assert(offsetof(AudioFrame, samples) ==
                   offsetof(AudioFrame, packet) + sizeof(PktAudioFrameHeader),
               "cabeçalho e amostras precisam ser contíguos para o envio sem cópia");

typedef struct {
    AudioFrame buffer[CIRC_BUFFER_CAPACITY]; // Array estático com até 4 frames
    int head;                               // Próxima posição de leitura (só o consumidor altera)
    int tail;                               // Próxima posição de escrita (só o produtor altera)
    volatile int count;                     // Número atual de elementos
    volatile uint32_t overruns;             // Frames descartados por buffer cheio
    portMUX_TYPE lock;                      // Spinlock do contador
} CircularBuffer;

void circ_buffer_init(CircularBuffer *cb);

/* Produtor: slot livre para preencher (NULL se cheio) e publicação do slot. */
AudioFrame *circ_buffer_write_slot(CircularBuffer *cb);
void circ_buffer_commit(CircularBuffer *cb);

/* Consumidor: quadro mais antigo (NULL se vazio) e liberação do slot. */
AudioFrame *circ_buffer_read_slot(CircularBuffer *cb);
void circ_buffer_release(CircularBuffer *cb);

int  circ_buffer_count(CircularBuffer *cb);

#endif // CIRCULAR_BUFFER_H
