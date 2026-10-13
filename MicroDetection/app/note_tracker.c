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
    cfg->settle_hops = 3;                               // 48 ms para confirmar a oitava
    cfg->min_midi = 21;                                 // A0
    cfg->max_midi = 108;                                // C8
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
    t->pending = false;
    t->mask_from_ms = 0;
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

void note_tracker_mask(NoteTracker *t, uint32_t from_ms, uint32_t until_ms, float click_hz) {
    t->mask_from_ms = from_ms;
    t->mask_until_ms = until_ms;
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

/*
 * Início de nota com confirmação de oitava: o NOTE_ON fica retido por
 * `settle_hops` trechos. Nos primeiros trechos a janela ainda está meio
 * vazia e um harmônico forte pode ser lido no lugar da nota (ex.: E6 no
 * lugar de E4 quando o som tem quase só o 4º harmônico); se nesse intervalo
 * as leituras mostrarem um submúltiplo exato estável, a nota sai com a
 * altura corrigida. O instante do ataque não muda.
 * Retorna quantos eventos escreveu em `out` (0 ou 1).
 */
static int begin_note(NoteTracker *t, NoteEvent *out, int midi, uint32_t time_ms,
                      float freq, float level_db, float conf) {
    make_on(t, &t->pending_on, midi, time_ms, freq, level_db, conf);
    t->sub_midi = -1;
    t->sub_count = 0;
    t->sub_freq_sum = 0.0f;
    if (t->cfg.settle_hops <= 0) {
        *out = t->pending_on;
        return 1;
    }
    t->pending = true;
    t->settle_left = t->cfg.settle_hops;
    return 0;
}

/* Publica o NOTE_ON retido (antes de qualquer NOTE_OFF ou nova nota). */
static int emit_pending(NoteTracker *t, NoteEvent *out) {
    if (!t->pending) return 0;
    t->pending = false;
    *out = t->pending_on;
    return 1;
}

/* `freq` é submúltiplo exato (1/2 a 1/8, ±3%) da nota ativa. */
static bool submultiple_of_active(const NoteTracker *t, float freq) {
    if (t->active_freq_count <= 0 || freq <= 0.0f) return false;
    float r = (t->active_freq_sum / (float)t->active_freq_count) / freq;
    int k = (int)lroundf(r);
    return k >= 2 && k <= 8 && fabsf(r - (float)k) <= 0.03f * (float)k;
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

/* `freq` é múltiplo ou submúltiplo inteiro (2 a 8, ±3%) da nota ativa. */
static bool harmonic_of_active(const NoteTracker *t, float freq) {
    if (t->active_freq_count <= 0 || freq <= 0.0f) return false;
    float active = t->active_freq_sum / (float)t->active_freq_count;
    float r = freq > active ? freq / active : active / freq;
    int k = (int)lroundf(r);
    return k >= 2 && k <= 8 && fabsf(r - (float)k) <= 0.03f * (float)k;
}

int note_tracker_process(NoteTracker *t, uint32_t hop_start_ms, float hop_db,
                         float freq_hz, float confidence, NoteEvent out[2]) {
    int n = 0;
    const float gate = note_tracker_gate_db(t);
    const float release = gate - t->cfg.release_hysteresis_db;
    const bool masked = hop_start_ms >= t->mask_from_ms && hop_start_ms < t->mask_until_ms;
    // Som periódico (tem altura) e, entre eles, os que estão na faixa aceita.
    // Um som periódico fora da faixa (do piano) não vira nota, mas também
    // não é tratado como ruído no aprendizado do piso.
    const bool pitched = freq_hz > 0.0f && confidence >= t->cfg.min_confidence;
    int midi_now = pitched ? (int)lroundf(dsp_hz_to_midi(freq_hz)) : -1;
    bool voiced = pitched && midi_now >= t->cfg.min_midi && midi_now <= t->cfg.max_midi;
    if (!voiced) midi_now = -1;
    const float jump = hop_db - t->prev_db;

    // Durante o clique do metrônomo, a altura do próprio clique é ignorada
    if (masked && voiced && t->mask_pitch_class >= 0 && midi_now % 12 == t->mask_pitch_class) {
        voiced = false;
        midi_now = -1;
    }

    switch (t->state) {
        case TRACKER_IDLE:
            // O clique do metrônomo (sempre presente na contagem de entrada)
            // não pode elevar o piso de ruído: o limiar subiria e as primeiras
            // notas da música, mais fracas, deixariam de ser detectadas
            if (!masked) update_noise_floor(t, hop_db, pitched);
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
                    n += emit_pending(t, &out[n]);
                    make_off(t, &out[n++], t->cand_time_ms);
                } else {
                    t->state = TRACKER_IDLE;            // Ruído impulsivo: descarta
                }
                break;
            }
            t->cand_hops++;
            // Reataque sobre uma nota que soa: um transiente curto (clique do
            // metrônomo, do app ou batida no móvel) some na janela seguinte;
            // um ataque verdadeiro mantém o nível. Volta à nota anterior.
            if (t->cand_over_active && hop_db < t->cand_peak_db - t->cfg.reattack_db) {
                abort_candidate(t);
                break;
            }
            // Um salto de energia logo no início do candidato indica o ataque
            // verdadeiro. Só nos primeiros trechos: em notas muito graves (A0
            // tem período de 36 ms, maior que o trecho de 16 ms) o nível oscila
            // enquanto a altura é confirmada e o ataque seria empurrado
            if (!t->cand_over_active && jump >= t->cfg.reattack_db && t->stable_count == 0 &&
                t->cand_hops <= 2) {
                t->cand_time_ms = hop_start_ms;
            }
            if (hop_db > t->cand_peak_db) t->cand_peak_db = hop_db;
            if (voiced) {
                candidate_add_pitch(t, midi_now, freq_hz, confidence);
            } else if (!t->cand_over_active && !pitched && !masked) {
                update_noise_floor(t, hop_db, false);   // Ruído sem altura: ajusta o limiar
            }

            if (t->stable_count >= t->cfg.stable_hops) {
                float cnt = (float)t->stable_count;
                if (t->cand_over_active) {
                    n += emit_pending(t, &out[n]);
                    make_off(t, &out[n++], t->cand_time_ms);   // Encerra a nota anterior
                }
                n += begin_note(t, &out[n], t->stable_midi, t->cand_time_ms,
                                t->cand_freq_sum / cnt, t->cand_peak_db, t->cand_conf_sum / cnt);
            } else if (t->cand_hops >= t->cfg.onset_timeout_hops) {
                abort_candidate(t);                     // Som sem altura definida
            }
            break;

        case TRACKER_ACTIVE: {
            // 1) Novo ataque sobre a nota que ainda soa (ex.: nota repetida).
            //    A nota anterior só é encerrada quando a nova altura se confirmar.
            if (jump >= t->cfg.reattack_db && hop_db >= gate) {
                n += emit_pending(t, &out[n]);          // A nota que soa já é definitiva
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
                    n += emit_pending(t, &out[n]);
                    make_off(t, &out[n++], t->low_start_ms);
                }
                break;
            }
            t->low_count = 0;
            if (hop_db > t->active_peak_db) t->active_peak_db = hop_db;

            // Confirmação da oitava (NOTE_ON retido): submúltiplo exato e
            // estável da altura lida no ataque corrige a nota
            if (t->pending) {
                if (voiced && submultiple_of_active(t, freq_hz)) {
                    if (midi_now == t->sub_midi) {
                        t->sub_count++;
                        t->sub_freq_sum += freq_hz;
                    } else {
                        t->sub_midi = midi_now;
                        t->sub_count = 1;
                        t->sub_freq_sum = freq_hz;
                    }
                    if (t->sub_count >= 2) {
                        float f = t->sub_freq_sum / (float)t->sub_count;
                        t->active_midi = t->sub_midi;
                        t->active_freq_sum = f;
                        t->active_freq_count = 1;
                        t->pending_on.midi = (uint8_t)t->sub_midi;
                        t->pending_on.frequency_hz = f;
                        t->sub_count = 0;
                        t->sub_midi = -1;
                    }
                }
                if (--t->settle_left <= 0) n += emit_pending(t, &out[n]);
                if (t->pending) break;                  // Sem troca de altura enquanto retido
            }

            // 3) Troca de altura sem novo ataque (legato). Uma "troca" para um
            //    múltiplo ou submúltiplo exato da nota que soa (2x a 8x) sem
            //    ataque é artefato da análise (harmônico ou período longo no
            //    decaimento com ruído), não uma nota nova: no piano toda nota
            //    nova tem ataque, tratado no item 1.
            if (voiced && midi_now != t->active_midi && harmonic_of_active(t, freq_hz)) {
                t->change_count = 0;
                t->change_midi = -1;
            } else if (voiced && midi_now != t->active_midi) {
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
                    n += begin_note(t, &out[n], new_midi, at, freq_hz, hop_db, confidence);
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

int note_tracker_flush(NoteTracker *t, uint32_t now_ms, NoteEvent out[2]) {
    if (t->state == TRACKER_ACTIVE || (t->state == TRACKER_CANDIDATE && t->cand_over_active)) {
        int n = emit_pending(t, &out[0]);
        t->state = TRACKER_ACTIVE;
        make_off(t, &out[n++], now_ms);
        return n;
    }
    t->pending = false;
    t->state = TRACKER_IDLE;
    return 0;
}
