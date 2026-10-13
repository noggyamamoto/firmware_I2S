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
#include "execution_monitor.h"
#include "hal_audio.h"
#include "hal_time.h"
#include "dsp.h"
#include "note_tracker.h"
#include "protocol.h"

static const char *TAG = "audio";

static BandpassFilter  s_filter;                    // Passa-banda 20 Hz – 5 kHz
static PitchDetector   s_pitch;                     // Altura: A0 a C8 (duas resoluções)
static NoteTracker     s_tracker;                   // Segmentação de notas
static float           s_hop[HOP_SAMPLES];          // Trecho mais recente

static portMUX_TYPE    s_mask_lock = portMUX_INITIALIZER_UNLOCKED;
static uint32_t        s_mask_from_ms = 0;
static uint32_t        s_mask_until_ms = 0;
static float           s_mask_hz = 0.0f;
static bool            s_mask_pending = false;

static float           s_probe_db[PITCH_PROBE_COUNT];   // Afinador: f/4, f/3, f/2, f, 2f
static portMUX_TYPE    s_probe_lock = portMUX_INITIALIZER_UNLOCKED;
static volatile uint32_t  s_process_us = 0;
static volatile float     s_level_db = -120.0f;
static volatile float     s_last_pitch = 0.0f;
static volatile MicStatus s_mic_status = MIC_STATUS_STARTING;

// Últimas notas detectadas (monitor de notas do menu serial)
#define RECENT_NOTES        8
static NoteEvent      s_recent[RECENT_NOTES];
static int            s_recent_next = 0;
static int            s_recent_count = 0;
static uint32_t       s_recent_version = 0;
static portMUX_TYPE   s_recent_lock = portMUX_INITIALIZER_UNLOCKED;

// Verificação do microfone (janelas de 16 ms)
#define MIC_CHECK_HOPS      62                      // ~1 s de observação
#define MIC_READ_FAILS      5                       // Leituras falhas seguidas = erro de I2S
#define CLIP_LEVEL          (0.999f * INPUT_GAIN)   // Próximo do fundo de escala de 24 bits
#define RATE_TOLERANCE      0.10f                   // Taxa medida > 10% fora: corrige alturas

bool audio_pipeline_init(void) {
    if (!hal_audio_init(HOP_SAMPLES)) {
        s_mic_status = MIC_STATUS_INIT_FAILED;
        return false;
    }

    bandpass_init(&s_filter, SAMPLE_RATE, BANDPASS_LOW_HZ, BANDPASS_HIGH_HZ);
    pitch_init(&s_pitch, SAMPLE_RATE, PITCH_MIN_HZ, PITCH_MAX_HZ, PITCH_SPLIT_HZ, YIN_THRESHOLD);
    pitch_reset(&s_pitch);

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
    cfg.min_midi = MIN_MIDI_NOTE;
    cfg.max_midi = MAX_MIDI_NOTE;
    note_tracker_init(&s_tracker, &cfg);

    ESP_LOGI(TAG, "Passa-banda %.0f-%.0f Hz, altura %.1f-%.0f Hz (A0 a C8)",
             BANDPASS_LOW_HZ, BANDPASS_HIGH_HZ, PITCH_MIN_HZ, PITCH_MAX_HZ);
    return true;
}

void audio_pipeline_mask_click(uint32_t click_ms, float click_hz) {
    // A janela de análise (32 ms) termina no fim do trecho (16 ms): o clique
    // aparece nos trechos que começam até 16 ms antes dele
    const uint32_t hop_ms = HOP_SAMPLES * 1000 / SAMPLE_RATE;
    portENTER_CRITICAL(&s_mask_lock);
    s_mask_from_ms = click_ms > hop_ms ? click_ms - hop_ms : 0;
    s_mask_until_ms = click_ms + CLICK_MASK_MS;
    s_mask_hz = click_hz;
    s_mask_pending = true;
    portEXIT_CRITICAL(&s_mask_lock);
}

uint32_t audio_pipeline_process_time_us(void) { return s_process_us; }
uint32_t audio_pipeline_clock_resyncs(void) { return hal_audio_resync_count(); }

float audio_pipeline_rate_scale(void) {
    const float measured = hal_audio_measured_rate();
    if (measured <= 0.0f) return 1.0f;
    const float scale = measured / (float)SAMPLE_RATE;
    const bool processing_ok = s_process_us < (uint32_t)(HOP_SAMPLES * 1000000u / SAMPLE_RATE / 2);
    return (fabsf(scale - 1.0f) > RATE_TOLERANCE && processing_ok) ? scale : 1.0f;
}

float audio_pipeline_measured_rate(void) {
    return hal_audio_measured_rate();
}

void audio_pipeline_harmonic_levels(float out_db[PITCH_PROBE_COUNT]) {
    portENTER_CRITICAL(&s_probe_lock);
    memcpy(out_db, s_probe_db, sizeof(s_probe_db));
    portEXIT_CRITICAL(&s_probe_lock);
}
float audio_pipeline_level_db(void) { return s_level_db; }
float audio_pipeline_last_pitch_hz(void) { return s_last_pitch; }
MicStatus audio_pipeline_mic_status(void) { return s_mic_status; }

int audio_pipeline_recent_notes(NoteEvent *out, int max, uint32_t *version) {
    portENTER_CRITICAL(&s_recent_lock);
    int n = s_recent_count < max ? s_recent_count : max;
    for (int i = 0; i < n; i++) {                   // Da mais nova para a mais antiga
        out[i] = s_recent[(s_recent_next - 1 - i + RECENT_NOTES) % RECENT_NOTES];
    }
    if (version) *version = s_recent_version;
    portEXIT_CRITICAL(&s_recent_lock);
    return n;
}

/* Atualiza a situação do microfone a partir das amostras brutas do trecho. */
static void check_microphone(const float *x, size_t n) {
    static int flat_hops = 0, clip_hops = 0, hops = 0;
    float lo = x[0], hi = x[0];
    for (size_t i = 1; i < n; i++) {
        if (x[i] < lo) lo = x[i];
        if (x[i] > hi) hi = x[i];
    }
    // INMP441 ativo sempre tem ruído de fundo: amostras idênticas = sem microfone
    flat_hops = (hi == lo) ? flat_hops + 1 : 0;
    if (hi >= CLIP_LEVEL || lo <= -CLIP_LEVEL) clip_hops++;

    if (flat_hops >= MIC_CHECK_HOPS) {
        s_mic_status = MIC_STATUS_NO_SIGNAL;
    } else if (++hops >= MIC_CHECK_HOPS) {
        // Saturado em mais de 20% das janelas do último segundo
        s_mic_status = clip_hops * 5 > hops ? MIC_STATUS_CLIPPING : MIC_STATUS_OK;
        hops = 0;
        clip_hops = 0;
    } else if (s_mic_status == MIC_STATUS_NO_SIGNAL || s_mic_status == MIC_STATUS_READ_ERROR) {
        if (flat_hops == 0) s_mic_status = MIC_STATUS_OK;   // Voltou a ter sinal
    }
}

/* Publica um evento de nota na fila de saída e no terminal serial. */
static void publish_note(const NoteEvent *ev, bool local) {
    OutEvent out = {
        .type = OUT_NOTE,
        .has_dest = false,
        .timestamp_ms = ev->time_ms,
    };
    out.u.note = *ev;
    if (!local) {
        app_state_post(&out);
        // Monitor serial (só registra), com a energia em f/2 e f/4 medida no
        // trecho mais recente (diagnóstico de oitava)
        float probe[PITCH_PROBE_COUNT];
        portENTER_CRITICAL(&s_probe_lock);
        memcpy(probe, s_probe_db, sizeof(probe));
        portEXIT_CRITICAL(&s_probe_lock);
        const float half_quarter[2] = {probe[2], probe[0]};
        execution_monitor_captured(ev, ev->type == NOTE_EVENT_ON ? half_quarter : NULL);
    }

    portENTER_CRITICAL(&s_recent_lock);
    s_recent[s_recent_next] = *ev;
    s_recent_next = (s_recent_next + 1) % RECENT_NOTES;
    if (s_recent_count < RECENT_NOTES) s_recent_count++;
    s_recent_version++;
    portEXIT_CRITICAL(&s_recent_lock);

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
    int read_fails = 0;
    int yield_counter = 0;
    size_t frame_fill = 0;
    AudioFrame *frame = NULL;                       // Slot do buffer circular em preenchimento

    if (!hal_audio_start()) s_mic_status = MIC_STATUS_READ_ERROR;

    while (1) {
        if (hal_audio_read(s_hop, HOP_SAMPLES, 200) != HOP_SAMPLES) {
            if (++read_fails >= MIC_READ_FAILS) s_mic_status = MIC_STATUS_READ_ERROR;
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        read_fails = 0;
        // Instante da primeira amostra do trecho lido (relógio do sistema)
        int64_t hop_start_us = hal_audio_block_time_us();
        int64_t t_begin = hal_time_us();
        check_microphone(s_hop, HOP_SAMPLES);

        // 1) Filtragem passa-banda (RNFE03)
        bandpass_process(&s_filter, s_hop, HOP_SAMPLES);

        // 2) Nível e altura (RFE05): janelas de 32 ms (agudos) e 80 ms
        //    (graves), com correção de oitava (harmônico forte não vira a nota)
        float hop_db = dsp_rms_db(s_hop, HOP_SAMPLES);
        float confidence = 0.0f;
        float freq = pitch_process(&s_pitch, s_hop, HOP_SAMPLES, &confidence);
        // Taxa real do I2S diferente da configurada (> 10%) com folga de
        // processamento (não são amostras perdidas): corrige a escala das
        // alturas, que seriam todas deslocadas na mesma proporção
        const float window_freq = freq;                 // Na escala da janela (sonda)
        const float scale = audio_pipeline_rate_scale();
        if (freq > 0.0f && scale != 1.0f) freq *= scale;
        s_level_db = hop_db;
        s_last_pitch = freq;
        if (freq > 0.0f) {
            float probe[PITCH_PROBE_COUNT];
            pitch_harmonic_levels(&s_pitch, window_freq, probe);
            portENTER_CRITICAL(&s_probe_lock);
            memcpy(s_probe_db, probe, sizeof(probe));
            portEXIT_CRITICAL(&s_probe_lock);
        }

        // 4) Estado da sessão
        SessionInfo session = app_state_session();
        if (session.generation != generation) {
            if (was_active) {
                NoteEvent off[2];
                uint32_t now_ms = (uint32_t)((hop_start_us - session.t0_us) / 1000);
                int k = note_tracker_flush(&s_tracker, now_ms, off);
                for (int i = 0; i < k; i++) publish_note(&off[i], true);
            }
            note_tracker_reset(&s_tracker);             // Mantém o piso de ruído aprendido
            generation = session.generation;
            frame_fill = 0;                             // Descarta o quadro incompleto
            frame = NULL;
        }
        was_active = session.active;

        int64_t rel_us = hop_start_us - session.t0_us;
        uint32_t hop_ms = rel_us > 0 ? (uint32_t)(rel_us / 1000) : 0;

        // Máscara do clique do metrônomo
        portENTER_CRITICAL(&s_mask_lock);
        bool mask = s_mask_pending;
        uint32_t mask_from = s_mask_from_ms;
        uint32_t mask_until = s_mask_until_ms;
        float mask_hz = s_mask_hz;
        s_mask_pending = false;
        portEXIT_CRITICAL(&s_mask_lock);
        if (mask) note_tracker_mask(&s_tracker, mask_from, mask_until, mask_hz);

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

            // 6) Quadros brutos para diagnóstico (1024 amostras = 4 janelas),
            //    escritos direto no slot do buffer circular (sem cópia)
            if (session.flags & CFG_FLAG_STREAM_AUDIO) {
                if (frame_fill == 0) {
                    frame = circ_buffer_write_slot(app_state_audio_buffer());
                    if (frame) frame->packet.h.timestamp_ms = hop_ms;
                }
                if (frame) {
                    for (size_t i = 0; i < HOP_SAMPLES; i++) {
                        float v = s_hop[i] * 32767.0f;
                        if (v > 32767.0f) v = 32767.0f;         // Saturação segura
                        if (v < -32768.0f) v = -32768.0f;
                        frame->samples[frame_fill + i] = (int16_t)v;
                    }
                }
                frame_fill += HOP_SAMPLES;
                if (frame_fill >= BLOCK_SAMPLES) {
                    if (frame) {                                // Buffer cheio: quadro descartado
                        double energy = 0;
                        for (size_t i = 0; i < BLOCK_SAMPLES; i++) {
                            energy += (double)frame->samples[i] * frame->samples[i];
                        }
                        frame->generation = generation;
                        frame->packet.num_samples = BLOCK_SAMPLES;
                        frame->packet.energy = (float)(energy / BLOCK_SAMPLES);
                        uint32_t ts = frame->packet.h.timestamp_ms;
                        circ_buffer_commit(app_state_audio_buffer());
                        OutEvent a = { .type = OUT_AUDIO_READY, .timestamp_ms = ts };
                        app_state_post(&a);
                    }
                    frame_fill = 0;
                    frame = NULL;
                }
            }
        }

        // Tempo de processamento por janela (RNFE01: deve ficar bem abaixo de 16 ms)
        uint32_t spent = (uint32_t)(hal_time_us() - t_begin);
        s_process_us = (s_process_us * 7 + spent) / 8;

        // Garantia para o watchdog: se o processamento ficar perto do tempo de
        // uma janela, a leitura nunca bloqueia e a tarefa ociosa do núcleo 1
        // não roda (task_wdt). Cede 1 tick no máximo a cada ~2 s; o DMA guarda
        // ~96 ms de áudio, então nenhuma amostra é perdida.
        if (++yield_counter >= 125) {
            yield_counter = 0;
            if (s_process_us > HOP_SAMPLES * 1000000u / SAMPLE_RATE * 3 / 4) vTaskDelay(1);
        }
    }
}

void audio_pipeline_start_task(void) {
    // Produtora: núcleo 1, prioridade mais alta para não perder amostras
    xTaskCreatePinnedToCore(audio_task, "audio_prod", 6144, NULL, 5, NULL, 1);
}

void audio_pipeline_deinit(void) {
    hal_audio_deinit();
}
