/*
 * ============================================================================
 * Processamento digital de sinais (independente de hardware)
 *
 *  - Filtro passa-banda (cascata de biquads passa-alta + passa-baixa)
 *  - Estimativa de frequência fundamental pelo algoritmo YIN
 *  - Conversões auxiliares (Hz <-> MIDI, RMS em dBFS)
 *
 * Este módulo não depende do ESP-IDF e é compilado também no computador
 * pelos testes em test/host.
 * ============================================================================
 */
#ifndef DSP_H
#define DSP_H

#include <stddef.h>
#include <stdint.h>

/* Biquad na forma direta I (coeficientes normalizados por a0). */
typedef struct {
    float b0, b1, b2;       // Coeficientes do numerador
    float a1, a2;           // Coeficientes do denominador (a0 = 1)
    float x1, x2;           // x[n-1], x[n-2]
    float y1, y2;           // y[n-1], y[n-2]
} Biquad;

/* Passa-banda formado por passa-alta + passa-baixa de 2ª ordem (Butterworth). */
typedef struct {
    Biquad highpass;
    Biquad lowpass;
} BandpassFilter;

/**
 * @brief Calcula os coeficientes reais do passa-banda (receitas RBJ, Q = 0,707).
 * @param f        Estrutura do filtro.
 * @param fs       Taxa de amostragem (Hz).
 * @param low_hz   Corte inferior (Hz).
 * @param high_hz  Corte superior (Hz).
 */
void bandpass_init(BandpassFilter *f, float fs, float low_hz, float high_hz);

/** @brief Zera o estado interno (atrasos) do filtro. */
void bandpass_reset(BandpassFilter *f);

/** @brief Filtra um bloco in-place. */
void bandpass_process(BandpassFilter *f, float *samples, size_t n);

/** @brief Nível RMS do bloco em dBFS (0 dBFS = senoide de amplitude 1). */
float dsp_rms_db(const float *samples, size_t n);

/* Estado/buffers do detector YIN (alocados pelo chamador). */
typedef struct {
    float  sample_rate;     // Hz
    size_t window;          // Tamanho da janela analisada
    size_t tau_min;         // Menor atraso (maior frequência)
    size_t tau_max;         // Maior atraso (menor frequência)
    float  threshold;       // Limiar absoluto do YIN (tipicamente 0,10 a 0,20)
    float *diff;            // Buffer de tau_max + 1 floats
} YinDetector;

/**
 * @brief Configura o detector YIN.
 * @param diff_buffer Buffer com pelo menos (window / 2 + 1) floats.
 */
void yin_init(YinDetector *y, float fs, size_t window, float fmin, float fmax,
              float threshold, float *diff_buffer);

/**
 * @brief Estima a frequência fundamental de uma janela.
 * @param confidence Saída: 1 - aperiodicidade no atraso escolhido (0..1).
 * @return Frequência em Hz ou 0 se o trecho não tiver altura definida.
 */
float yin_detect(YinDetector *y, const float *window, float *confidence);

/** @brief Converte frequência para número MIDI (fracionário). */
float dsp_hz_to_midi(float hz);

/** @brief Converte número MIDI para frequência (Hz). */
float dsp_midi_to_hz(float midi);

#endif // DSP_H
