/*
 * ============================================================================
 * Teste do pipeline de DSP no computador (sem ESP32)
 *
 * Sintetiza uma execução de teclado (harmônicos com decaimento), adiciona
 * ruído de sala e cliques de metrônomo e verifica se o filtro + YIN +
 * segmentação detectam as notas esperadas com a altura e o tempo corretos.
 *
 * Compilar e executar:  make -C test/host
 * ============================================================================
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dsp.h"
#include "note_tracker.h"

#define FS       16000
#define HOP      256

typedef struct { int midi; double start; double dur; } Expected;

static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("  FALHA: " __VA_ARGS__); printf("\n"); } } while (0)

/* Ruído branco gaussiano simples (Box-Muller). */
static double noise(void) {
    double u1 = (rand() + 1.0) / (RAND_MAX + 2.0);
    double u2 = (rand() + 1.0) / (RAND_MAX + 2.0);
    return sqrt(-2.0 * log(u1)) * cos(2 * M_PI * u2);
}

/* Amplitude de cada harmônico (1..8): padrão = teclado (1/k). */
static const double KEYBOARD[8] = {1.0, 1.0 / 2, 1.0 / 3, 1.0 / 4, 1.0 / 5, 1.0 / 6, 0, 0};
static const double *s_harmonics = KEYBOARD;

/* Síntese de um som de teclado: harmônicos, ataque de 5 ms, decaimento e soltura. */
static void render_note(float *buf, size_t len, int midi, double start, double dur, double amp) {
    double f0 = 440.0 * pow(2.0, (midi - 69) / 12.0);
    size_t s0 = (size_t)(start * FS);
    size_t n_note = (size_t)(dur * FS);
    size_t n_total = n_note + (size_t)(0.04 * FS);          // 40 ms de soltura
    for (size_t i = 0; i < n_total && s0 + i < len; i++) {
        double t = (double)i / FS;
        double env = fmin(1.0, t / 0.005) * exp(-t / 0.9);  // Ataque + decaimento natural
        if (i >= n_note) env *= exp(-((double)(i - n_note) / FS) / 0.008);  // Soltura rápida
        double s = 0;
        for (int k = 1; k <= 8; k++) {
            if (f0 * k > FS / 2 - 500) break;
            s += s_harmonics[k - 1] * sin(2 * M_PI * f0 * k * t);
        }
        buf[s0 + i] += (float)(amp * env * s);
    }
}

static void render_click(float *buf, size_t len, double start) {
    size_t s0 = (size_t)(start * FS);
    for (size_t i = 0; i < (size_t)(0.03 * FS) && s0 + i < len; i++) {
        double t = (double)i / FS;
        buf[s0 + i] += (float)(0.2 * exp(-t / 0.008) * sin(2 * M_PI * 1760 * t));
    }
}

static int run_case_ex(const char *name, const Expected *played, int n_played,
                       const Expected *exp_notes, int n_exp, double total_s, double noise_db,
                       double amp, const double *clicks, int n_clicks, int check_dur) {
    printf("Caso: %s\n", name);
    size_t len = (size_t)(total_s * FS);
    float *sig = calloc(len, sizeof(float));
    for (int i = 0; i < n_played; i++)
        render_note(sig, len, played[i].midi, played[i].start, played[i].dur, amp);
    for (int i = 0; i < n_clicks; i++) render_click(sig, len, clicks[i]);
    double noise_amp = pow(10.0, noise_db / 20.0);
    for (size_t i = 0; i < len; i++) sig[i] += (float)(noise_amp * noise());

    BandpassFilter filt;
    bandpass_init(&filt, FS, 20.0f, 5000.0f);
    static PitchDetector pitch;
    pitch_init(&pitch, FS, 26.7f, 4400.0f, 125.0f, 0.15f);
    pitch_reset(&pitch);
    NoteTrackerConfig cfg;
    note_tracker_default_config(&cfg);
    NoteTracker tr;
    note_tracker_init(&tr, &cfg);

    NoteEvent got[64];
    int n_got = 0;
    int ci = 0;
    for (size_t pos = 0; pos + HOP <= len; pos += HOP) {
        float hop[HOP];
        memcpy(hop, sig + pos, sizeof(hop));
        bandpass_process(&filt, hop, HOP);
        uint32_t t_ms = (uint32_t)(pos * 1000 / FS);
        while (ci < n_clicks && clicks[ci] * 1000 <= t_ms + 16) {
            uint32_t c = (uint32_t)(clicks[ci] * 1000);
            note_tracker_mask(&tr, c > 16 ? c - 16 : 0, c + 45, 1760.0f);
            ci++;
        }
        float conf = 0;
        float f = pitch_process(&pitch, hop, HOP, &conf);
        NoteEvent ev[2];
        int n = note_tracker_process(&tr, t_ms, dsp_rms_db(hop, HOP), f, conf, ev);
        for (int k = 0; k < n && n_got < 64; k++) got[n_got++] = ev[k];
    }
    NoteEvent last;
    if (note_tracker_flush(&tr, (uint32_t)(total_s * 1000), &last)) got[n_got++] = last;

    int ons = 0;
    for (int i = 0; i < n_got; i++) {
        NoteEvent *e = &got[i];
        if (e->type == NOTE_EVENT_ON) {
            printf("  ON  %3d t=%5u ms f=%7.2f Hz conf=%.2f nivel=%.1f dB\n", e->midi, e->time_ms,
                   e->frequency_hz, e->confidence, e->level_db);
            ons++;
        } else {
            printf("  OFF %3d t=%5u ms dur=%u ms\n", e->midi, e->time_ms, e->duration_ms);
        }
    }
    printf("  piso de ruido estimado: %.1f dBFS, limiar: %.1f dBFS\n", tr.noise_floor_db,
           note_tracker_gate_db(&tr));

    CHECK(ons == n_exp, "esperadas %d notas, detectadas %d", n_exp, ons);
    int idx = 0;
    for (int i = 0; i < n_got && idx < n_exp; i++) {
        if (got[i].type != NOTE_EVENT_ON) continue;
        const Expected *x = &exp_notes[idx++];
        double dt = (double)got[i].time_ms - x->start * 1000.0;
        CHECK(got[i].midi == x->midi, "nota %d: esperado MIDI %d, obtido %d", idx, x->midi, got[i].midi);
        CHECK(fabs(dt) <= 30.0, "nota %d: erro de ataque %.0f ms", idx, dt);
        // Procura o NOTE_OFF correspondente
        for (int j = i + 1; check_dur && j < n_got; j++) {
            if (got[j].type == NOTE_EVENT_OFF) {
                double dd = (double)got[j].duration_ms - x->dur * 1000.0;
                CHECK(fabs(dd) <= 60.0, "nota %d: erro de duracao %.0f ms", idx, dd);
                break;
            }
        }
    }
    free(sig);
    return 0;
}

static int run_case(const char *name, const Expected *exp_notes, int n_exp, double total_s,
                    double noise_db, double amp, const double *clicks, int n_clicks, int check_dur) {
    return run_case_ex(name, exp_notes, n_exp, exp_notes, n_exp, total_s, noise_db, amp, clicks,
                       n_clicks, check_dur);
}

int main(void) {
    srand(42);

    // Melodia simples com nota repetida (Mi-Mi) e saltos, do grave ao agudo
    const Expected melody[] = {
        {60, 0.30, 0.45}, {62, 0.90, 0.45}, {64, 1.50, 0.40}, {64, 1.95, 0.40},
        {67, 2.50, 0.90}, {72, 3.60, 0.30}, {57, 4.10, 0.60}, {48, 5.00, 0.80},
    };
    run_case("melodia em sala silenciosa", melody, 8, 6.2, -80.0, 0.05, NULL, 0, 1);

    // Mesma melodia com ruído de sala (≈ 20 pessoas conversando baixo)
    run_case("melodia com ruido de sala", melody, 8, 6.2, -55.0, 0.05, NULL, 0, 1);

    // Notas rápidas (colcheias a 120 BPM) com cliques de metrônomo nas semínimas
    const Expected fast[] = {
        {67, 0.50, 0.22}, {69, 0.75, 0.22}, {71, 1.00, 0.22}, {72, 1.25, 0.22},
        {74, 1.50, 0.22}, {72, 1.75, 0.22}, {71, 2.00, 0.45},
    };
    const double clicks[] = {0.0, 0.5, 1.0, 1.5, 2.0, 2.5};
    run_case("colcheias com cliques do metronomo", fast, 7, 3.0, -75.0, 0.05, clicks, 6, 1);

    // Teclado em volume baixo com ruído ambiente forte (relação sinal-ruído ≈ 16 dB).
    // Notas longas decaem abaixo do ruído antes da soltura, então somente a
    // altura e o ataque são verificados neste caso.
    run_case("volume baixo com ruido forte", melody, 8, 6.2, -50.0, 0.012, NULL, 0, 0);

    // Notas dos extremos (A0 e C8)
    const Expected extremes[] = {{21, 0.3, 1.0}, {108, 1.6, 0.5}};
    run_case("extremos da faixa (A0 e C8)", extremes, 2, 2.5, -80.0, 0.05, NULL, 0, 0);

    // Notas rapidas: fusas a 100 BPM (75 ms) na regiao de B6
    const Expected fusas[] = {
        {91, 0.30, 0.075}, {93, 0.375, 0.075}, {95, 0.45, 0.075}, {93, 0.525, 0.075},
        {91, 0.60, 0.30},
    };
    run_case("fusas a 100 BPM (G6-B6)", fusas, 5, 1.3, -80.0, 0.05, NULL, 0, 0);

    // Nota longa com cliques de metronomo por cima: os cliques nao podem
    // virar um "reataque" (nota repetida falsa)
    const Expected sustained[] = {{64, 0.20, 1.60}};
    const double clicks_over[] = {0.6, 1.0, 1.4};
    run_case("nota longa com cliques por cima", sustained, 1, 2.2, -80.0, 0.05, clicks_over, 3, 0);

    // Mesma nota repetida rapidamente (tecla solta por ~40 ms entre os ataques)
    const Expected repeated[] = {{67, 0.30, 0.36}, {67, 0.70, 0.36}, {67, 1.10, 0.50}};
    run_case("nota repetida rapidamente", repeated, 3, 2.0, -80.0, 0.05, NULL, 0, 0);

    // Graves e agudos do piano: A0, C1, A1, C3, C6, B6, C8
    const Expected range[] = {
        {21, 0.30, 0.90}, {24, 1.40, 0.70}, {33, 2.30, 0.60}, {48, 3.10, 0.50},
        {84, 3.80, 0.40}, {95, 4.40, 0.40}, {108, 5.00, 0.40},
    };
    run_case("piano inteiro: A0, C1, A1, C3, C6, B6 e C8", range, 7, 5.8, -80.0, 0.05, NULL, 0, 0);

    // Caso real (monitor serial): E4, F4 e G4 lidos como E6, F6 e G6. O som
    // captado tinha o 4o harmonico muito mais forte que a fundamental
    static const double FOURTH[8] = {0.1, 0.1, 0.1, 1.0, 0.1, 0.05, 0.03, 0.1};
    s_harmonics = FOURTH;
    const Expected strong4[] = {
        {64, 0.30, 0.70}, {64, 1.10, 0.70}, {65, 1.90, 0.70}, {67, 2.70, 0.70}, {67, 3.50, 0.70},
    };
    run_case("4o harmonico dominante (E4 nao vira E6)", strong4, 5, 4.4, -80.0, 0.05, NULL, 0, 0);
    s_harmonics = KEYBOARD;

    if (failures == 0) {
        printf("\nTODOS OS TESTES PASSARAM\n");
        return 0;
    }
    printf("\n%d verificacoes falharam\n", failures);
    return 1;
}
