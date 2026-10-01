/*
 * ============================================================================
 * Implementação do processamento digital de sinais
 * ============================================================================
 */
#include "dsp.h"

#include <math.h>
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

float yin_detect(YinDetector *y, const float *x, float *confidence) {
    const size_t tau_max = y->tau_max;
    const size_t w = y->window - tau_max;           // Janela de integração
    float *d = y->diff;

    // 1) Função diferença
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

    // 2) Normalização cumulativa
    d[0] = 1.0f;
    float running = 0.0f;
    for (size_t tau = 1; tau <= tau_max; tau++) {
        running += d[tau];
        d[tau] = (running > 0.0f) ? d[tau] * (float)tau / running : 1.0f;
    }

    // 3) Primeiro mínimo local abaixo do limiar
    size_t best = 0;
    for (size_t tau = y->tau_min; tau <= tau_max; tau++) {
        if (d[tau] < y->threshold) {
            while (tau + 1 <= tau_max && d[tau + 1] < d[tau]) tau++;   // Desce até o vale
            best = tau;
            break;
        }
    }

    if (best == 0) {
        // Nenhum vale abaixo do limiar: sem altura definida (ruído/silêncio)
        size_t argmin = y->tau_min;
        for (size_t tau = y->tau_min; tau <= tau_max; tau++) {
            if (d[tau] < d[argmin]) argmin = tau;
        }
        if (confidence) *confidence = 1.0f - fminf(1.0f, d[argmin]);
        return 0.0f;
    }

    // 4) Interpolação parabólica em torno do mínimo
    float tau_f = (float)best;
    if (best > 1 && best < tau_max) {
        float s0 = d[best - 1], s1 = d[best], s2 = d[best + 1];
        float denom = 2.0f * (2.0f * s1 - s2 - s0);
        if (fabsf(denom) > 1e-9f) tau_f += (s2 - s0) / denom;
    }

    if (confidence) *confidence = 1.0f - fminf(1.0f, d[best]);
    return y->sample_rate / tau_f;
}

float dsp_hz_to_midi(float hz) {
    if (hz <= 0.0f) return 0.0f;
    return 69.0f + 12.0f * log2f(hz / 440.0f);
}

float dsp_midi_to_hz(float midi) {
    return 440.0f * powf(2.0f, (midi - 69.0f) / 12.0f);
}
