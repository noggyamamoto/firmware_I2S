/*
 * ============================================================================
 * Processamento digital de sinais (independente de hardware)
 *
 *  - Filtro passa-banda (cascata de biquads passa-alta + passa-baixa)
 *  - Estimativa de frequência fundamental pelo algoritmo YIN
 *  - Detector de altura de duas resoluções (A0 a C8) com correção de oitava
 *  - Conversões auxiliares (Hz <-> MIDI, RMS em dBFS)
 *
 * Este módulo não depende do ESP-IDF e é compilado também no computador
 * pelos testes em test/host.
 * ============================================================================
 */
#ifndef DSP_H
#define DSP_H

#include <stdbool.h>
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

/* Detector de altura do piano inteiro (A0 a C8): YIN em duas resoluções
 * (taxa cheia para os agudos, taxa/4 para os graves) com correção de oitava
 * (harmônico forte não é confundido com a nota). Ver dsp.c. */
#define PITCH_HIGH_WINDOW       512     // 32 ms a 16 kHz (períodos até 128 amostras)
#define PITCH_LOW_DECIMATION    4       // Faixa grave a 4 kHz
#define PITCH_LOW_WINDOW        320     // 80 ms a 4 kHz (períodos até ~150 amostras)

typedef struct {
    float       fs;
    int         decimation;
    int         phase;                                  // Posição na decimação
    size_t      low_split_tau;                          // Período (faixa grave) de split_hz
    Biquad      aa[2];                                  // Antisserrilhado (4ª ordem)
    YinDetector high;                                   // Faixa aguda (taxa cheia)
    YinDetector low;                                    // Faixa grave (decimada)
    float       high_window[PITCH_HIGH_WINDOW];
    float       low_window[PITCH_LOW_WINDOW];
    float       high_diff[PITCH_HIGH_WINDOW / 2 + 1];
    float       low_diff[PITCH_LOW_WINDOW / 2 + 1];
    float       high_hann[PITCH_HIGH_WINDOW];           // Janelas de Hann (Goertzel)
    float       low_hann[PITCH_LOW_WINDOW];
} PitchDetector;

/**
 * @brief Configura o detector.
 * @param fmin/fmax  Faixa de frequências (ex.: A0 - 50 cents a C8 + margem).
 * @param split_hz   Menor frequência analisada na taxa cheia (~125 Hz).
 */
void pitch_init(PitchDetector *p, float fs, float fmin, float fmax, float split_hz,
                float threshold);

/** @brief Zera as janelas (ex.: após falha de leitura). */
void pitch_reset(PitchDetector *p);

/**
 * @brief Acrescenta um trecho filtrado e estima a altura das janelas atuais.
 * @param confidence Saída: 1 - aperiodicidade no período escolhido (0..1).
 * @return Frequência em Hz ou 0 se não houver altura definida.
 */
float pitch_process(PitchDetector *p, const float *hop, size_t n, float *confidence);

/* Diagnóstico (afinador): nível relativo (dB) do sinal em f/4, f/3, f/2, f
 * e 2f. Se a nota sai uma ou duas oitavas acima, mostra se havia energia
 * na nota verdadeira (f/2, f/4) que o detector não aproveitou. */
#define PITCH_PROBE_COUNT 5
void pitch_harmonic_levels(const PitchDetector *p, float freq, float out_db[PITCH_PROBE_COUNT]);

/** @brief Converte frequência para número MIDI (fracionário). */
float dsp_hz_to_midi(float hz);

/** @brief Converte número MIDI para frequência (Hz). */
float dsp_midi_to_hz(float midi);

#endif // DSP_H
