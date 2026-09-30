/*
 * ============================================================================
 * Interface serial de controle e diagnóstico (UART0, 115200 bps)
 * ============================================================================
 */
#ifndef SERIAL_UI_H
#define SERIAL_UI_H

/** @brief Configura a UART0. */
void serial_ui_init(void);

/** @brief printf enviado pela UART. */
void uart_printf(const char *format, ...);

/** @brief Cria a tarefa do menu (núcleo 0, prioridade baixa). */
void serial_ui_start_task(void);

#endif // SERIAL_UI_H
