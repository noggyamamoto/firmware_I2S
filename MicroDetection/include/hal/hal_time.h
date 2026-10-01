/*
 * ============================================================================
 * hal/hal_time – relógio monotônico e temporizadores de alta resolução
 * ============================================================================
 */
#ifndef HAL_TIME_H
#define HAL_TIME_H

#include <stdbool.h>
#include <stdint.h>

typedef struct HalTimer *HalTimer;
typedef void (*hal_timer_cb)(void *arg);

/** @brief Microssegundos desde o boot (relógio monotônico). */
int64_t hal_time_us(void);

/** @brief Cria um temporizador (callback executado na tarefa de timers). */
HalTimer hal_timer_create(hal_timer_cb callback, void *arg, const char *name);

/** @brief Dispara uma única vez após `delay_us` (reinicia se já estiver ativo). */
void hal_timer_start_once(HalTimer timer, uint64_t delay_us);

/** @brief Dispara periodicamente a cada `period_us`. */
void hal_timer_start_periodic(HalTimer timer, uint64_t period_us);

/** @brief Para o temporizador (sem efeito se não estiver ativo). */
void hal_timer_stop(HalTimer timer);

#endif // HAL_TIME_H
