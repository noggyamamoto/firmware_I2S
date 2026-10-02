/*
 * ============================================================================
 * Implementação do estado compartilhado
 * ============================================================================
 */
#include "app_state.h"

#include <string.h>

#include "esp_log.h"

#include "config.h"
#include "hal_indicators.h"
#include "hal_time.h"
#include "hal_wifi.h"
#include "metronome.h"
#include "protocol.h"
#include "ui_events.h"

static const char *TAG = "state";

static portMUX_TYPE       s_lock = portMUX_INITIALIZER_UNLOCKED;   // Protege os campos abaixo
static QueueHandle_t      s_queue_ctrl;                 // Respostas, notas e batidas
static QueueHandle_t      s_queue_stream;               // Altura contínua e áudio bruto
static TaskHandle_t       s_consumer = NULL;            // Tarefa de transmissão
static CircularBuffer     s_audio_buffer;               // Quadros brutos (diagnóstico)

static bool               s_paired = false;
static NetPeer            s_peer;
static char               s_peer_name[PROTO_NAME_LEN + 1];
static int64_t            s_last_touch_us = 0;

static SessionInfo        s_session;
static uint8_t            s_flags = CFG_FLAG_METRO_SOUND | CFG_FLAG_METRO_VISUAL;
static float              s_noise_floor_db = -75.0f;
static uint32_t           s_dropped = 0;
static uint32_t           s_dropped_stream = 0;
static uint32_t           s_notes = 0;

// Spinlock: seções curtíssimas, sem chamadas bloqueantes (ver app_state.h)
#define LOCK()   portENTER_CRITICAL(&s_lock)
#define UNLOCK() portEXIT_CRITICAL(&s_lock)

void app_state_init(void) {
    s_queue_ctrl = xQueueCreate(EVENT_QUEUE_LENGTH, sizeof(OutEvent));
    s_queue_stream = xQueueCreate(STREAM_QUEUE_LENGTH, sizeof(OutEvent));
    circ_buffer_init(&s_audio_buffer);
    memset(&s_session, 0, sizeof(s_session));
}

// ======================= PAREAMENTO =========================================

bool app_state_is_paired(void) {
    LOCK();
    bool p = s_paired;
    UNLOCK();
    return p;
}

bool app_state_get_peer(NetPeer *out) {
    LOCK();
    bool p = s_paired;
    if (p) *out = s_peer;
    UNLOCK();
    return p;
}

void app_state_set_peer(const NetPeer *peer, const char *app_name) {
    LOCK();
    s_peer = *peer;
    s_paired = true;
    strncpy(s_peer_name, app_name, PROTO_NAME_LEN);
    s_peer_name[PROTO_NAME_LEN] = '\0';
    s_last_touch_us = hal_time_us();
    UNLOCK();
    char where[40];
    hal_net_peer_str(peer, where, sizeof(where));
    ESP_LOGI(TAG, "App pareado: %s (%s)", s_peer_name, where);
    ui_event(UI_EV_OK, "App \"%s\" conectado (%s)", s_peer_name, where);
    app_state_refresh_status_led();
}

void app_state_clear_peer(void) {
    app_state_session_stop();
    LOCK();
    bool was = s_paired;
    s_paired = false;
    s_peer_name[0] = '\0';
    UNLOCK();
    if (was) {
        ESP_LOGI(TAG, "App desconectado");
        ui_event(UI_EV_INFO, "App desconectado");
        app_state_refresh_status_led();
    }
}

const char *app_state_peer_name(void) {
    return s_peer_name;
}

void app_state_touch(void) {
    LOCK();
    s_last_touch_us = hal_time_us();
    UNLOCK();
}

int64_t app_state_ms_since_touch(void) {
    LOCK();
    int64_t t = s_last_touch_us;
    UNLOCK();
    return (hal_time_us() - t) / 1000;
}

// ======================= SESSÃO =============================================

bool app_state_session_start(uint16_t bpm, uint8_t beats_per_bar, uint8_t count_in_bars,
                             uint8_t flags, bool local, uint8_t session_id) {
    int64_t now = hal_time_us();
    LOCK();
    // Reenvio do mesmo SESSION_START (pacote perdido no caminho de volta):
    // reiniciar zeraria o relógio de novo e dessincronizaria o app
    bool duplicate = s_session.active && session_id != 0 && s_session.session_id == session_id;
    if (!duplicate) {
        s_session.active = true;
        s_session.generation++;
        s_session.t0_us = now;
        s_session.flags = flags;
        s_session.local = local;
        s_session.session_id = session_id;
        s_session.bpm = bpm;
        s_flags = flags;
    }
    UNLOCK();
    if (duplicate) {
        ESP_LOGI(TAG, "SESSION_START repetido (sessão %u): ignorado", session_id);
        return false;
    }
    metronome_start(bpm, beats_per_bar, count_in_bars, flags, now, session_id);
    ESP_LOGI(TAG, "Sessão iniciada: %u BPM, %u tempos, %u compassos de contagem",
             bpm, beats_per_bar, count_in_bars);
    ui_event(UI_EV_INFO, "%s iniciada: %u BPM, %u tempos por compasso",
             local ? "Captura local" : "Execução", bpm, beats_per_bar);
    return true;
}

void app_state_session_stop(void) {
    LOCK();
    bool was = s_session.active;
    s_session.active = false;
    s_session.generation++;
    UNLOCK();
    if (was) {
        metronome_stop();
        ESP_LOGI(TAG, "Sessão encerrada");
        ui_event(UI_EV_INFO, "Execução encerrada");
    }
}

void app_state_set_flags(uint8_t flags) {
    LOCK();
    s_flags = flags;
    s_session.flags = flags;
    UNLOCK();
    metronome_set_flags(flags);
}

uint8_t app_state_flags(void) {
    LOCK();
    uint8_t f = s_flags;
    UNLOCK();
    return f;
}

SessionInfo app_state_session(void) {
    LOCK();
    SessionInfo s = s_session;
    UNLOCK();
    return s;
}

uint32_t app_state_session_ms(void) {
    LOCK();
    int64_t t0 = s_session.t0_us;
    bool active = s_session.active;
    UNLOCK();
    if (!active) return (uint32_t)(hal_time_us() / 1000);
    return (uint32_t)((hal_time_us() - t0) / 1000);
}

uint8_t app_state_device_state(void) {
    LOCK();
    uint8_t st = s_session.active ? DEV_STATE_SESSION
                                  : (s_paired ? DEV_STATE_CONNECTED : DEV_STATE_IDLE);
    UNLOCK();
    return st;
}

// ======================= MÉTRICAS ===========================================

void app_state_set_noise_floor(float db) {
    s_noise_floor_db = db;                              // Escrita atômica de 32 bits
}

float app_state_noise_floor(void) {
    return s_noise_floor_db;
}

uint32_t app_state_dropped_events(void) {
    return s_dropped;
}

uint32_t app_state_dropped_stream(void) {
    return s_dropped_stream;
}

uint32_t app_state_notes_detected(void) {
    return s_notes;
}

// ======================= LED DE STATUS ======================================

void app_state_refresh_status_led(void) {
    bool paired = app_state_is_paired();
    switch (hal_wifi_state()) {
        case HAL_WIFI_CONNECTED:
            hal_status_led_set(paired ? STATUS_PAIRED : STATUS_WIFI_OK);
            break;
        case HAL_WIFI_AP_ONLY:
            hal_status_led_set(paired ? STATUS_PAIRED : STATUS_AP_MODE);
            break;
        case HAL_WIFI_CONNECTING:
            hal_status_led_set(STATUS_CONNECTING);
            break;
    }
}

// ======================= FILAS DE SAÍDA =====================================

static bool is_stream(OutEventType type) {
    return type == OUT_PITCH || type == OUT_AUDIO_READY;
}

bool app_state_post(const OutEvent *ev) {
    bool stream = is_stream(ev->type);
    // Produtores (áudio, metrônomo, rede) nunca bloqueiam: timeout 0
    bool ok = xQueueSend(stream ? s_queue_stream : s_queue_ctrl, ev, 0) == pdTRUE;
    LOCK();
    if (ev->type == OUT_NOTE && ev->u.note.type == NOTE_EVENT_ON) s_notes++;
    if (!ok) {
        if (stream) {
            s_dropped_stream++;                         // Fluxo de diagnóstico: aceitável
        } else {
            s_dropped++;                                // Evento importante perdido
        }
    }
    TaskHandle_t consumer = s_consumer;
    UNLOCK();
    if (ok && consumer) xTaskNotifyGive(consumer);
    return ok;
}

void app_state_set_consumer(TaskHandle_t task) {
    LOCK();
    s_consumer = task;
    UNLOCK();
}

bool app_state_next_event(OutEvent *out, TickType_t wait) {
    while (1) {
        // Prioridade: respostas, notas e batidas antes de altura/áudio bruto
        if (xQueueReceive(s_queue_ctrl, out, 0) == pdTRUE) return true;
        if (xQueueReceive(s_queue_stream, out, 0) == pdTRUE) return true;
        // Filas vazias: dorme até a próxima notificação de um produtor
        if (ulTaskNotifyTake(pdTRUE, wait) == 0) return false;
    }
}

CircularBuffer *app_state_audio_buffer(void) {
    return &s_audio_buffer;
}
