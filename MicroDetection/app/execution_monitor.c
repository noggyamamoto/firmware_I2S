/*
 * ============================================================================
 * app/execution_monitor – implementacao
 *
 * Estado protegido por spinlock (secoes curtas, sem chamadas bloqueantes):
 * escrito pela rede (notas esperadas), pelo audio (notas captadas) e pelo
 * metronomo (BPM); lido pela tarefa do menu, que formata e imprime.
 * ============================================================================
 */
#include "execution_monitor.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
#define LOCK()   portENTER_CRITICAL(&s_lock)
#define UNLOCK() portEXIT_CRITICAL(&s_lock)
#else
#define LOCK()   ((void)0)                      // Teste no computador (uma thread)
#define UNLOCK() ((void)0)
#endif

#define EXP_SLOTS           64                  // Notas esperadas guardadas (indice % 64)
#define CAP_SLOTS           32                  // Notas captadas aguardando impressao
#define MATCH_WINDOW_MS     400                 // Distancia maxima para comparar com a esperada
#define COUNT_IN_MARGIN_MS  400                 // O app ignora notas antes da musica - 400 ms

typedef struct {
    bool     used;
    bool     printed;
    uint16_t index;
    uint8_t  midi;
    uint32_t onset_ms;
    uint32_t duration_ms;
    uint16_t beats_milli;
} ExpEntry;

static ExpEntry  s_exp[EXP_SLOTS];
static NoteEvent s_cap[CAP_SLOTS];
static int       s_cap_head = 0, s_cap_count = 0;
static uint32_t  s_cap_lost = 0;

static struct {
    bool     active;
    bool     started_flag;                      // Para a tela abrir sozinha
    bool     start_pending;                     // Cabecalho ainda nao impresso
    bool     stop_pending;                      // Resumo ainda nao impresso
    bool     music_printed;
    uint16_t bpm;                               // BPM vigente (unidade do compasso)
    uint8_t  beats_per_bar;
    uint8_t  beat_type;
    uint8_t  count_in_bars;
    uint32_t count_in_ms;
    uint32_t captured;
    uint32_t expected;
    uint32_t pitch_ok;
} s_run;

// ======================= NOMES ===============================================

void execution_monitor_note_name(int midi, char *buf, int len) {
    static const char *names[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    if (midi < 0 || midi > 127) {
        snprintf(buf, len, "--");
        return;
    }
    snprintf(buf, len, "%s%d", names[midi % 12], midi / 12 - 1);
}

const char *execution_monitor_figure_name(float quarters, bool *exact) {
    static const struct { float q; const char *name; } figures[] = {
        {4.0f, "semibreve"},           {3.0f, "minima pontuada"},
        {2.0f, "minima"},              {1.5f, "seminima pontuada"},
        {1.0f, "seminima"},            {0.75f, "colcheia pontuada"},
        {0.5f, "colcheia"},            {0.375f, "semicolcheia pontuada"},
        {0.25f, "semicolcheia"},       {0.1875f, "fusa pontuada"},
        {0.125f, "fusa"},              {0.0625f, "semifusa"},
    };
    const int n = (int)(sizeof(figures) / sizeof(figures[0]));
    if (quarters <= 0.0f) {
        if (exact) *exact = false;
        return "?";
    }
    // Distancia em escala logaritmica (dobro e metade ficam igualmente longe)
    int best = 0;
    float best_d = 1e9f;
    for (int i = 0; i < n; i++) {
        float d = fabsf(log2f(quarters / figures[i].q));
        if (d < best_d) {
            best_d = d;
            best = i;
        }
    }
    if (exact) *exact = best_d < 0.15f;         // Ate ~10% de diferenca
    return figures[best].name;
}

// ======================= PRODUTORES ==========================================

void execution_monitor_session_start(uint16_t bpm, uint8_t beats_per_bar, uint8_t beat_type,
                                     uint8_t count_in_bars) {
    if (bpm == 0) bpm = 60;
    if (beat_type == 0) beat_type = 4;
    LOCK();
    memset(s_exp, 0, sizeof(s_exp));
    s_cap_head = s_cap_count = 0;
    s_cap_lost = 0;
    memset(&s_run, 0, sizeof(s_run));
    s_run.active = true;
    s_run.started_flag = true;
    s_run.start_pending = true;
    s_run.bpm = bpm;
    s_run.beats_per_bar = beats_per_bar;
    s_run.beat_type = beat_type;
    s_run.count_in_bars = count_in_bars;
    s_run.count_in_ms = (uint32_t)count_in_bars * beats_per_bar * 60000u / bpm;
    UNLOCK();
}

void execution_monitor_session_stop(void) {
    LOCK();
    if (s_run.active) s_run.stop_pending = true;
    s_run.active = false;
    UNLOCK();
}

void execution_monitor_set_bpm(uint16_t bpm) {
    if (bpm == 0) return;
    LOCK();
    s_run.bpm = bpm;
    UNLOCK();
}

void execution_monitor_expected(uint16_t index, uint8_t midi, uint32_t onset_ms,
                                uint32_t duration_ms, uint16_t beats_milli) {
    LOCK();
    if (s_run.active) {
        ExpEntry *e = &s_exp[index % EXP_SLOTS];
        bool same = e->used && e->index == index;
        if (!same) s_run.expected++;
        bool printed = same && e->printed;
        e->used = true;
        e->printed = printed;
        e->index = index;
        e->midi = midi;
        e->onset_ms = onset_ms;
        e->duration_ms = duration_ms;
        e->beats_milli = beats_milli;
    }
    UNLOCK();
}

void execution_monitor_captured(const NoteEvent *ev) {
    LOCK();
    if (s_run.active) {
        if (s_cap_count < CAP_SLOTS) {
            s_cap[(s_cap_head + s_cap_count) % CAP_SLOTS] = *ev;
            s_cap_count++;
        } else {
            s_cap_lost++;                       // Menu nao acompanhou: so o log perde
        }
    }
    UNLOCK();
}

bool execution_monitor_take_started(void) {
    LOCK();
    bool started = s_run.started_flag;
    s_run.started_flag = false;
    UNLOCK();
    return started;
}

bool execution_monitor_active(void) {
    LOCK();
    bool active = s_run.active;
    UNLOCK();
    return active;
}

// ======================= FORMATACAO ==========================================

typedef struct {
    uint32_t time_ms;
    bool     captured;                          // false = nota esperada
    ExpEntry exp;
    NoteEvent cap;
} Item;

static int cmp_item(const void *a, const void *b) {
    const Item *x = a, *y = b;
    if (x->time_ms != y->time_ms) return x->time_ms < y->time_ms ? -1 : 1;
    return (int)x->captured - (int)y->captured;  // Esperada antes da captada
}

/* Esperada mais proxima de `t` (dentro da janela). Chamar com o lock. */
static bool nearest_expected(uint32_t t, ExpEntry *out) {
    int32_t best_d = MATCH_WINDOW_MS + 1;
    for (int i = 0; i < EXP_SLOTS; i++) {
        if (!s_exp[i].used) continue;
        int32_t d = (int32_t)(t - s_exp[i].onset_ms);
        if (abs(d) < abs(best_d)) {
            best_d = d;
            *out = s_exp[i];
        }
    }
    return abs(best_d) <= MATCH_WINDOW_MS;
}

int execution_monitor_poll(uint32_t session_ms, char (*out)[EXEC_MONITOR_LINE_LEN], int max) {
    Item items[24];
    int n_items = 0;
    int n = 0;
    bool start, stop, music;
    uint16_t bpm;
    uint8_t beats, beat_type, count_in_bars;
    uint32_t count_in_ms, captured, expected, pitch_ok, lost;

    LOCK();
    start = s_run.start_pending;
    s_run.start_pending = false;
    music = !s_run.music_printed && (s_run.active || s_run.stop_pending) &&
            session_ms >= s_run.count_in_ms;
    if (music) s_run.music_printed = true;
    bpm = s_run.bpm;
    beats = s_run.beats_per_bar;
    beat_type = s_run.beat_type;
    count_in_bars = s_run.count_in_bars;
    count_in_ms = s_run.count_in_ms;
    // Esperadas cuja hora chegou
    for (int i = 0; i < EXP_SLOTS && n_items < 12; i++) {
        ExpEntry *e = &s_exp[i];
        if (e->used && !e->printed && (e->onset_ms <= session_ms || s_run.stop_pending)) {
            e->printed = true;
            items[n_items].time_ms = e->onset_ms;
            items[n_items].captured = false;
            items[n_items].exp = *e;
            n_items++;
        }
    }
    // Captadas
    while (s_cap_count > 0 && n_items < (int)(sizeof(items) / sizeof(items[0]))) {
        Item *it = &items[n_items++];
        it->cap = s_cap[s_cap_head];
        it->time_ms = it->cap.time_ms;
        it->captured = true;
        s_cap_head = (s_cap_head + 1) % CAP_SLOTS;
        s_cap_count--;
        if (it->cap.type == NOTE_EVENT_ON) s_run.captured++;
    }
    // A parada so e impressa depois de esvaziar o que faltava
    stop = s_run.stop_pending && s_cap_count == 0;
    if (stop) s_run.stop_pending = false;
    lost = s_cap_lost;
    UNLOCK();

    if (start && n < max) {
        snprintf(out[n++], EXEC_MONITOR_LINE_LEN,
                 "=== EXECUCAO INICIADA: %u BPM, compasso %u/%u, contagem de %u compasso(s) "
                 "(%lu ms) ===", bpm, beats, beat_type, count_in_bars, (unsigned long)count_in_ms);
        if (n < max) {
            snprintf(out[n++], EXEC_MONITOR_LINE_LEN,
                     "    Contagem de entrada: notas tocadas agora sao ignoradas pelo app.");
        }
    }

    qsort(items, (size_t)n_items, sizeof(Item), cmp_item);
    float quarter_ms = 60000.0f / (bpm ? bpm : 60) * beat_type / 4.0f;

    bool music_line_pending = music;
    for (int i = 0; i < n_items && n < max; i++) {
        Item *it = &items[i];
        if (music_line_pending && it->time_ms >= count_in_ms) {
            snprintf(out[n++], EXEC_MONITOR_LINE_LEN, "--- Musica iniciada (t=%lu ms) ---",
                     (unsigned long)count_in_ms);
            music_line_pending = false;
            if (n >= max) break;
        }
        char name[8];
        if (!it->captured) {
            bool exact;
            execution_monitor_note_name(it->exp.midi, name, sizeof(name));
            const char *fig = execution_monitor_figure_name(it->exp.beats_milli / 1000.0f, &exact);
            snprintf(out[n++], EXEC_MONITOR_LINE_LEN,
                     "ESPERADO #%-3u %-4s %-22s t=%6lu ms  dur %4lu ms", it->exp.index, name,
                     exact ? fig : "quialtera/irregular", (unsigned long)it->exp.onset_ms,
                     (unsigned long)it->exp.duration_ms);
            continue;
        }

        const NoteEvent *e = &it->cap;
        execution_monitor_note_name(e->midi, name, sizeof(name));
        if (e->type == NOTE_EVENT_ON) {
            char verdict[64];
            ExpEntry match = {0};
            bool found;
            LOCK();
            found = nearest_expected(e->time_ms, &match);
            if (found && match.midi == e->midi) s_run.pitch_ok++;
            UNLOCK();
            if (e->time_ms + COUNT_IN_MARGIN_MS < count_in_ms) {
                snprintf(verdict, sizeof(verdict), "(durante a contagem: ignorada)");
            } else if (found) {
                char exp_name[8];
                execution_monitor_note_name(match.midi, exp_name, sizeof(exp_name));
                snprintf(verdict, sizeof(verdict), "-> esperado #%u %s: altura %s, tempo %+ld ms",
                         match.index, exp_name, match.midi == e->midi ? "OK" : "ERRADA",
                         (long)((int32_t)(e->time_ms - match.onset_ms)));
            } else {
                snprintf(verdict, sizeof(verdict), "-> nenhuma nota esperada perto (extra)");
            }
            char hz[16];
            snprintf(hz, sizeof(hz), "%.1f Hz", e->frequency_hz);
            snprintf(out[n++], EXEC_MONITOR_LINE_LEN, "CAPTADO       %-4s %-22s t=%6lu ms  %s",
                     name, hz, (unsigned long)e->time_ms, verdict);
        } else {
            bool exact;
            const char *fig = execution_monitor_figure_name(e->duration_ms / quarter_ms, &exact);
            snprintf(out[n++], EXEC_MONITOR_LINE_LEN,
                     "  solta       %-4s %-22s t=%6lu ms  dur %4lu ms", name,
                     fig, (unsigned long)e->time_ms, (unsigned long)e->duration_ms);
        }
    }
    if (music_line_pending && n < max) {
        snprintf(out[n++], EXEC_MONITOR_LINE_LEN, "--- Musica iniciada (t=%lu ms) ---",
                 (unsigned long)count_in_ms);
    }

    if (stop && n < max) {
        LOCK();
        captured = s_run.captured;
        expected = s_run.expected;
        pitch_ok = s_run.pitch_ok;
        UNLOCK();
        snprintf(out[n++], EXEC_MONITOR_LINE_LEN,
                 "=== EXECUCAO ENCERRADA: %lu nota(s) esperada(s), %lu captada(s), %lu com a "
                 "altura certa ===", (unsigned long)expected, (unsigned long)captured,
                 (unsigned long)pitch_ok);
        if (lost > 0 && n < max) {
            snprintf(out[n++], EXEC_MONITOR_LINE_LEN,
                     "    (%lu evento(s) nao couberam no monitor serial)", (unsigned long)lost);
        }
    }
    return n;
}
