/*
 * ============================================================================
 * Implementação do processamento digital de sinais
 * ============================================================================
 */
#include "dsp.h"

#include <math.h>
#include <stdbool.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// ======================= BIQUADS ============================================

/* Coeficientes de um passa-alta de 2ª ordem (Audio EQ Cookbook, R. Bristow-Johnson). */
static void biquad_highpass(Biquad *bq, float fs, float fc, float q) {
    float w0 = 2.0f * (float)M_PI * fc / fs;        // Frequência angular normalizada
    float cw = cosf(w0);
    float alpha = sinf(w0) / (2.0f * q);
    float a0 = 1.0f + alpha;                        // Normalizador
    bq->b0 = ((1.0f + cw) / 2.0f) / a0;
    bq->b1 = (-(1.0f + cw)) / a0;
    bq->b2 = ((1.0f + cw) / 2.0f) / a0;
    bq->a1 = (-2.0f * cw) / a0;
    bq->a2 = (1.0f - alpha) / a0;
}

/* Coeficientes de um passa-baixa de 2ª ordem (Audio EQ Cookbook). */
static void biquad_lowpass(Biquad *bq, float fs, float fc, float q) {
    float w0 = 2.0f * (float)M_PI * fc / fs;
    float cw = cosf(w0);
    float alpha = sinf(w0) / (2.0f * q);
    float a0 = 1.0f + alpha;
    bq->b0 = ((1.0f - cw) / 2.0f) / a0;
    bq->b1 = (1.0f - cw) / a0;
    bq->b2 = ((1.0f - cw) / 2.0f) / a0;
    bq->a1 = (-2.0f * cw) / a0;
    bq->a2 = (1.0f - alpha) / a0;
}

static void biquad_reset(Biquad *bq) {
    bq->x1 = bq->x2 = bq->y1 = bq->y2 = 0.0f;
}

/* y[n] = b0*x[n] + b1*x[n-1] + b2*x[n-2] - a1*y[n-1] - a2*y[n-2] */
static void biquad_process(Biquad *bq, float *s, size_t n) {
    float x1 = bq->x1, x2 = bq->x2, y1 = bq->y1, y2 = bq->y2;
    for (size_t i = 0; i < n; i++) {
        float x = s[i];
        float y = bq->b0 * x + bq->b1 * x1 + bq->b2 * x2 - bq->a1 * y1 - bq->a2 * y2;
        x2 = x1; x1 = x;                            // Desloca os atrasos de entrada
        y2 = y1; y1 = y;                            // Desloca os atrasos de saída
        s[i] = y;
    }
    bq->x1 = x1; bq->x2 = x2; bq->y1 = y1; bq->y2 = y2;
}

void bandpass_init(BandpassFilter *f, float fs, float low_hz, float high_hz) {
    const float q = 0.70710678f;                    // Butterworth (resposta plana)
    biquad_highpass(&f->highpass, fs, low_hz, q);   // Remove DC, ruído de manuseio e zumbido
    biquad_lowpass(&f->lowpass, fs, high_hz, q);    // Remove chiado e ruído de alta frequência
    bandpass_reset(f);
}

void bandpass_reset(BandpassFilter *f) {
    biquad_reset(&f->highpass);
    biquad_reset(&f->lowpass);
}

void bandpass_process(BandpassFilter *f, float *samples, size_t n) {
    biquad_process(&f->highpass, samples, n);
    biquad_process(&f->lowpass, samples, n);
}

float dsp_rms_db(const float *samples, size_t n) {
    if (n == 0) return -120.0f;
    double acc = 0.0;
    for (size_t i = 0; i < n; i++) acc += (double)samples[i] * samples[i];
    double rms = sqrt(acc / (double)n);
    // Referência: senoide de amplitude 1 tem RMS 1/sqrt(2) = 0 dBFS
    double db = 20.0 * log10(rms * 1.41421356 + 1e-9);
    return (float)(db < -120.0 ? -120.0 : db);
}

// ======================= YIN ================================================
/*
 * Implementação do YIN (de Cheveigné & Kawahara, 2002):
 *  1. Função diferença d(tau)
 *  2. Diferença média normalizada cumulativa d'(tau)
 *  3. Primeiro mínimo abaixo do limiar absoluto
 *  4. Interpolação parabólica para precisão sub-amostra
 */
void yin_init(YinDetector *y, float fs, size_t window, float fmin, float fmax,
              float threshold, float *diff_buffer) {
    y->sample_rate = fs;
    y->window = window;
    y->tau_min = (size_t)(fs / fmax);
    if (y->tau_min < 2) y->tau_min = 2;
    y->tau_max = (size_t)(fs / fmin) + 1;
    if (y->tau_max > window / 2) y->tau_max = window / 2;   // Garante janela de integração
    y->threshold = threshold;
    y->diff = diff_buffer;
}

/* Passos 1 e 2: d'(tau) em y->diff[0..tau_max]. */
static void yin_cmndf(YinDetector *y, const float *x) {
    const size_t tau_max = y->tau_max;
    const size_t w = y->window - tau_max;           // Janela de integração
    float *d = y->diff;

    d[0] = 0.0f;
    for (size_t tau = 1; tau <= tau_max; tau++) {
        float sum = 0.0f;
        const float *a = x;
        const float *b = x + tau;
        for (size_t j = 0; j < w; j++) {
            float delta = a[j] - b[j];
            sum += delta * delta;
        }
        d[tau] = sum;
    }

    d[0] = 1.0f;
    float running = 0.0f;
    for (size_t tau = 1; tau <= tau_max; tau++) {
        running += d[tau];
        d[tau] = (running > 0.0f) ? d[tau] * (float)tau / running : 1.0f;
    }
}

static float yin_valley_value(const YinDetector *y, size_t tau);

/*
 * Passo 3: primeiro vale abaixo do limiar a partir de `from` (0 = nenhum).
 * O limiar é testado no fundo interpolado do vale: em períodos curtos (C8
 * tem ~3,8 amostras) o atraso inteiro fica longe do vale e o YIN pularia
 * para dois períodos (uma oitava abaixo). Um "vale" que ainda desce no
 * último atraso da faixa não é vale (o período está fora da faixa).
 */
static size_t yin_first_dip(const YinDetector *y, size_t from) {
    const float *d = y->diff;
    if (from < 1) from = 1;
    for (size_t tau = from; tau < y->tau_max; tau++) {
        if (d[tau] > d[tau - 1] || d[tau] > d[tau + 1]) continue;   // Não é mínimo local
        if (d[tau] < y->threshold || yin_valley_value(y, tau) < y->threshold) return tau;
    }
    return 0;
}

/* Passo 4: posição sub-amostra do vale em `tau`. */
static float yin_refine(const YinDetector *y, size_t tau) {
    const float *d = y->diff;
    float tau_f = (float)tau;
    if (tau > 1 && tau < y->tau_max) {
        float s0 = d[tau - 1], s1 = d[tau], s2 = d[tau + 1];
        float denom = 2.0f * (2.0f * s1 - s2 - s0);
        if (fabsf(denom) > 1e-9f) tau_f += (s2 - s0) / denom;
    }
    return tau_f;
}

/* d' no fundo do vale em `tau` (vértice da parábola por 3 pontos). Em
 * períodos curtos (notas agudas: C8 tem ~3,8 amostras) o atraso inteiro
 * fica longe do vale verdadeiro e superestima d'. */
static float yin_valley_value(const YinDetector *y, size_t tau) {
    const float *d = y->diff;
    float v = d[tau];
    if (tau > 1 && tau < y->tau_max) {
        float a = 0.5f * (d[tau - 1] + d[tau + 1]) - d[tau];
        float b = 0.5f * (d[tau + 1] - d[tau - 1]);
        if (a > 1e-9f) v = d[tau] - b * b / (4.0f * a);
    }
    return v < 0.0f ? 0.0f : v;
}

/* Menor d' em torno de `tau` (± raio), dentro da faixa. Retorna o atraso. */
static size_t yin_min_near(const YinDetector *y, float tau, float radius) {
    long lo = (long)floorf(tau - radius), hi = (long)ceilf(tau + radius);
    if (lo < (long)y->tau_min) lo = (long)y->tau_min;
    if (hi > (long)y->tau_max) hi = (long)y->tau_max;
    if (lo > hi) return 0;
    size_t best = (size_t)lo;
    for (long t = lo; t <= hi; t++) {
        if (y->diff[t] < y->diff[best]) best = (size_t)t;
    }
    return best;
}

static float yin_min_value(const YinDetector *y) {
    float m = 1.0f;
    for (size_t tau = y->tau_min; tau <= y->tau_max; tau++) {
        if (y->diff[tau] < m) m = y->diff[tau];
    }
    return m;
}

float yin_detect(YinDetector *y, const float *x, float *confidence) {
    yin_cmndf(y, x);
    size_t best = yin_first_dip(y, y->tau_min);
    if (best == 0) {
        // Nenhum vale abaixo do limiar: sem altura definida (ruído/silêncio)
        if (confidence) *confidence = 1.0f - fminf(1.0f, yin_min_value(y));
        return 0.0f;
    }
    if (confidence) *confidence = 1.0f - fminf(1.0f, y->diff[best]);
    return y->sample_rate / yin_refine(y, best);
}

// ======================= DETECTOR DE DUAS RESOLUÇÕES ========================
/*
 * Cobre o piano inteiro (A0 a C8) com custo menor que um YIN único:
 *  - faixa aguda: YIN na taxa cheia (16 kHz), janela curta (32 ms), períodos
 *    de 3 a 128 amostras (~125 Hz a 5,3 kHz);
 *  - faixa grave: o mesmo sinal filtrado (passa-baixa) e decimado por 4
 *    (4 kHz), janela longa (80 ms), períodos até ~150 amostras (~26,7 Hz).
 *
 * Correção de oitava: o YIN escolhe o primeiro vale abaixo do limiar, que
 * pode ser o período de um harmônico forte (ex.: E4 lido como E6 quando o
 * 4º harmônico domina o som captado). Um sinal com período T também se
 * repete em 2T, 3T...; por isso, depois do candidato, são verificados os
 * múltiplos: se algum for claramente mais periódico (d' bem menor), o
 * candidato era um harmônico e o período verdadeiro é o múltiplo.
 */
#define MULTIPLE_RATIO      0.5f        // Múltiplo precisa ter d' < 50% do atual
#define MULTIPLE_MARGIN     0.005f      // ...e ser menor em pelo menos 0,005 (sinais
                                        // muito limpos têm d' ~0 em todos os múltiplos;
                                        // quem os separa é subharmonics_present)
#define MULTIPLE_TOLERANCE  0.03f       // ±3% (inarmonicidade das cordas)
#define MAX_STEP_MULTIPLE   8           // Harmônico mais alto corrigido por passo
#define SUBHARMONIC_MIN_RATIO 0.003f    // Componentes do período maior >= -25 dB
#define SUBHARMONIC_PEAK_RATIO 3.0f     // ...e 3x acima do ruído entre elas

void pitch_init(PitchDetector *p, float fs, float fmin, float fmax, float split_hz,
                float threshold) {
    memset(p, 0, sizeof(*p));
    p->fs = fs;
    p->decimation = PITCH_LOW_DECIMATION;
    // Faixa aguda: de split_hz até fmax
    yin_init(&p->high, fs, PITCH_HIGH_WINDOW, split_hz, fmax, threshold, p->high_diff);
    // Faixa grave: de fmin até ~5x split_hz (sobreposição para a correção de oitava)
    const float fs_low = fs / (float)p->decimation;
    yin_init(&p->low, fs_low, PITCH_LOW_WINDOW, fmin, fs_low / 6.0f, threshold, p->low_diff);
    p->low_split_tau = (size_t)(fs_low / split_hz);
    // Antisserrilhado da decimação: 4ª ordem em ~0,45 x Nyquist da taxa reduzida
    biquad_lowpass(&p->aa[0], fs, fs_low * 0.225f, 0.5412f);
    biquad_lowpass(&p->aa[1], fs, fs_low * 0.225f, 1.3066f);
    for (size_t i = 0; i < PITCH_HIGH_WINDOW; i++) {
        p->high_hann[i] = 0.5f - 0.5f * cosf(2.0f * (float)M_PI * (float)i / (PITCH_HIGH_WINDOW - 1));
    }
    for (size_t i = 0; i < PITCH_LOW_WINDOW; i++) {
        p->low_hann[i] = 0.5f - 0.5f * cosf(2.0f * (float)M_PI * (float)i / (PITCH_LOW_WINDOW - 1));
    }
}

void pitch_reset(PitchDetector *p) {
    memset(p->high_window, 0, sizeof(p->high_window));
    memset(p->low_window, 0, sizeof(p->low_window));
    biquad_reset(&p->aa[0]);
    biquad_reset(&p->aa[1]);
    p->phase = 0;
}

/* Atualiza as duas janelas com o trecho mais recente (já filtrado). */
static void pitch_push(PitchDetector *p, const float *hop, size_t n) {
    if (n > PITCH_HIGH_WINDOW) {
        hop += n - PITCH_HIGH_WINDOW;
        n = PITCH_HIGH_WINDOW;
    }
    memmove(p->high_window, p->high_window + n, (PITCH_HIGH_WINDOW - n) * sizeof(float));
    memcpy(p->high_window + PITCH_HIGH_WINDOW - n, hop, n * sizeof(float));

    float low[PITCH_HIGH_WINDOW / PITCH_LOW_DECIMATION + 1];
    size_t m = 0;
    for (size_t i = 0; i < n; i++) {
        float s = hop[i];
        biquad_process(&p->aa[0], &s, 1);
        biquad_process(&p->aa[1], &s, 1);
        if (++p->phase >= p->decimation) {
            p->phase = 0;
            low[m++] = s;
        }
    }
    if (m > 0) {
        memmove(p->low_window, p->low_window + m, (PITCH_LOW_WINDOW - m) * sizeof(float));
        memcpy(p->low_window + PITCH_LOW_WINDOW - m, low, m * sizeof(float));
    }
}

/* d' interpolado no vale mais próximo de `t` (atraso fracionário) na faixa. */
static bool band_value(const YinDetector *y, float t, float *value, size_t *tau) {
    if (t < (float)y->tau_min || t + 1.0f > (float)y->tau_max) return false;
    size_t at = yin_min_near(y, t, 1.0f + t * MULTIPLE_TOLERANCE);
    if (at == 0) return false;
    *value = yin_valley_value(y, at);
    *tau = at;
    return true;
}

/* Potência média (A²/2) da componente em `freq` (Goertzel com janela de
 * Hann, `hann` com n pontos). */
static float tone_power(const float *x, const float *hann, size_t n, float freq, float fs) {
    const float w = 2.0f * (float)M_PI * freq / fs;
    const float coeff = 2.0f * cosf(w);
    float s1 = 0.0f, s2 = 0.0f;
    for (size_t i = 0; i < n; i++) {
        float s0 = x[i] * hann[i] + coeff * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    float mag2 = s1 * s1 + s2 * s2 - coeff * s1 * s2;
    // Ganho coerente da janela de Hann = n/2: amplitude A -> |X| = A n / 4
    return mag2 * 8.0f / ((float)n * (float)n);
}

static float window_power(const float *x, const float *hann, size_t n) {
    float acc = 0.0f, norm = 0.0f;
    for (size_t i = 0; i < n; i++) {
        float v = x[i] * hann[i];
        acc += v * v;
        norm += hann[i] * hann[i];
    }
    return norm > 0.0f ? acc / norm : 0.0f;
}

/*
 * Confirma que o período verdadeiro é `k` vezes o candidato: um sinal de
 * período P = k*T tem componentes em m/P com m coprimo de k (é isso que o
 * torna diferente de um sinal de período T ou de qualquer divisor de k*T),
 * abaixo ou acima do harmônico candidato. A inarmonicidade das
 * cordas não cria energia nessas frequências, então não engana o teste.
 */
static bool subharmonics_present(const PitchDetector *p, float period_s, int k) {
    const float f0 = 1.0f / period_s;
    // Notas graves: janela longa da faixa grave (resolução de 12,5 Hz, até
    // ~900 Hz). As demais: janela da faixa aguda (31 Hz, até 7,2 kHz), que
    // alcança as componentes acima do harmônico (5º, 7º... quando k = 4).
    const bool use_low = f0 < 2.5f * p->high.sample_rate / (float)PITCH_HIGH_WINDOW;
    const float *x = use_low ? p->low_window : p->high_window;
    const float *hann = use_low ? p->low_hann : p->high_hann;
    const size_t n = use_low ? PITCH_LOW_WINDOW : PITCH_HIGH_WINDOW;
    const float fs = use_low ? p->low.sample_rate : p->high.sample_rate;
    const float f_max = use_low ? fs * 0.225f : fs * 0.45f;
    const float total = window_power(x, hann, n);
    if (total <= 0.0f) return false;
    // A referência de ruído só é confiável quando meio espaçamento entre as
    // componentes cabe em 2,5 bins (fora do lóbulo principal da janela)
    const bool check_peaks = 0.5f * f0 >= 2.5f * fs / (float)n;
    float sub = 0.0f, ref = 0.0f;
    int used = 0;
    // m coprimo de k, abaixo E acima do harmônico candidato: um alto-falante
    // pequeno corta a fundamental e os primeiros harmônicos (ex.: E4 com só o
    // 4º harmônico forte), mas o 5º, o 7º... ainda revelam o período k*T
    for (int m = 1; m <= 4 * k && used < 8; m++) {
        int a = m, b = k;
        while (b != 0) { int r = a % b; a = b; b = r; }
        if (a != 1) continue;
        const float f = f0 * (float)m;
        if (f >= f_max) break;
        used++;
        sub += tone_power(x, hann, n, f, fs);
        // Referência de ruído: entre as componentes (o ruído é plano; as
        // componentes de uma nota são picos)
        ref += tone_power(x, hann, n, f + 0.5f * f0, fs);
    }
    return used > 0 && sub >= total * SUBHARMONIC_MIN_RATIO &&
           (!check_peaks || sub >= ref * SUBHARMONIC_PEAK_RATIO);
}

void pitch_harmonic_levels(const PitchDetector *p, float freq, float out_db[PITCH_PROBE_COUNT]) {
    static const float ratios[PITCH_PROBE_COUNT] = {0.25f, 1.0f / 3.0f, 0.5f, 1.0f, 2.0f};
    const float total = window_power(p->high_window, p->high_hann, PITCH_HIGH_WINDOW);
    for (int i = 0; i < PITCH_PROBE_COUNT; i++) {
        float f = freq * ratios[i];
        float power = 0.0f;
        if (freq > 0.0f && total > 0.0f && f < p->high.sample_rate * 0.45f) {
            power = tone_power(p->high_window, p->high_hann, PITCH_HIGH_WINDOW, f,
                               p->high.sample_rate) / total;
        }
        out_db[i] = power > 1e-12f ? 10.0f * log10f(power) : -120.0f;
    }
}

float pitch_process(PitchDetector *p, const float *hop, size_t n, float *confidence) {
    pitch_push(p, hop, n);
    yin_cmndf(&p->high, p->high_window);
    yin_cmndf(&p->low, p->low_window);

    // 1) Candidato: primeiro vale da faixa aguda; sem ele, o da faixa grave
    const YinDetector *band = &p->high;
    size_t tau = yin_first_dip(&p->high, p->high.tau_min);
    if (tau == 0) {
        // Sem altura nos agudos: a faixa grave só decide para os graves de
        // fato (abaixo de split_hz). O primeiro vale é procurado desde o
        // início da faixa: começar em split_hz encontraria um múltiplo do
        // período de uma nota mais aguda (ex.: o fim de um G4 lido como G2).
        band = &p->low;
        tau = yin_first_dip(&p->low, p->low.tau_min);
        if (tau != 0 && tau < p->low_split_tau) tau = 0;
    }
    if (tau == 0) {
        float m = fminf(yin_min_value(&p->high), yin_min_value(&p->low));
        if (confidence) *confidence = 1.0f - fminf(1.0f, m);
        return 0.0f;
    }

    // 2) Correção de oitava, um passo por vez: j = 2..8 vezes o melhor período
    //    atual substitui o candidato se for claramente mais periódico E se o
    //    sinal tiver as componentes que só existem com o período maior
    //    (subharmonics_present). Notas muito graves são alcançadas por passos
    //    encadeados (ex.: 4x e depois 2x).
    float best_period = yin_refine(band, tau) / band->sample_rate;
    float best_value = yin_valley_value(band, tau);
    bool improved = true;
    while (improved) {
        improved = false;
        for (int j = 2; j <= MAX_STEP_MULTIPLE; j++) {
            const float period_j = best_period * (float)j;
            if (period_j * p->low.sample_rate + 1.0f > (float)p->low.tau_max) break;  // < A0
            float value;
            size_t t;
            const YinDetector *b = &p->high;
            if (!band_value(&p->high, period_j * p->high.sample_rate, &value, &t)) {
                b = &p->low;
                if (!band_value(&p->low, period_j * p->low.sample_rate, &value, &t)) continue;
            }
            if (value < best_value * MULTIPLE_RATIO && best_value - value > MULTIPLE_MARGIN &&
                value < band->threshold && subharmonics_present(p, period_j, j)) {
                best_value = value;
                band = b;
                tau = t;
                best_period = yin_refine(b, t) / b->sample_rate;
                improved = true;
                break;
            }
        }
    }

    // 3) Frequência com a melhor resolução disponível: se o período cabe na
    //    faixa aguda, refina nela
    float period = yin_refine(band, tau) / band->sample_rate;
    if (band == &p->high && tau < 24) {
        // Período curto (notas agudas): a interpolação parabólica erra até
        // ~50 cents com 3-5 amostras por período. O vale de m períodos
        // (~32 amostras) tem a mesma posição relativa com m vezes mais
        // resolução.
        const float t0 = yin_refine(band, tau);
        const int m = (int)(32.0f / t0);
        if (m >= 2 && t0 * (float)m + 1.0f <= (float)band->tau_max) {
            size_t at = yin_min_near(band, t0 * (float)m, 1.5f);
            if (at != 0) period = yin_refine(band, at) / (float)m / band->sample_rate;
        }
    }
    if (band == &p->low) {
        float t = period * p->high.sample_rate;
        if (t + 1.0f <= (float)p->high.tau_max && t >= (float)p->high.tau_min) {
            size_t at = yin_min_near(&p->high, t, 1.5f);
            if (at != 0) period = yin_refine(&p->high, at) / p->high.sample_rate;
        }
    }
    if (confidence) *confidence = 1.0f - fminf(1.0f, band->diff[tau]);
    return period > 0.0f ? 1.0f / period : 0.0f;
}

float dsp_hz_to_midi(float hz) {
    if (hz <= 0.0f) return 0.0f;
    return 69.0f + 12.0f * log2f(hz / 440.0f);
}

float dsp_midi_to_hz(float midi) {
    return 440.0f * powf(2.0f, (midi - 69.0f) / 12.0f);
}
