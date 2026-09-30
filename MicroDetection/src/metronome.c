/*
 * ============================================================================
 * Implementação do metrônomo (esp_timer one-shot reagendado a cada batida)
 * ============================================================================
 */
#include "metronome.h"

#include <stdbool.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

#include "app_state.h"
#include "audio_pipeline.h"
#include "config.h"
#include "indicators.h"
#include "protocol.h"

static const char *TAG = "metro";

#define CLICK_ACCENT_HZ     1760            // Clique do tempo forte (Lá 6)
#define CLICK_BEAT_HZ       880             // Clique dos demais tempos (Lá 5)
#define CLICK_MS            30              // Duração do clique

static esp_timer_handle_t s_timer;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

static bool     s_running = false;
static bool     s_test = false;             // Modo de teste (sem sessão)
static uint16_t s_bpm = 100;
static uint16_t s_pending_bpm = 0;          // Novo BPM aplicado na próxima batida
static uint8_t  s_beats_per_bar = 4;
static uint8_t  s_count_in_bars = 2;
static uint8_t  s_flags = 0;
static int64_t  s_t0_us = 0;
static int64_t  s_next_beat_us = 0;
static uint32_t s_beat_index = 0;           // Batidas desde o início
static uint32_t s_test_remaining = 0;

static int64_t beat_period_us(uint16_t bpm) {
    return 60000000LL / (bpm > 0 ? bpm : 60);
}

/* Cor do metrônomo visual conforme a fórmula de compasso. */
static void meter_color(uint8_t beats, bool count_in, uint8_t *r, uint8_t *g, uint8_t *b) {
    if (count_in) { *r = 255; *g = 180; *b = 0; return; }       // Amarelo
    switch (beats) {
        case 2:  *r = 0;   *g = 80;  *b = 255; break;           // Azul (binário)
        case 3:  *r = 0;   *g = 255; *b = 60;  break;           // Verde (ternário)
        default: *r = 180; *g = 0;   *b = 255; break;           // Roxo (quaternário)
    }
}

static void beat_cb(void *arg) {
    (void)arg;
    portENTER_CRITICAL(&s_lock);
    if (!s_running) {
        portEXIT_CRITICAL(&s_lock);
        return;
    }
    uint32_t index = s_beat_index++;
    uint8_t beats = s_beats_per_bar;
    uint8_t flags = s_flags;
    uint16_t bpm = s_bpm;
    int64_t beat_time = s_next_beat_us;
    bool test = s_test;
    if (s_pending_bpm) {                                    // Troca de andamento
        s_bpm = s_pending_bpm;
        s_pending_bpm = 0;
    }
    s_next_beat_us += beat_period_us(s_bpm);
    int64_t next = s_next_beat_us;
    if (test && s_test_remaining > 0 && --s_test_remaining == 0) s_running = false;
    bool keep = s_running;
    portEXIT_CRITICAL(&s_lock);

    uint8_t beat_in_bar = index % beats;
    uint32_t bar = index / beats;
    bool count_in = bar < s_count_in_bars;
    bool accent = beat_in_bar == 0;
    uint32_t t_ms = (uint32_t)((beat_time - s_t0_us) / 1000);

    // Som: sempre durante a contagem (aviso sonoro – RU07), depois conforme a opção
    if ((flags & CFG_FLAG_METRO_SOUND) || count_in) {
        float hz = accent ? CLICK_ACCENT_HZ : CLICK_BEAT_HZ;
        indicators_beep((uint32_t)hz, CLICK_MS);
        audio_pipeline_mask_click(t_ms + CLICK_MASK_MS, hz);   // Não confundir o clique com nota
    }
    // Luz: sempre durante a contagem (aviso visual), depois conforme a opção
    if ((flags & CFG_FLAG_METRO_VISUAL) || count_in) {
        uint8_t r, g, b;
        meter_color(beats, count_in, &r, &g, &b);
        if (!accent) { r /= 4; g /= 4; b /= 4; }               // Tempo fraco mais fraco
        indicators_flash_rgb(r, g, b, accent ? 150 : 80);
    }

    if (!test) {
        OutEvent ev = {.type = OUT_BEAT, .timestamp_ms = t_ms};
        ev.u.beat.beat_in_bar = beat_in_bar;
        ev.u.beat.count_in = count_in;
        ev.u.beat.bar_index = (uint16_t)bar;
        ev.u.beat.bpm = bpm;
        app_state_post(&ev);
    }

    if (keep) {
        int64_t delay = next - esp_timer_get_time();
        if (delay < 100) delay = 100;
        esp_timer_start_once(s_timer, (uint64_t)delay);
    }
}

void metronome_init(void) {
    const esp_timer_create_args_t args = {.callback = beat_cb, .name = "metronome"};
    ESP_ERROR_CHECK(esp_timer_create(&args, &s_timer));
}

static void start_internal(uint16_t bpm, uint8_t beats_per_bar, uint8_t count_in_bars,
                           uint8_t flags, int64_t t0_us, bool test, uint32_t test_beats) {
    esp_timer_stop(s_timer);
    portENTER_CRITICAL(&s_lock);
    s_bpm = bpm ? bpm : 60;
    s_pending_bpm = 0;
    s_beats_per_bar = (beats_per_bar >= 2 && beats_per_bar <= 12) ? beats_per_bar : 4;
    s_count_in_bars = count_in_bars;
    s_flags = flags;
    s_t0_us = t0_us;
    s_next_beat_us = t0_us;
    s_beat_index = 0;
    s_test = test;
    s_test_remaining = test_beats;
    s_running = true;
    portEXIT_CRITICAL(&s_lock);

    int64_t delay = t0_us - esp_timer_get_time();
    if (delay < 100) delay = 100;
    esp_timer_start_once(s_timer, (uint64_t)delay);
}

void metronome_start(uint16_t bpm, uint8_t beats_per_bar, uint8_t count_in_bars,
                     uint8_t flags, int64_t t0_us) {
    start_internal(bpm, beats_per_bar, count_in_bars, flags, t0_us, false, 0);
    ESP_LOGI(TAG, "Metrônomo: %u BPM, %u/4, som=%d luz=%d", bpm, beats_per_bar,
             (flags & CFG_FLAG_METRO_SOUND) != 0, (flags & CFG_FLAG_METRO_VISUAL) != 0);
}

void metronome_set_tempo(uint16_t bpm) {
    if (bpm == 0) return;
    portENTER_CRITICAL(&s_lock);
    s_pending_bpm = bpm;
    portEXIT_CRITICAL(&s_lock);
    ESP_LOGI(TAG, "Novo andamento na próxima batida: %u BPM", bpm);
}

void metronome_set_flags(uint8_t flags) {
    portENTER_CRITICAL(&s_lock);
    s_flags = flags;
    portEXIT_CRITICAL(&s_lock);
}

void metronome_stop(void) {
    portENTER_CRITICAL(&s_lock);
    s_running = false;
    portEXIT_CRITICAL(&s_lock);
    esp_timer_stop(s_timer);
    indicators_flash_rgb(0, 0, 0, 1);
}

void metronome_test(uint16_t bpm, uint8_t beats_per_bar) {
    start_internal(bpm, beats_per_bar, 0, CFG_FLAG_METRO_SOUND | CFG_FLAG_METRO_VISUAL,
                   esp_timer_get_time() + 1000, true, (uint32_t)beats_per_bar * 2);
}
