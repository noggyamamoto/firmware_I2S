/*
 * ============================================================================
 * Implementação do enlace UDP
 * ============================================================================
 */
#include "udp_link.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

#include "app_state.h"
#include "config.h"
#include "metronome.h"
#include "protocol.h"
#include "wifi_manager.h"

static const char *TAG = "udp";

static int s_sock = -1;                         // Socket UDP (recepção e envio)
static uint32_t s_sequence = 0;                 // Usado apenas pela tarefa de transmissão
static uint32_t s_sent = 0;
static uint32_t s_errors = 0;

// Buffer do maior pacote: quadro de áudio bruto
static uint8_t s_tx_buf[sizeof(PktAudioFrameHeader) + BLOCK_SAMPLES * sizeof(int16_t)];

static void fill_header(PacketHeader *h, uint8_t type, uint32_t timestamp_ms) {
    h->magic = PROTO_MAGIC;
    h->version = PROTO_VERSION;
    h->type = type;
    h->sequence = s_sequence++;
    h->timestamp_ms = timestamp_ms;
}

static bool send_to(const struct sockaddr_in *dest, const void *data, size_t len) {
    if (s_sock < 0) return false;
    int sent = sendto(s_sock, data, len, 0, (const struct sockaddr *)dest, sizeof(*dest));
    if (sent != (int)len) {
        s_errors++;
        ESP_LOGE(TAG, "Falha no envio UDP (%d)", errno);
        return false;
    }
    s_sent++;
    return true;
}

// ======================= TRANSMISSÃO ========================================

static void tx_task(void *arg) {
    (void)arg;
    OutEvent ev;
    AudioFrame frame;
    while (1) {
        if (xQueueReceive(app_state_queue(), &ev, portMAX_DELAY) != pdTRUE) continue;

        struct sockaddr_in dest;
        if (ev.has_dest) {
            dest = ev.dest;
        } else if (!app_state_get_peer(&dest)) {
            continue;                                   // Sem app pareado: descarta
        }

        switch (ev.type) {
            case OUT_ANNOUNCE: {
                PktAnnounce p = {0};
                fill_header(&p.h, PKT_ANNOUNCE, ev.timestamp_ms);
                strncpy(p.name, wifi_manager_device_name(), PROTO_NAME_LEN);
                strncpy(p.firmware, FIRMWARE_VERSION, PROTO_FW_LEN);
                wifi_manager_mac(p.mac);
                p.state = app_state_device_state();
                p.rssi = wifi_manager_rssi();
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
                p.rssi = wifi_manager_rssi();
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
                send_to(&dest, &p, sizeof(p));
                break;
            }
            case OUT_AUDIO_READY: {
                // Empacota o quadro bruto: cabeçalho + amostras PCM de 16 bits
                while (circ_buffer_pop(app_state_audio_buffer(), &frame)) {
                    PktAudioFrameHeader *h = (PktAudioFrameHeader *)s_tx_buf;
                    fill_header(&h->h, PKT_AUDIO_FRAME, frame.timestamp_ms);
                    h->num_samples = frame.num_samples;
                    h->energy = frame.energy;
                    size_t bytes = frame.num_samples * sizeof(int16_t);
                    memcpy(s_tx_buf + sizeof(*h), frame.samples, bytes);
                    send_to(&dest, s_tx_buf, sizeof(*h) + bytes);
                }
                break;
            }
        }
    }
}

// ======================= RECEPÇÃO ===========================================

static void post_reply(OutEventType type, const struct sockaddr_in *to, uint8_t accepted) {
    OutEvent ev = {.type = type, .has_dest = true, .dest = *to,
                   .timestamp_ms = app_state_session_ms()};
    ev.u.accepted = accepted;
    app_state_post(&ev);
}

static bool same_peer(const struct sockaddr_in *a, const struct sockaddr_in *b) {
    return a->sin_addr.s_addr == b->sin_addr.s_addr && a->sin_port == b->sin_port;
}

static void handle_packet(const uint8_t *buf, int len, const struct sockaddr_in *from) {
    if (len < (int)sizeof(PacketHeader)) return;
    const PacketHeader *h = (const PacketHeader *)buf;
    if (h->magic != PROTO_MAGIC || h->version != PROTO_VERSION) return;   // Não é do app

    struct sockaddr_in peer;
    bool paired = app_state_get_peer(&peer);
    bool from_peer = paired && same_peer(&peer, from);

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
            app_state_session_start(p->bpm, p->beats_per_bar, p->count_in_bars, p->flags, false);
            break;
        }

        case PKT_SESSION_STOP:
            if (from_peer) app_state_session_stop();
            break;

        case PKT_SET_TEMPO: {
            if (!from_peer || len < (int)sizeof(PktSetTempo)) return;
            const PktSetTempo *p = (const PktSetTempo *)buf;
            metronome_set_tempo(p->bpm);
            break;
        }

        default:
            break;
    }
}

static void rx_task(void *arg) {
    (void)arg;
    uint8_t buf[128];
    while (1) {
        struct sockaddr_in from;
        socklen_t from_len = sizeof(from);
        int len = recvfrom(s_sock, buf, sizeof(buf), 0, (struct sockaddr *)&from, &from_len);
        if (len > 0) {
            handle_packet(buf, len, &from);
        }

        // App parou de responder: encerra a sessão e libera o dispositivo
        if (app_state_is_paired() && app_state_ms_since_touch() > SESSION_TIMEOUT_MS) {
            ESP_LOGW(TAG, "App sem resposta há %d ms, desconectando", SESSION_TIMEOUT_MS);
            app_state_clear_peer();
        }
    }
}

// ======================= CICLO DE VIDA ======================================

bool udp_link_init(void) {
    s_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_sock < 0) {
        ESP_LOGE(TAG, "Falha ao criar socket");
        return false;
    }
    int yes = 1;
    setsockopt(s_sock, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes));
    struct timeval tv = {.tv_sec = 0, .tv_usec = 500000};   // Timeout de 500 ms no recvfrom
    setsockopt(s_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PROTO_DEVICE_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(s_sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "Falha no bind da porta %d", PROTO_DEVICE_PORT);
        udp_link_deinit();
        return false;
    }
    ESP_LOGI(TAG, "Aguardando o app na porta UDP %d", PROTO_DEVICE_PORT);
    return true;
}

void udp_link_start_tasks(void) {
    // Consumidora de rede: núcleo 0, prioridade abaixo da captura de áudio
    xTaskCreatePinnedToCore(tx_task, "net_tx", 4096, NULL, 4, NULL, 0);
    xTaskCreatePinnedToCore(rx_task, "net_rx", 4096, NULL, 3, NULL, 0);
}

uint32_t udp_link_packets_sent(void) { return s_sent; }
uint32_t udp_link_send_errors(void) { return s_errors; }

void udp_link_deinit(void) {
    if (s_sock >= 0) {
        close(s_sock);                                  // Fecha o socket
        s_sock = -1;
    }
}
