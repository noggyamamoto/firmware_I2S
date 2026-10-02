/*
 * ============================================================================
 * Implementação do buffer circular (SPSC, sem cópia, spinlock no contador)
 * ============================================================================
 */
#include "circular_buffer.h"

#include <string.h>

void circ_buffer_init(CircularBuffer *cb) {
    cb->head = 0;                                           // Inicia leitura no índice 0
    cb->tail = 0;                                           // Inicia escrita no índice 0
    cb->count = 0;                                          // Nenhum elemento armazenado
    cb->overruns = 0;
    portMUX_INITIALIZE(&cb->lock);
}

/**
 * @brief Slot da cauda para o produtor preencher. Se o buffer estiver cheio,
 *        conta um overrun e retorna NULL (o quadro deve ser descartado).
 */
AudioFrame *circ_buffer_write_slot(CircularBuffer *cb) {
    portENTER_CRITICAL(&cb->lock);
    bool full = cb->count >= CIRC_BUFFER_CAPACITY;
    if (full) cb->overruns++;
    portEXIT_CRITICAL(&cb->lock);
    return full ? NULL : &cb->buffer[cb->tail];
}

/** @brief Publica o slot preenchido para o consumidor. */
void circ_buffer_commit(CircularBuffer *cb) {
    cb->tail = (cb->tail + 1) % CIRC_BUFFER_CAPACITY;       // Avança o índice de escrita (circular)
    portENTER_CRITICAL(&cb->lock);
    cb->count++;
    portEXIT_CRITICAL(&cb->lock);
}

/** @brief Quadro mais antigo, ainda no buffer (NULL se vazio). */
AudioFrame *circ_buffer_read_slot(CircularBuffer *cb) {
    portENTER_CRITICAL(&cb->lock);
    bool empty = cb->count == 0;
    portEXIT_CRITICAL(&cb->lock);
    return empty ? NULL : &cb->buffer[cb->head];
}

/** @brief Devolve o slot lido ao produtor. */
void circ_buffer_release(CircularBuffer *cb) {
    cb->head = (cb->head + 1) % CIRC_BUFFER_CAPACITY;       // Avança índice de leitura (circular)
    portENTER_CRITICAL(&cb->lock);
    cb->count--;
    portEXIT_CRITICAL(&cb->lock);
}

/** @brief Número de elementos atualmente no buffer (diagnóstico). */
int circ_buffer_count(CircularBuffer *cb) {
    return cb->count;
}
