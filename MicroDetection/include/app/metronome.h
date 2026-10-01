/*
 * ============================================================================
 * Metrônomo sonoro (RFE02) e visual (RFE03)
 *
 * As batidas são agendadas em instantes absolutos (sem deriva acumulada) a
 * partir do instante zero da sessão. Os primeiros `count_in_bars` compassos
 * formam a contagem de entrada (RFA05 / RU07).
 *
 * Cores do metrônomo visual por fórmula de compasso:
 *   binário (2)     -> azul
 *   ternário (3)    -> verde
 *   quaternário (4) -> roxo
 *   contagem        -> amarelo
 * O tempo forte acende com brilho máximo e duração maior.
 * ============================================================================
 */
#ifndef METRONOME_H
#define METRONOME_H

#include <stdint.h>

void metronome_init(void);

/** @brief Inicia a primeira batida em `t0_us` (instante zero da sessão). */
void metronome_start(uint16_t bpm, uint8_t beats_per_bar, uint8_t count_in_bars,
                     uint8_t flags, int64_t t0_us);

/** @brief Altera o andamento a partir da próxima batida. */
void metronome_set_tempo(uint16_t bpm);

/** @brief Liga/desliga som e luz (CFG_FLAG_METRO_*). */
void metronome_set_flags(uint8_t flags);

void metronome_stop(void);

/** @brief Toca `beats` batidas de teste (menu serial). */
void metronome_test(uint16_t bpm, uint8_t beats_per_bar);

#endif // METRONOME_H
