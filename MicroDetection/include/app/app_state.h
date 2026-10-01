/*
 * ============================================================================
 * Estado compartilhado entre as tarefas (sessão, pareamento e fila de saída)
 *
 *  - audio_task     : lê o estado da sessão e publica notas/altura
 *  - metronome      : publica batidas
 *  - net_rx_task    : altera o estado (CONNECT, SESSION_START, ...)
 *  - net_tx_task    : consome a fila de saída e envia os pacotes UDP
 *  - ui_task        : exibe diagnósticos e inicia sessões locais de teste
 *
 * Camada: app (usa hal_net, hal_wifi e hal_indicators)
 * ============================================================================
 */
#ifndef APP_STATE_H
#define APP_STATE_H

#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include "hal_net.h"

#include "circular_buffer.h"
#include "note_tracker.h"

/* Tipos de itens da fila de saída. */
typedef enum {
    OUT_ANNOUNCE,           // Resposta ao DISCOVER (para o remetente)
    OUT_CONNECT_ACK,        // Resposta ao CONNECT
    OUT_PONG,               // Resposta ao PING
    OUT_NOTE,               // NOTE_ON / NOTE_OFF
    OUT_PITCH,              // Altura contínua
    OUT_BEAT,               // Batida do metrônomo
    OUT_AUDIO_READY,        // Há quadro bruto no buffer circular
} OutEventType;

typedef struct {
    OutEventType type;
    bool has_dest;                          // true = enviar para `dest` em vez do app pareado
    NetPeer dest;                           // Endereço (UDP ou WebSocket) do destinatário
    uint32_t timestamp_ms;
    union {
        NoteEvent note;
        struct { float frequency_hz, confidence, level_db; } pitch;
        struct { uint8_t beat_in_bar, count_in; uint16_t bar_index, bpm; } beat;
        uint8_t accepted;
    } u;
} OutEvent;

/* Snapshot da sessão lido pela tarefa de áudio. */
typedef struct {
    bool     active;                        // Sessão em andamento
    uint32_t generation;                    // Incrementa a cada início/fim (reinicia o rastreador)
    int64_t  t0_us;                         // Instante zero da sessão (relógio hal_time)
    uint8_t  flags;                         // CFG_FLAG_*
    bool     local;                         // Sessão de teste iniciada pelo menu serial
} SessionInfo;

void app_state_init(void);

/* ---- Pareamento com o app ---- */
bool app_state_is_paired(void);
bool app_state_get_peer(NetPeer *out);
void app_state_set_peer(const NetPeer *peer, const char *app_name);
void app_state_clear_peer(void);
const char *app_state_peer_name(void);
void app_state_touch(void);                             // Registra PING recebido
int64_t app_state_ms_since_touch(void);

/* ---- Sessão ---- */
void app_state_session_start(uint16_t bpm, uint8_t beats_per_bar, uint8_t count_in_bars,
                             uint8_t flags, bool local);
void app_state_session_stop(void);
void app_state_set_flags(uint8_t flags);
uint8_t app_state_flags(void);
SessionInfo app_state_session(void);
uint32_t app_state_session_ms(void);                    // ms desde o início da sessão
uint8_t app_state_device_state(void);                   // DEV_STATE_*

/* ---- Métricas ---- */
void app_state_set_noise_floor(float db);
float app_state_noise_floor(void);
uint32_t app_state_dropped_events(void);
uint32_t app_state_notes_detected(void);

/* ---- LED de status (RFE01): Wi-Fi + pareamento ---- */
void app_state_refresh_status_led(void);

/* ---- Fila de saída ---- */
bool app_state_post(const OutEvent *ev);                // Não bloqueante
QueueHandle_t app_state_queue(void);

/* Buffer de áudio bruto (modo diagnóstico). */
CircularBuffer *app_state_audio_buffer(void);

#endif // APP_STATE_H
