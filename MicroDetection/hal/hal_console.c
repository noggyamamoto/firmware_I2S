/*
 * ============================================================================
 * hal/hal_console – implementação sobre drivers/drv_uart
 * ============================================================================
 */
#include "hal_console.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "config.h"
#include "drv_uart.h"

bool hal_console_init(void) {
    return drv_uart_init(UART_PORT, UART_BAUDRATE, UART_RX_BUF_SIZE, UART_TX_BUF_SIZE);
}

void hal_console_printf(const char *format, ...) {
    char buf[256];                                      // Buffer para montar a string
    va_list args;
    va_start(args, format);
    vsnprintf(buf, sizeof(buf), format, args);
    va_end(args);
    drv_uart_write(UART_PORT, buf, strlen(buf));
}

int hal_console_readline(char *out, int max_len, uint32_t timeout_ms, bool echo) {
    int len = 0;
    uint32_t waited = 0;
    while (len < max_len - 1) {
        uint8_t c;
        if (!drv_uart_read_byte(UART_PORT, &c, 100)) {
            waited += 100;
            if (len == 0 && waited >= timeout_ms) return -1;
            continue;
        }
        if (c == '\r' || c == '\n') {
            if (len == 0) continue;                     // Ignora ENTER vazio / CRLF
            break;
        }
        if ((c == 0x08 || c == 0x7F) && len > 0) {      // Backspace
            len--;
            hal_console_printf("\b \b");
            continue;
        }
        if (echo) drv_uart_write(UART_PORT, &c, 1);
        out[len++] = (char)c;
    }
    out[len] = '\0';
    hal_console_printf("\n");
    return len;
}
