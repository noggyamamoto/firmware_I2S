/*
 * ============================================================================
 * app/execution_monitor – monitor da execucao no terminal serial
 *
 * Durante uma execucao iniciada pelo app, mostra no monitor serial, em ordem
 * de tempo:
 *  - as notas ESPERADAS da partitura (nome, ex.: C4, e figura, ex.: seminima),
 *    recebidas do app (PKT_EXPECTED_NOTE) e impressas quando chega a hora
 *    de cada uma no relogio da sessao;
 *  - as notas CAPTADAS pelo microfone (nome, frequencia, instante e, ao
 *    soltar, a figura correspondente a duracao medida), com a comparacao com
 *    a nota esperada mais proxima.
 *
 * Os produtores (rede, audio) apenas registram os dados (sem bloquear, sem
 * imprimir); a tarefa do menu chama execution_monitor_poll() e imprime as
 * linhas prontas. Tudo em ASCII (sem acentos), como o restante do menu.
 *
 * Camada: app
 * ============================================================================
 */
#ifndef EXECUTION_MONITOR_H
#define EXECUTION_MONITOR_H

#include <stdbool.h>
#include <stdint.h>

#include "note_tracker.h"

#define EXEC_MONITOR_LINE_LEN   160

/** @brief Nova execucao do app (contagem de entrada incluida). */
void execution_monitor_session_start(uint16_t bpm, uint8_t beats_per_bar, uint8_t beat_type,
                                     uint8_t count_in_bars);

/** @brief Fim da execucao. */
void execution_monitor_session_stop(void);

/** @brief BPM vigente do metronomo (figura das notas captadas). */
void execution_monitor_set_bpm(uint16_t bpm);

/** @brief Nota esperada enviada pelo app (substitui o mesmo indice). */
void execution_monitor_expected(uint16_t index, uint8_t midi, uint32_t onset_ms,
                                uint32_t duration_ms, uint16_t beats_milli);

/**
 * @brief Nota captada pelo microfone (chamada pela tarefa de audio; nao bloqueia).
 * @param probe_half_quarter_db Energia (dB) em f/2 e f/4 da nota (diagnostico de
 *        oitava) ou NULL.
 */
void execution_monitor_captured(const NoteEvent *ev, const float *probe_half_quarter_db);

/**
 * @brief Linhas a imprimir ate o instante `session_ms` (relogio da sessao).
 * @return quantidade de linhas escritas em `out`.
 */
int execution_monitor_poll(uint32_t session_ms, char (*out)[EXEC_MONITOR_LINE_LEN], int max);

/** @brief true uma unica vez depois de cada inicio de execucao (abre a tela). */
bool execution_monitor_take_started(void);

/** @brief Execucao do app em andamento. */
bool execution_monitor_active(void);

/** @brief Nome da nota no padrao internacional (60 -> "C4", 70 -> "A#4"). */
void execution_monitor_note_name(int midi, char *buf, int len);

/** @brief Nome da figura mais proxima de uma duracao em seminimas (1 = seminima). */
const char *execution_monitor_figure_name(float quarters, bool *exact);

#endif // EXECUTION_MONITOR_H
