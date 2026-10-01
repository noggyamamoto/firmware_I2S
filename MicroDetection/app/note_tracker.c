/*
 * ============================================================================
 * Implementação da segmentação de notas
 * ============================================================================
 */
#include "note_tracker.h"
#include "dsp.h"

#include <math.h>
#include <string.h>

#define INITIAL_NOISE_FLOOR_DB  -75.0f      // Estimativa inicial do piso de ruído

void note_tracker_default_config(NoteTrackerConfig *cfg) {
    cfg->gate_margin_db = 12.0f;
    cfg->gate_min_db = -62.0f;
    cfg->release_hysteresis_db = 3.0f;
    cfg->decay_release_db = 30.0f;
    cfg->reattack_db = 6.0f;
    cfg->min_confidence = 0.70f;
    cfg->stable_hops = 2;
    cfg->onset_timeout_hops = 8;
    cfg->release_hops = 2;
    cfg->quantize_ms = 10;
}

void note_tracker_init(NoteTracker *t, const NoteTrackerConfig *cfg) {
    memset(t, 0, sizeof(*t));
    t->cfg = *cfg;
    t->noise_floor_db = INITIAL_NOISE_FLOOR_DB;
    t->prev_db = -120.0f;
    note_tracker_reset(t);
}

void note_tracker_reset(NoteTracker *t) {
    t->state = TRACKER_IDLE;
    t->mask_until_ms = 0;
    t->mask_pitch_class = -1;
    t->active_midi = -1;
    t->stable_midi = -1;
    t->change_midi = -1;
    t->cand_over_active = false;
    t->prev_db = -120.0f;
}

void note_tracker_mask_until(NoteTracker *t, uint32_t until_ms, float click_hz) {
    if (until_ms > t->mask_until_ms) t->mask_until_ms = until_ms;
    t->mask_pitch_class = click_hz > 0.0f ? ((int)lroundf(dsp_hz_to_midi(click_hz)) % 12) : -1;
}

float note_tracker_gate_db(const NoteTracker *t) {
    float gate = t->noise_floor_db + t->cfg.gate_margin_db;
    return gate > t->cfg.gate_min_db ? gate : t->cfg.gate_min_db;
}

/* Arredonda o instante para a grade de quantização (RNFE04). */
static uint32_t quantize(const NoteTracker *t, uint32_t ms) {
    uint32_t q = t->cfg.quantize_ms;
    if (q <= 1) return ms;
    return ((ms + q / 2) / q) * q;
}

/*
 * Atualiza o piso de ruído. Aprende com trechos abaixo do limiar e também
 * com trechos sem altura definida (ruído de conversa, ventilador etc.),
 * para que o limiar acompanhe o ruído ambiente da sala (RNFE03).
 */
static void update_noise_floor(NoteTracker *t, float db, bool voiced) {
    float gate = note_tracker_gate_db(t);
    float alpha;
    if (db < t->noise_floor_db) {
        alpha = 0.20f;                                  // Ambiente ficou mais silencioso
    } else if (db < gate) {
        alpha = 0.03f;                                  // Ruído abaixo do limiar
    } else if (!voiced) {
        alpha = 0.05f;                                  // Ruído forte sem altura definida
    } else {
        return;                                         // Provável nota: não aprende
    }
    t->noise_floor_db += alpha * (db - t->noise_floor_db);
}

static void start_candidate(NoteTracker *t, uint32_t time_ms, float db, bool over_active) {
    t->state = TRACKER_CANDIDATE;
    t->cand_time_ms = time_ms;
    t->cand_hops = 0;
    t->stable_midi = -1;
    t->stable_count = 0;
    t->cand_freq_sum = 0.0f;
    t->cand_conf_sum = 0.0f;
    t->cand_peak_db = db;
    t->cand_over_active = over_active;
}

/* Acumula a altura observada na confirmação do candidato. */
static void candidate_add_pitch(NoteTracker *t, int midi, float freq, float conf) {
    if (midi == t->stable_midi) {
        t->stable_count++;
        t->cand_freq_sum += freq;
        t->cand_conf_sum += conf;
    } else {
        t->stable_midi = midi;
        t->stable_count = 1;
        t->cand_freq_sum = freq;
        t->cand_conf_sum = conf;
    }
}

static void make_on(NoteTracker *t, NoteEvent *e, int midi, uint32_t time_ms,
                    float freq, float level_db, float conf) {
    e->type = NOTE_EVENT_ON;
    e->midi = (uint8_t)midi;
    e->time_ms = quantize(t, time_ms);
    e->duration_ms = 0;
    e->frequency_hz = freq;
    e->level_db = level_db;
    e->confidence = conf;

    t->state = TRACKER_ACTIVE;
    t->active_midi = midi;
    t->active_on_ms = e->time_ms;
    t->active_peak_db = level_db;
    t->active_freq_sum = freq;
    t->active_freq_count = 1;
    t->low_count = 0;
    t->change_midi = -1;
    t->change_count = 0;
    t->cand_over_active = false;
}

static void make_off(NoteTracker *t, NoteEvent *e, uint32_t time_ms) {
    uint32_t off = quantize(t, time_ms);
    if (off < t->active_on_ms) off = t->active_on_ms;
    e->type = NOTE_EVENT_OFF;
    e->midi = (uint8_t)t->active_midi;
    e->time_ms = off;
    e->duration_ms = off - t->active_on_ms;
    e->frequency_hz = t->active_freq_count > 0
                          ? t->active_freq_sum / (float)t->active_freq_count
                          : 0.0f;
    e->level_db = t->active_peak_db;
    e->confidence = 0.0f;
    t->state = TRACKER_IDLE;
    t->active_midi = -1;
    t->cand_over_active = false;
}

/* Candidato descartado: volta à nota anterior (se havia) ou ao silêncio. */
static void abort_candidate(NoteTracker *t) {
    if (t->cand_over_active) {
        t->state = TRACKER_ACTIVE;                      // Foi só um ruído sobre a nota
        t->cand_over_active = false;
        t->low_count = 0;
    } else {
        t->state = TRACKER_IDLE;
    }
}

int note_tracker_process(NoteTracker *t, uint32_t hop_start_ms, float hop_db,
                         float freq_hz, float confidence, NoteEvent out[2]) {
    int n = 0;
    const float gate = note_tracker_gate_db(t);
    const float release = gate - t->cfg.release_hysteresis_db;
    const bool masked = hop_start_ms < t->mask_until_ms;
    bool voiced = freq_hz > 0.0f && confidence >= t->cfg.min_confidence;
    int midi_now = voiced ? (int)lroundf(dsp_hz_to_midi(freq_hz)) : -1;
    const float jump = hop_db - t->prev_db;

    // Durante o clique do metrônomo, a altura do próprio clique é ignorada
    if (masked && voiced && t->mask_pitch_class >= 0 && midi_now % 12 == t->mask_pitch_class) {
        voiced = false;
        midi_now = -1;
    }

    switch (t->state) {
        case TRACKER_IDLE:
            update_noise_floor(t, hop_db, voiced);
            if (hop_db >= note_tracker_gate_db(t)) {
                start_candidate(t, hop_start_ms, hop_db, false);
                if (voiced) candidate_add_pitch(t, midi_now, freq_hz, confidence);
            }
            break;

        case TRACKER_CANDIDATE:
            if (hop_db < release) {
                if (t->cand_over_active) {
                    // A nota anterior terminou enquanto o candidato era avaliado
                    t->state = TRACKER_ACTIVE;
                    make_off(t, &out[n++], t->cand_time_ms);
                } else {
                    t->state = TRACKER_IDLE;            // Ruído impulsivo: descarta
                }
                break;
            }
            t->cand_hops++;
            // Um salto de energia dentro do candidato indica o ataque verdadeiro
            if (!t->cand_over_active && jump >= t->cfg.reattack_db && t->stable_count == 0) {
                t->cand_time_ms = hop_start_ms;
            }
            if (hop_db > t->cand_peak_db) t->cand_peak_db = hop_db;
            if (voiced) {
                candidate_add_pitch(t, midi_now, freq_hz, confidence);
            } else if (!t->cand_over_active) {
                update_noise_floor(t, hop_db, false);   // Ruído sem altura: ajusta o limiar
            }

            if (t->stable_count >= t->cfg.stable_hops) {
                float cnt = (float)t->stable_count;
                if (t->cand_over_active) {
                    make_off(t, &out[n++], t->cand_time_ms);   // Encerra a nota anterior
                }
                make_on(t, &out[n++], t->stable_midi, t->cand_time_ms,
                        t->cand_freq_sum / cnt, t->cand_peak_db, t->cand_conf_sum / cnt);
            } else if (t->cand_hops >= t->cfg.onset_timeout_hops) {
                abort_candidate(t);                     // Som sem altura definida
            }
            break;

        case TRACKER_ACTIVE: {
            // 1) Novo ataque sobre a nota que ainda soa (ex.: nota repetida).
            //    A nota anterior só é encerrada quando a nova altura se confirmar.
            if (jump >= t->cfg.reattack_db && hop_db >= gate) {
                start_candidate(t, hop_start_ms, hop_db, true);
                if (voiced) candidate_add_pitch(t, midi_now, freq_hz, confidence);
                break;
            }

            // 2) Soltura: abaixo do limiar ou decaimento acentuado
            bool low = hop_db < release || hop_db < t->active_peak_db - t->cfg.decay_release_db;
            if (low) {
                if (t->low_count == 0) t->low_start_ms = hop_start_ms;
                t->low_count++;
                if (t->low_count >= t->cfg.release_hops) {
                    make_off(t, &out[n++], t->low_start_ms);
                }
                break;
            }
            t->low_count = 0;
            if (hop_db > t->active_peak_db) t->active_peak_db = hop_db;

            // 3) Troca de altura sem novo ataque (legato)
            if (voiced && midi_now != t->active_midi) {
                if (midi_now == t->change_midi) {
                    t->change_count++;
                } else {
                    t->change_midi = midi_now;
                    t->change_count = 1;
                    t->change_start_ms = hop_start_ms;
                }
                if (t->change_count > t->cfg.stable_hops) {
                    uint32_t at = t->change_start_ms;
                    int new_midi = t->change_midi;
                    make_off(t, &out[n++], at);
                    make_on(t, &out[n++], new_midi, at, freq_hz, hop_db, confidence);
                }
            } else if (voiced) {
                t->change_count = 0;
                t->change_midi = -1;
                t->active_freq_sum += freq_hz;
                t->active_freq_count++;
            }
            break;
        }
    }

    t->prev_db = hop_db;
    return n;
}

int note_tracker_flush(NoteTracker *t, uint32_t now_ms, NoteEvent *out) {
    if (t->state == TRACKER_ACTIVE || (t->state == TRACKER_CANDIDATE && t->cand_over_active)) {
        t->state = TRACKER_ACTIVE;
        make_off(t, out, now_ms);
        return 1;
    }
    t->state = TRACKER_IDLE;
    return 0;
}
