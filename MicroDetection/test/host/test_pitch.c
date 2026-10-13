/*
 * ============================================================================
 * Teste do detector de altura (dsp.c: pitch_process) no computador
 *
 * Toca as 88 notas do piano (A0 a C8), com inarmonicidade de corda e
 * diferentes perfis de harmonicos, e confere a nota MIDI em cada janela
 * depois que as janelas estao cheias (90% das janelas no minimo) e o erro
 * medio de afinacao.
 *
 * Compilar e executar:  make -C test/host pitch
 * ============================================================================
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dsp.h"

#define FS   16000
#define HOP  256

typedef struct {
    const char *name;
    float harmonics[8];
    int first_midi, last_midi;          // Faixa em que o perfil e realista
    float speaker_hz;                   // > 0: alto-falante pequeno (corte nesta frequencia)
} Profile;

static const Profile PROFILES[] = {
    // Teclado/piano: harmonicos decrescentes
    {"teclado", {1.0f, 0.5f, 0.33f, 0.25f, 0.2f, 0.17f, 0.14f, 0.12f}, 21, 108, 0},
    // Senoide pura (notas agudas do piano sao quase senoidais)
    {"senoide", {1.0f, 0, 0, 0, 0, 0, 0, 0}, 21, 108, 0},
    // Graves do piano: fundamental quase ausente
    {"fundamental fraca", {0.05f, 0.6f, 1.0f, 0.7f, 0.4f, 0.3f, 0.2f, 0.1f}, 21, 93, 0},
    // Caso real do monitor serial: 4o harmonico dominante (E4 lido como E6)
    {"4o harmonico dominante", {0.1f, 0.1f, 0.1f, 1.0f, 0.1f, 0.05f, 0.03f, 0.1f}, 21, 78, 0},
    // Caso real (monitor serial): teclado/celular com alto-falante pequeno,
    // que corta abaixo de ~1,2 kHz (E4 so com o 4o harmonico forte). Abaixo
    // do A3 todos os harmonicos audiveis ficariam abaixo do corte.
    {"alto-falante pequeno", {0}, 57, 96, 1200.0f},
    // Caso real (v1.3.1 na placa): C4, D4, E4, F4 e G4 lidos duas oitavas
    // acima. O som tinha praticamente so o 4o harmonico; as componentes da
    // nota (f, 3f, 5f...) chegavam ~36 dB abaixo dele. Acima do C5 um
    // alto-falante pequeno ja reproduz a propria nota (> 1 kHz)
    {"harmonico quase isolado", {0.015f, 0.015f, 0.015f, 1.0f, 0.015f, 0.01f, 0.01f, 0.01f}, 38, 72, 0},
};

/* Ganho de um alto-falante pequeno: passa-alta de 16a ordem + ressonancia. */
static double speaker_gain(double f, double fc) {
    double r = pow(f / fc, 16);
    double bump = 1 + 1.5 * exp(-pow(log2(f / (fc * 1.15)), 2) / 0.05);
    return sqrt(r / (1 + r)) * bump;
}

static PitchDetector s_pitch;

int main(void) {
    int failures = 0;
    double worst_cents = 0;
    srand(7);

    // Custo de um trecho sem nota (só ruído): referência para o custo dos
    // trechos com nota (iterações dos laços internos, independente da máquina)
    uint32_t silence_work = 0;
    {
        pitch_init(&s_pitch, FS, 26.7f, 4400.0f, 125.0f, 0.15f);
        for (int h = 0; h < 20; h++) {
            float hop[HOP];
            for (int i = 0; i < HOP; i++) hop[i] = 2e-4f * ((float)rand() / (float)RAND_MAX - 0.5f);
            float conf;
            uint32_t before = pitch_work_counter();
            pitch_process(&s_pitch, hop, HOP, &conf);
            silence_work = pitch_work_counter() - before;
        }
    }
    uint32_t worst_work = 0;
    double work_sum = 0;
    int work_hops = 0;
    for (size_t p = 0; p < sizeof(PROFILES) / sizeof(PROFILES[0]); p++) {
        const Profile *pr = &PROFILES[p];
        int ok_notes = 0;
        for (int midi = pr->first_midi; midi <= pr->last_midi; midi++) {
            const double f0 = 440.0 * pow(2.0, (midi - 69) / 12.0);
            const double B = 0.0004;                        // Inarmonicidade da corda
            const size_t len = FS * 6 / 10;
            float *x = calloc(len, sizeof(float));
            for (size_t i = 0; i < len; i++) {
                double t = (double)i / FS, s = 0;
                for (int h = 1; h <= 16; h++) {
                    double fh = f0 * h * sqrt(1 + B * h * h);
                    if (fh >= FS / 2 - 500) break;
                    double a = pr->speaker_hz > 0 ? speaker_gain(fh, pr->speaker_hz) / (h * h)
                                                  : (h <= 8 ? pr->harmonics[h - 1] : 0.0);
                    s += a * sin(2 * M_PI * fh * t + h);
                }
                x[i] = (float)(0.05 * s * exp(-t / 1.2) + 2e-4 * (rand() / (double)RAND_MAX - 0.5));
            }
            BandpassFilter bp;
            bandpass_init(&bp, FS, 20.0f, 5000.0f);
            pitch_init(&s_pitch, FS, 26.7f, 4400.0f, 125.0f, 0.15f);
            pitch_reset(&s_pitch);
            int ok = 0, total = 0;
            double cents = 0;
            float last = 0;
            for (size_t pos = 0; pos + HOP <= len; pos += HOP) {
                float hop[HOP];
                memcpy(hop, x + pos, sizeof(hop));
                bandpass_process(&bp, hop, HOP);
                float conf;
                uint32_t before = pitch_work_counter();
                float f = pitch_process(&s_pitch, hop, HOP, &conf);
                uint32_t work = pitch_work_counter() - before;
                if (work > worst_work) worst_work = work;
                work_sum += work;
                work_hops++;
                if (pos < 8 * HOP) continue;                // Janela grave (80 ms) cheia
                total++;
                last = f;
                if (f > 0 && conf >= 0.7f) {
                    double m = 69 + 12 * log2(f / 440.0);
                    if (lround(m) == midi) {
                        ok++;
                        cents += fabs(m - midi) * 100;
                    }
                }
            }
            if (ok < total * 9 / 10) {
                failures++;
                printf("  FALHA: %s, MIDI %d (%.1f Hz): %d de %d janelas corretas (ultima %.1f Hz)\n",
                       pr->name, midi, f0, ok, total, last);
            } else {
                ok_notes++;
            }
            if (ok > 0 && cents / ok > worst_cents) worst_cents = cents / ok;
            free(x);
        }
        printf("Perfil %-24s MIDI %3d a %3d: %d notas corretas\n", pr->name, pr->first_midi,
               pr->last_midi, ok_notes);
    }
    printf("Pior erro medio de afinacao: %.1f cents\n", worst_cents);
    // Desempenho no ESP32: um trecho com nota não pode custar muito mais que
    // um trecho sem nota (antes da otimização: pior 2,3x; o processamento de
    // um trecho tem de caber nos 16 ms do trecho com folga)
    printf("Custo por trecho: silencio %u, com nota media %.2fx, pior %.2fx\n", silence_work,
           work_sum / work_hops / silence_work, (double)worst_work / silence_work);
    if (worst_work > silence_work * 2.1) {
        failures++;
        printf("  FALHA: pior trecho custa %.2fx o trecho sem nota (maximo 2,1x)\n",
               (double)worst_work / silence_work);
    }
    if (worst_cents > 25.0) {
        failures++;
        printf("  FALHA: afinacao acima de 25 cents\n");
    }
    if (failures == 0) {
        printf("\nALTURA: TODOS OS TESTES PASSARAM\n");
        return 0;
    }
    printf("\nALTURA: %d verificacoes falharam\n", failures);
    return 1;
}
