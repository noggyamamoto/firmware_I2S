/*
 * ============================================================================
 * hal/hal_console – implementação sobre drivers/drv_uart
 * ============================================================================
 */
#include "hal_console.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"

#include "config.h"
#include "drv_uart.h"

#define LOG_LINE_LEN    96

// Histórico do ESP_LOG. Protegido por spinlock: o ESP_LOG é chamado por
// qualquer tarefa (inclusive a dos timers e a do Wi-Fi) e não pode bloquear.
static char              s_log[CONSOLE_LOG_LINES][LOG_LINE_LEN];
static int               s_log_next = 0;
static int               s_log_count = 0;
static volatile uint32_t s_log_version = 0;
static portMUX_TYPE      s_log_lock = portMUX_INITIALIZER_UNLOCKED;

bool hal_console_init(void) {
    return drv_uart_init(UART_PORT, UART_BAUDRATE, UART_RX_BUF_SIZE, UART_TX_BUF_SIZE);
}

/* Substitui a saída do ESP_LOG: guarda a linha em memória, sem imprimir. */
static int log_sink(const char *format, va_list args) {
    char line[LOG_LINE_LEN];
    int len = vsnprintf(line, sizeof(line), format, args);
    if (len < 0) return len;

    // Remove quebras de linha e códigos de cor
    char clean[LOG_LINE_LEN];
    int j = 0;
    for (int i = 0; line[i] != '\0' && j < LOG_LINE_LEN - 1; i++) {
        if (line[i] == '\033') {                        // ESC [ ... m
            while (line[i] != '\0' && line[i] != 'm') i++;
            if (line[i] == '\0') break;
            continue;
        }
        if (line[i] == '\n' || line[i] == '\r') continue;
        clean[j++] = line[i];
    }
    clean[j] = '\0';
    if (j == 0) return len;

    portENTER_CRITICAL(&s_log_lock);
    memcpy(s_log[s_log_next], clean, (size_t)j + 1);
    s_log_next = (s_log_next + 1) % CONSOLE_LOG_LINES;
    if (s_log_count < CONSOLE_LOG_LINES) s_log_count++;
    s_log_version++;
    portEXIT_CRITICAL(&s_log_lock);
    return len;
}

void hal_console_capture_logs(void) {
    esp_log_set_vprintf(log_sink);
}

void hal_console_printf(const char *format, ...) {
    char buf[256];                                      // Buffer para montar a string
    va_list args;
    va_start(args, format);
    vsnprintf(buf, sizeof(buf), format, args);
    va_end(args);
    drv_uart_write(UART_PORT, buf, strlen(buf));
}

void hal_console_write(const char *text) {
    drv_uart_write(UART_PORT, text, strlen(text));
}

bool hal_console_read_byte(uint8_t *out, uint32_t timeout_ms) {
    return drv_uart_read_byte(UART_PORT, out, timeout_ms);
}

int hal_console_log_lines(char (*out)[96], int max_lines) {
    portENTER_CRITICAL(&s_log_lock);
    int n = s_log_count < max_lines ? s_log_count : max_lines;
    int start = (s_log_next - n + CONSOLE_LOG_LINES) % CONSOLE_LOG_LINES;
    for (int i = 0; i < n; i++) {
        memcpy(out[i], s_log[(start + i) % CONSOLE_LOG_LINES], LOG_LINE_LEN);
    }
    portEXIT_CRITICAL(&s_log_lock);
    return n;
}

uint32_t hal_console_log_version(void) {
    return s_log_version;
}
