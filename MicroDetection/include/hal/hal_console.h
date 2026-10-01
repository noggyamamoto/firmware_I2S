/*
 * ============================================================================
 * hal/hal_console – terminal serial de texto (monitor serial, 115200 bps)
 * ============================================================================
 */
#ifndef HAL_CONSOLE_H
#define HAL_CONSOLE_H

#include <stdbool.h>
#include <stdint.h>

/** @brief Configura a UART do console (UART_PORT / UART_BAUDRATE do config.h). */
bool hal_console_init(void);

/** @brief printf enviado pelo console. */
void hal_console_printf(const char *format, ...);

/**
 * @brief Lê uma linha até ENTER (com eco opcional e suporte a backspace).
 * @return tamanho lido ou -1 se nada foi digitado dentro de `timeout_ms`.
 */
int hal_console_readline(char *out, int max_len, uint32_t timeout_ms, bool echo);

#endif // HAL_CONSOLE_H
