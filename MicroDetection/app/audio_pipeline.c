/*
 * ============================================================================
 * app/audio_pipeline – implementação da tarefa de áudio
 * ============================================================================
 */
#include "audio_pipeline.h"

#include <math.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app_state.h"
#include "config.h"
#include "hal_audio.h"
#include "hal_time.h"
#include "dsp.h"
#include "note_tracker.h"
#include "protocol.h"

static const char *TAG = "audio";

static BandpassFilter  s_filter;                    // Passa-banda 70 Hz – 2,5 kHz
static YinDetector     s_yin;                       // Detector de altura
static NoteTracker     s_tracker;                   // Segmentação de notas
static float           s_window[WINDOW_SAMPLES];    // Janela deslizante de análise
static float           s_yin_diff[WINDOW_SAMPLES / 2 + 1];
static float           s_hop[HOP_SAMPLES];          // Trecho mais recente
static AudioFrame      s_frame;                     // Quadro bruto em montagem

static portMUX_TYPE    s_mask_lock = portMUX_INITIALIZER_UNLOCKED;
static uint32_t        s_mask_until_ms = 0;
static float           s_mask_hz = 0.0f;
static bool            s_mask_pending = false;

static volatile uint32_t s_process_us = 0;
static volatile float    s_level_db = -120.0f;
static volatile float    s_last_pitch = 0.0f;

bool audio_pipeline_init(void) {
    if (!hal_audio_init(HOP_SAMPLES)) return false;

    bandpass_init(&s_filter, SAMPLE_RATE, BANDPASS_LOW_HZ, BANDPASS_HIGH_HZ);
    yin_init(&s_yin, SAMPLE_RATE, WINDOW_SAMPLES, PITCH_MIN_HZ, PITCH_MAX_HZ,
             YIN_THRESHOLD, s_yin_diff);

    NoteTrackerConfig cfg;
    note_tracker_default_config(&cfg);
    cfg.gate_margin_db = GATE_MARGIN_DB;
    cfg.gate_min_db = GATE_MIN_DB;
    cfg.release_hysteresis_db = RELEASE_HYSTERESIS_DB;
    cfg.decay_release_db = DECAY_RELEASE_DB;
    cfg.reattack_db = REATTACK_DB;
    cfg.min_confidence = MIN_CONFIDENCE;
    cfg.stable_hops = STABLE_HOPS;
    cfg.onset_timeout_hops = ONSET_TIMEOUT_HOPS;
    cfg.release_hops = RELEASE_HOPS;
    cfg.quantize_ms = QUANTIZE_MS;
    note_tracker_init(&s_tracker, &cfg);

    memset(s_window, 0, sizeof(s_window));
    ESP_LOGI(TAG, "Passa-banda %.0f-%.0f Hz, YIN %.0f-%.0f Hz (tau %u..%u)",
             BANDPASS_LOW_HZ, BANDPASS_HIGH_HZ, PITCH_MIN_HZ, PITCH_MAX_HZ,
             (unsigned)s_yin.tau_min, (unsigned)s_yin.tau_max);
    return true;
}

void audio_pipeline_mask_click(uint32_t until_ms, float click_hz) {
    portENTER_CRITICAL(&s_mask_lock);
    s_mask_until_ms = until_ms;
    s_mask_hz = click_hz;
    s_mask_pending = true;
    portEXIT_CRITICAL(&s_mask_lock);
}

uint32_t audio_pipeline_process_time_us(void) { return s_process_us; }
float audio_pipeline_level_db(void) { return s_level_db; }
float audio_pipeline_last_pitch_hz(void) { return s_last_pitch; }

/* Publica um evento de nota na fila de saída e no terminal serial. */
static void publish_note(const NoteEvent *ev, bool local) {
    OutEvent out = {
        .type = OUT_NOTE,
        .has_dest = false,
        .timestamp_ms = ev->time_ms,
    };
    out.u.note = *ev;
    if (!local) app_state_post(&out);

    float hz = ev->frequency_hz;
    if (ev->type == NOTE_EVENT_ON) {
        ESP_LOGI(TAG, "NOTE_ON  midi=%u %.1f Hz t=%lu ms conf=%.2f", ev->midi, hz,
                 (unsigned long)ev->time_ms, ev->confidence);
    } else {
        ESP_LOGI(TAG, "NOTE_OFF midi=%u dur=%lu ms", ev->midi, (unsigned long)ev->duration_ms);
    }
}

/**
 * @brief Tarefa produtora (Core 1): lê o I2S, processa e publica eventos.
 */
static void audio_task(void *arg) {
    (void)arg;
    uint32_t generation = 0;
    bool was_active = false;
    int hop_counter = 0;
    size_t frame_fill = 0;

    hal_audio_start();

    while (1) {
        // Instante da primeira amostra do trecho que será lido
        int64_t hop_start_us = hal_audio_next_sample_time_us();
        if (hal_audio_read(s_hop, HOP_SAMPLES, 200) != HOP_SAMPLES) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        int64_t t_begin = hal_time_us();

        // 1) Filtragem passa-banda (RNFE03)
        bandpass_process(&s_filter, s_hop, HOP_SAMPLES);

        // 2) Atualiza a janela deslizante de 1024 amostras
        memmove(s_window, s_window + HOP_SAMPLES, (WINDOW_SAMPLES - HOP_SAMPLES) * sizeof(float));
        memcpy(s_window + WINDOW_SAMPLES - HOP_SAMPLES, s_hop, HOP_SAMPLES * sizeof(float));

        // 3) Nível e altura (RFE05)
        float hop_db = dsp_rms_db(s_hop, HOP_SAMPLES);
        float confidence = 0.0f;
        float freq = yin_detect(&s_yin, s_window, &confidence);
        s_level_db = hop_db;
        s_last_pitch = freq;

        // 4) Estado da sessão
        SessionInfo session = app_state_session();
        if (session.generation != generation) {
            if (was_active) {
                NoteEvent off;
                uint32_t now_ms = (uint32_t)((hop_start_us - session.t0_us) / 1000);
                if (note_tracker_flush(&s_tracker, now_ms, &off)) publish_note(&off, true);
            }
            note_tracker_reset(&s_tracker);             // Mantém o piso de ruído aprendido
            generation = session.generation;
            frame_fill = 0;
        }
        was_active = session.active;

        int64_t rel_us = hop_start_us - session.t0_us;
        uint32_t hop_ms = rel_us > 0 ? (uint32_t)(rel_us / 1000) : 0;

        // Máscara do clique do metrônomo
        portENTER_CRITICAL(&s_mask_lock);
        bool mask = s_mask_pending;
        uint32_t mask_until = s_mask_until_ms;
        float mask_hz = s_mask_hz;
        s_mask_pending = false;
        portEXIT_CRITICAL(&s_mask_lock);
        if (mask) note_tracker_mask_until(&s_tracker, mask_until, mask_hz);

        // 5) Segmentação em notas (somente trechos após o início da sessão)
        NoteEvent events[2];
        int n = note_tracker_process(&s_tracker, hop_ms, hop_db, freq, confidence, events);
        app_state_set_noise_floor(s_tracker.noise_floor_db);
        if (session.active && rel_us >= 0) {
            for (int i = 0; i < n; i++) publish_note(&events[i], session.local);

            // Altura contínua a cada 2 janelas (~32 ms)
            if ((session.flags & CFG_FLAG_STREAM_PITCH) && (++hop_counter % 2) == 0) {
                OutEvent p = { .type = OUT_PITCH, .timestamp_ms = hop_ms };
                p.u.pitch.frequency_hz = confidence >= MIN_CONFIDENCE ? freq : 0.0f;
                p.u.pitch.confidence = confidence;
                p.u.pitch.level_db = hop_db;
                app_state_post(&p);
            }

            // 6) Quadros brutos para diagnóstico (1024 amostras = 4 janelas)
            if (session.flags & CFG_FLAG_STREAM_AUDIO) {
                if (frame_fill == 0) s_frame.timestamp_ms = hop_ms;
                for (size_t i = 0; i < HOP_SAMPLES; i++) {
                    float v = s_hop[i] * 32767.0f;
                    if (v > 32767.0f) v = 32767.0f;             // Saturação segura
                    if (v < -32768.0f) v = -32768.0f;
                    s_frame.samples[frame_fill + i] = (int16_t)v;
                }
                frame_fill += HOP_SAMPLES;
                if (frame_fill >= BLOCK_SAMPLES) {
                    double energy = 0;
                    for (size_t i = 0; i < BLOCK_SAMPLES; i++) {
                        energy += (double)s_frame.samples[i] * s_frame.samples[i];
                    }
                    s_frame.num_samples = BLOCK_SAMPLES;
                    s_frame.energy = (float)(energy / BLOCK_SAMPLES);
                    circ_buffer_push(app_state_audio_buffer(), &s_frame);
                    OutEvent a = { .type = OUT_AUDIO_READY, .timestamp_ms = s_frame.timestamp_ms };
                    app_state_post(&a);
                    frame_fill = 0;
                }
            }
        }

        // Tempo de processamento por janela (RNFE01: deve ficar bem abaixo de 16 ms)
        uint32_t spent = (uint32_t)(hal_time_us() - t_begin);
        s_process_us = (s_process_us * 7 + spent) / 8;
    }
}

void audio_pipeline_start_task(void) {
    // Produtora: núcleo 1, prioridade mais alta para não perder amostras
    xTaskCreatePinnedToCore(audio_task, "audio_prod", 6144, NULL, 5, NULL, 1);
}

void audio_pipeline_deinit(void) {
    hal_audio_deinit();
}
