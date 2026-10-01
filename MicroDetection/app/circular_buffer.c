/*
 * ============================================================================
 * Implementação do buffer circular sincronizado
 * ============================================================================
 */
#include "circular_buffer.h"

#include <stdlib.h>

#include "esp_log.h"

/**
 * @brief Inicializa o buffer circular criando o mutex e zerando índices.
 */
void circ_buffer_init(CircularBuffer *cb) {
    cb->head = 0;                                           // Inicia leitura no índice 0
    cb->tail = 0;                                           // Inicia escrita no índice 0
    cb->count = 0;                                          // Nenhum elemento armazenado
    cb->overruns = 0;
    cb->mutex = xSemaphoreCreateMutex();                    // Cria o mutex do FreeRTOS
    if (cb->mutex == NULL) {                                // Verifica se a criação falhou
        ESP_LOGE("CircBuffer", "Falha ao criar mutex");     // Log de erro crítico
        abort();                                            // Aborta execução (crítico)
    }
}

/**
 * @brief Insere um frame no buffer. Se estiver cheio, descarta o mais antigo
 *        (o áudio mais recente é o mais útil para o feedback em tempo real).
 * @return true se não houve descarte.
 */
bool circ_buffer_push(CircularBuffer *cb, const AudioFrame *frame) {
    if (xSemaphoreTake(cb->mutex, portMAX_DELAY) != pdTRUE)
        return false;
    bool ok = true;
    if (cb->count >= CIRC_BUFFER_CAPACITY) {                // Buffer cheio: descarta o mais antigo
        cb->head = (cb->head + 1) % CIRC_BUFFER_CAPACITY;
        cb->count--;
        cb->overruns++;
        ok = false;
    }
    cb->buffer[cb->tail] = *frame;                          // Copia o frame para a posição de escrita
    cb->tail = (cb->tail + 1) % CIRC_BUFFER_CAPACITY;       // Avança o índice de escrita (circular)
    cb->count++;
    xSemaphoreGive(cb->mutex);
    return ok;
}

/**
 * @brief Remove e obtém um frame do buffer.
 * @return true se havia um frame disponível.
 */
bool circ_buffer_pop(CircularBuffer *cb, AudioFrame *out) {
    if (xSemaphoreTake(cb->mutex, portMAX_DELAY) != pdTRUE)
        return false;
    if (cb->count == 0) {                                   // Buffer vazio
        xSemaphoreGive(cb->mutex);
        return false;
    }
    *out = cb->buffer[cb->head];                            // Copia o frame da posição de leitura
    cb->head = (cb->head + 1) % CIRC_BUFFER_CAPACITY;       // Avança índice de leitura (circular)
    cb->count--;
    xSemaphoreGive(cb->mutex);
    return true;
}

/**
 * @brief Número de elementos atualmente no buffer (diagnóstico).
 */
int circ_buffer_count(CircularBuffer *cb) {
    return cb->count;
}

/**
 * @brief Esvazia o buffer (fim de sessão).
 */
void circ_buffer_clear(CircularBuffer *cb) {
    if (xSemaphoreTake(cb->mutex, portMAX_DELAY) != pdTRUE) return;
    cb->head = cb->tail = cb->count = 0;
    xSemaphoreGive(cb->mutex);
}
