/*
 * ============================================================================
 * Teste do monitor da execucao (app/execution_monitor.c) no computador
 *
 * Simula uma execucao: contagem de entrada, notas esperadas enviadas pelo
 * app e notas captadas pelo microfone, e confere as linhas do terminal.
 *
 * Compilar e executar:  make -C test/host monitor
 * ============================================================================
 */
#include <stdio.h>
#include <string.h>

#include "execution_monitor.h"

static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("  FALHA: " __VA_ARGS__); printf("\n"); } } while (0)

static char lines[64][EXEC_MONITOR_LINE_LEN];
static int total = 0;

static void poll_at(uint32_t ms) {
    char out[16][EXEC_MONITOR_LINE_LEN];
    int n = execution_monitor_poll(ms, out, 16);
    for (int i = 0; i < n && total < 64; i++) {
        printf("  %s\n", out[i]);
        memcpy(lines[total++], out[i], EXEC_MONITOR_LINE_LEN);
    }
}

static int find(const char *text) {
    for (int i = 0; i < total; i++) if (strstr(lines[i], text)) return i;
    return -1;
}

static NoteEvent on(int midi, uint32_t t, float hz) {
    NoteEvent e = {.type = NOTE_EVENT_ON, .midi = (uint8_t)midi, .time_ms = t, .frequency_hz = hz};
    return e;
}

static NoteEvent off(int midi, uint32_t t, uint32_t dur) {
    NoteEvent e = {.type = NOTE_EVENT_OFF, .midi = (uint8_t)midi, .time_ms = t, .duration_ms = dur};
    return e;
}

int main(void) {
    char name[8];
    execution_monitor_note_name(60, name, sizeof(name));
    CHECK(strcmp(name, "C4") == 0, "60 deveria ser C4 (%s)", name);
    execution_monitor_note_name(95, name, sizeof(name));
    CHECK(strcmp(name, "B6") == 0, "95 deveria ser B6 (%s)", name);
    bool exact;
    CHECK(strcmp(execution_monitor_figure_name(1.0f, &exact), "seminima") == 0 && exact, "seminima");
    CHECK(strcmp(execution_monitor_figure_name(0.5f, &exact), "colcheia") == 0, "colcheia");
    CHECK(strcmp(execution_monitor_figure_name(0.125f, &exact), "fusa") == 0, "fusa");
    CHECK(strcmp(execution_monitor_figure_name(0.47f, &exact), "colcheia") == 0 && exact,
          "duracao medida 0.47 deveria ser colcheia");

    printf("Execucao: 100 BPM, 4/4, 2 compassos de contagem (4800 ms)\n");
    execution_monitor_session_start(100, 4, 4, 2);
    CHECK(execution_monitor_take_started(), "inicio deveria abrir a tela");
    CHECK(!execution_monitor_take_started(), "inicio so uma vez");
    execution_monitor_expected(0, 60, 4800, 600, 1000);     // C4 seminima
    execution_monitor_expected(1, 62, 5400, 300, 500);      // D4 colcheia
    execution_monitor_expected(2, 95, 5700, 75, 125);       // B6 fusa

    poll_at(1000);
    NoteEvent ci = on(64, 2000, 329.6f);                    // Durante a contagem
    execution_monitor_captured(&ci);
    poll_at(2100);
    NoteEvent a = on(60, 4810, 261.7f), a2 = off(60, 5380, 570);
    NoteEvent b = on(64, 5420, 329.6f), b2 = off(64, 5690, 270);
    execution_monitor_captured(&a);
    poll_at(4900);
    execution_monitor_captured(&a2);
    execution_monitor_captured(&b);
    execution_monitor_captured(&b2);
    poll_at(6000);
    execution_monitor_session_stop();
    poll_at(6000);

    CHECK(find("EXECUCAO INICIADA: 100 BPM") >= 0, "cabecalho");
    CHECK(find("durante a contagem") >= 0, "nota na contagem marcada");
    int music = find("Musica iniciada");
    int exp0 = find("ESPERADO #0   C4   seminima");
    CHECK(music >= 0 && exp0 > music, "musica iniciada antes da primeira esperada");
    CHECK(find("esperado #0 C4: altura OK, tempo +10 ms") >= 0, "C4 no tempo");
    CHECK(find("ESPERADO #1   D4   colcheia") >= 0, "D4 colcheia");
    CHECK(find("esperado #1 D4: altura ERRADA") >= 0, "E4 no lugar do D4");
    CHECK(find("ESPERADO #2   B6   fusa") >= 0, "B6 fusa");
    CHECK(find("solta       C4   seminima") >= 0, "duracao do C4 como seminima");
    CHECK(find("ENCERRADA: 3 nota(s) esperada(s), 3 captada(s), 1 com a altura certa") >= 0,
          "resumo");
    for (int i = 0; i < total; i++) {
        for (const char *c = lines[i]; *c; c++) {
            CHECK((unsigned char)*c < 0x80, "linha %d com caractere especial", i);
        }
    }

    if (failures == 0) {
        printf("\nMONITOR: TODOS OS TESTES PASSARAM\n");
        return 0;
    }
    printf("\nMONITOR: %d verificacoes falharam\n", failures);
    return 1;
}
