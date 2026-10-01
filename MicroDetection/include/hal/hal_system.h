/*
 * ============================================================================
 * hal/hal_system – informações e controle do sistema
 * ============================================================================
 */
#ifndef HAL_SYSTEM_H
#define HAL_SYSTEM_H

#include <stdint.h>

/** @brief Reinicia o microcontrolador. */
void hal_system_restart(void);

/** @brief Memória livre (bytes). */
uint32_t hal_system_free_heap(void);

#endif // HAL_SYSTEM_H
