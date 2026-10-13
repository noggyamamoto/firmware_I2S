/*
 * ============================================================================
 * hal/hal_console – terminal serial de texto (monitor serial, 115200 bps)
 *
 * O terminal é de uso exclusivo do menu (serial_ui): depois de
 * hal_console_capture_logs(), as mensagens do ESP_LOG (firmware, Wi-Fi,
 * lwIP...) deixam de ser impressas e passam a ser guardadas em memória
 * (últimas CONSOLE_LOG_LINES linhas), consultáveis pelo menu de diagnóstico.
 * O mesmo vale para as mensagens precoces (esp_rom_printf), como o aviso do
 * watchdog de tarefas ("task_wdt") e o "Backtrace" que o acompanha.
 *
 * Tudo o que é escrito no terminal sai em ASCII puro: acentos, cedilha e
 * símbolos especiais são trocados por equivalentes simples.
 * ============================================================================
 */
#ifndef HAL_CONSOLE_H
#define HAL_CONSOLE_H

#include <stdbool.h>
#include <stdint.h>

/** @brief Configura a UART do console (UART_PORT / UART_BAUDRATE do config.h). */
bool hal_console_init(void);

/** @brief Desvia o ESP_LOG e o esp_rom_printf do terminal para o histórico em memória. */
void hal_console_capture_logs(void);

/** @brief printf enviado pelo console. */
void hal_console_printf(const char *format, ...);

/** @brief Envia uma string sem formatação (telas grandes do menu). */
void hal_console_write(const char *text);

/**
 * @brief Lê um byte do teclado aguardando até `timeout_ms`.
 * @return true se um byte foi lido.
 */
bool hal_console_read_byte(uint8_t *out, uint32_t timeout_ms);

/**
 * @brief Copia as linhas mais recentes do log técnico (da mais antiga para a
 *        mais nova). @return quantidade copiada.
 */
int hal_console_log_lines(char (*out)[96], int max_lines);

/** @brief Contador que muda a cada nova linha de log (para atualizar a tela). */
uint32_t hal_console_log_version(void);

#endif // HAL_CONSOLE_H
