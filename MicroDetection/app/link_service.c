/*
 * ============================================================================
 * app/link_service – implementação (protocolo sobre hal_net)
 * ============================================================================
 */
#include "link_service.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app_state.h"
#include "config.h"
#include "hal_net.h"
#include "hal_time.h"
#include "hal_wifi.h"
#include "metronome.h"
#include "protocol.h"
#include "ui_events.h"

static const char *TAG = "link";

static uint32_t s_sequence = 0;                 // Usado apenas pela tarefa de transmissão
static HalTimer s_watchdog = NULL;              // Verifica o PING do app a cada 1 s
static volatile uint32_t s_timeouts = 0;        // Vezes em que o app parou de responder

static void fill_header(PacketHeader *h, uint8_t type, uint32_t timestamp_ms) {
    h->magic = PROTO_MAGIC;
    h->version = PROTO_VERSION;
    h->type = type;
    h->sequence = s_sequence++;
    h->timestamp_ms = timestamp_ms;
}

static bool send_to(const NetPeer *dest, const void *data, size_t len) {
    if (!hal_net_send(dest, data, len)) {
        ESP_LOGD(TAG, "Falha no envio");
        return false;
    }
    return true;
}

// ======================= TRANSMISSÃO ========================================

static void tx_task(void *arg) {
    (void)arg;
    OutEvent ev;
    app_state_set_consumer(xTaskGetCurrentTaskHandle());   // Antes de esvaziar as filas
    while (1) {
        // Bloqueio só aqui (consumidor), sem prazo: acorda a cada evento publicado
        if (!app_state_next_event(&ev, portMAX_DELAY)) continue;

        NetPeer dest;
        if (ev.has_dest) {
            dest = ev.dest;
        } else if (!app_state_get_peer(&dest)) {
            continue;                                   // Sem app pareado: descarta
        }

        switch (ev.type) {
            case OUT_ANNOUNCE: {
                PktAnnounce p = {0};
                fill_header(&p.h, PKT_ANNOUNCE, ev.timestamp_ms);
                strncpy(p.name, hal_wifi_device_name(), PROTO_NAME_LEN);
                strncpy(p.firmware, FIRMWARE_VERSION, PROTO_FW_LEN);
                hal_wifi_mac(p.mac);
                p.state = app_state_device_state();
                p.rssi = hal_wifi_rssi();
                p.sample_rate = SAMPLE_RATE;
                send_to(&dest, &p, sizeof(p));
                break;
            }
            case OUT_CONNECT_ACK: {
                PktConnectAck p = {0};
                fill_header(&p.h, PKT_CONNECT_ACK, ev.timestamp_ms);
                p.accepted = ev.u.accepted;
                send_to(&dest, &p, sizeof(p));
                break;
            }
            case OUT_PONG: {
                PktPong p = {0};
                fill_header(&p.h, PKT_PONG, ev.timestamp_ms);
                p.state = app_state_device_state();
                p.rssi = hal_wifi_rssi();
                p.noise_floor_db = app_state_noise_floor();
                p.dropped_events = app_state_dropped_events();
                send_to(&dest, &p, sizeof(p));
                break;
            }
            case OUT_NOTE: {
                if (ev.u.note.type == NOTE_EVENT_ON) {
                    PktNoteOn p = {0};
                    fill_header(&p.h, PKT_NOTE_ON, ev.u.note.time_ms);
                    p.midi = ev.u.note.midi;
                    p.frequency_hz = ev.u.note.frequency_hz;
                    p.level_db = ev.u.note.level_db;
                    p.confidence = ev.u.note.confidence;
                    send_to(&dest, &p, sizeof(p));
                } else {
                    PktNoteOff p = {0};
                    fill_header(&p.h, PKT_NOTE_OFF, ev.u.note.time_ms);
                    p.midi = ev.u.note.midi;
                    p.duration_ms = ev.u.note.duration_ms;
                    p.frequency_hz = ev.u.note.frequency_hz;
                    send_to(&dest, &p, sizeof(p));
                }
                break;
            }
            case OUT_PITCH: {
                PktPitch p = {0};
                fill_header(&p.h, PKT_PITCH, ev.timestamp_ms);
                p.frequency_hz = ev.u.pitch.frequency_hz;
                p.confidence = ev.u.pitch.confidence;
                p.level_db = ev.u.pitch.level_db;
                send_to(&dest, &p, sizeof(p));
                break;
            }
            case OUT_BEAT: {
                PktBeat p = {0};
                fill_header(&p.h, PKT_BEAT, ev.timestamp_ms);
                p.beat_in_bar = ev.u.beat.beat_in_bar;
                p.count_in = ev.u.beat.count_in;
                p.bar_index = ev.u.beat.bar_index;
                p.bpm = ev.u.beat.bpm;
                p.session_id = ev.u.beat.session_id;
                send_to(&dest, &p, sizeof(p));
                break;
            }
            case OUT_AUDIO_READY: {
                // O slot já tem o formato do pacote (cabeçalho + PCM 16 bits):
                // completa o cabeçalho e envia direto do buffer circular
                CircularBuffer *cb = app_state_audio_buffer();
                uint32_t generation = app_state_session().generation;
                AudioFrame *frame;
                while ((frame = circ_buffer_read_slot(cb)) != NULL) {
                    if (frame->generation == generation) {      // Descarta sessão anterior
                        PktAudioFrameHeader *h = &frame->packet;
                        fill_header(&h->h, PKT_AUDIO_FRAME, h->h.timestamp_ms);
                        send_to(&dest, h, sizeof(*h) + h->num_samples * sizeof(int16_t));
                    }
                    circ_buffer_release(cb);
                }
                break;
            }
        }
    }
}

// ======================= RECEPÇÃO ===========================================

static void post_reply(OutEventType type, const NetPeer *to, uint8_t accepted) {
    OutEvent ev = {.type = type, .has_dest = true, .dest = *to,
                   .timestamp_ms = app_state_session_ms()};
    ev.u.accepted = accepted;
    app_state_post(&ev);
}

/* Trata um comando recebido do app (UDP ou WebSocket). */
static void handle_packet(const NetPeer *from, const uint8_t *buf, size_t size) {
    int len = (int)size;
    if (len < (int)sizeof(PacketHeader)) return;
    const PacketHeader *h = (const PacketHeader *)buf;
    if (h->magic != PROTO_MAGIC || h->version != PROTO_VERSION) return;   // Não é do app

    NetPeer peer;
    bool paired = app_state_get_peer(&peer);
    bool from_peer = paired && hal_net_peer_equal(&peer, from);

    switch (h->type) {
        case PKT_DISCOVER:
            post_reply(OUT_ANNOUNCE, from, 0);
            break;

        case PKT_CONNECT: {
            if (len < (int)sizeof(PktConnect)) return;
            const PktConnect *p = (const PktConnect *)buf;
            // Um app pareado recentemente mantém o dispositivo; outro app só assume
            // se o anterior parou de enviar PING (RU02)
            bool busy = paired && !from_peer && app_state_ms_since_touch() < SESSION_TIMEOUT_MS;
            if (!busy) {
                char name[PROTO_NAME_LEN + 1];
                memcpy(name, p->app_name, PROTO_NAME_LEN);
                name[PROTO_NAME_LEN] = '\0';
                if (paired && !from_peer) app_state_clear_peer();
                app_state_set_peer(from, name);
            }
            post_reply(OUT_CONNECT_ACK, from, busy ? 0 : 1);
            break;
        }

        case PKT_DISCONNECT:
            if (from_peer) app_state_clear_peer();      // Desconexão segura (RU17)
            break;

        case PKT_PING:
            if (from_peer) {
                app_state_touch();
                post_reply(OUT_PONG, from, 0);
            }
            break;

        case PKT_CONFIG: {
            if (!from_peer || len < (int)sizeof(PktConfig)) return;
            const PktConfig *p = (const PktConfig *)buf;
            app_state_set_flags(p->flags);
            ESP_LOGI(TAG, "CONFIG: %u BPM, %u tempos, flags=0x%02x", p->bpm, p->beats_per_bar,
                     p->flags);
            break;
        }

        case PKT_SESSION_START: {
            if (!from_peer || len < (int)sizeof(PktSessionStart)) return;
            const PktSessionStart *p = (const PktSessionStart *)buf;
            app_state_session_start(p->bpm, p->beats_per_bar, p->count_in_bars, p->flags, false,
                                    p->session_id);
            break;
        }

        case PKT_SESSION_STOP:
            if (from_peer) app_state_session_stop();
            break;

        case PKT_SET_TEMPO: {
            if (!from_peer || len < (int)sizeof(PktSetTempo)) return;
            const PktSetTempo *p = (const PktSetTempo *)buf;
            metronome_set_tempo(p->bpm, p->at_beat);
            break;
        }

        default:
            break;
    }
}

/* App parou de responder: encerra a sessão e libera o dispositivo. */
static void watchdog_cb(void *arg) {
    (void)arg;
    if (app_state_is_paired() && app_state_ms_since_touch() > SESSION_TIMEOUT_MS) {
        ESP_LOGW(TAG, "App sem resposta há %d ms, desconectando", SESSION_TIMEOUT_MS);
        s_timeouts++;
        ui_event(UI_EV_WARN, "App parou de responder (%d s sem sinal): pareamento desfeito",
                 SESSION_TIMEOUT_MS / 1000);
        app_state_clear_peer();
    }
}

// ======================= CICLO DE VIDA ======================================

bool link_service_init(void) {
    s_watchdog = hal_timer_create(watchdog_cb, NULL, "link_wd");
    return s_watchdog != NULL && hal_net_init(handle_packet);
}

void link_service_start(void) {
    // Consumidora de rede: núcleo 0, prioridade abaixo da captura de áudio
    xTaskCreatePinnedToCore(tx_task, "net_tx", 4096, NULL, 4, NULL, 0);
    hal_net_start();
    hal_timer_start_periodic(s_watchdog, 1000 * 1000);
}

uint32_t link_service_packets_sent(void) { return hal_net_packets_sent(); }
uint32_t link_service_send_errors(void) { return hal_net_send_errors(); }
uint32_t link_service_timeouts(void) { return s_timeouts; }

void link_service_deinit(void) {
    if (s_watchdog) hal_timer_stop(s_watchdog);
    hal_net_deinit();
}
