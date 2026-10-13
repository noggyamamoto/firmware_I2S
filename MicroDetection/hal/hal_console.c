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
#include "esp_rom_sys.h"
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

/* Guarda uma linha no histórico, sem quebras de linha e códigos de cor.
 * Pode ser chamada de tarefa ou de interrupção (seção crítica "SAFE"). */
static void store_line(const char *line) {
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
    if (j == 0) return;

    portENTER_CRITICAL_SAFE(&s_log_lock);
    memcpy(s_log[s_log_next], clean, (size_t)j + 1);
    s_log_next = (s_log_next + 1) % CONSOLE_LOG_LINES;
    if (s_log_count < CONSOLE_LOG_LINES) s_log_count++;
    s_log_version++;
    portEXIT_CRITICAL_SAFE(&s_log_lock);
}

/* Substitui a saída do ESP_LOG: guarda a linha em memória, sem imprimir. */
static int log_sink(const char *format, va_list args) {
    char line[LOG_LINE_LEN];
    int len = vsnprintf(line, sizeof(line), format, args);
    if (len < 0) return len;
    store_line(line);
    return len;
}

/*
 * Saída das mensagens "precoces" (esp_rom_printf / ESP_EARLY_LOG), usadas
 * pelo watchdog de tarefas ("task_wdt: Task watchdog got triggered...") e
 * pelo "Backtrace:" que ele imprime de dentro de uma interrupção. Essas
 * mensagens não passam pelo ESP_LOG; sem este desvio elas apareceriam no
 * meio do menu. Agora vão para o log técnico, caractere a caractere.
 * (O relatório de um travamento real – panic – continua indo direto à UART.)
 */
static char         s_rom_line[LOG_LINE_LEN];
static int          s_rom_len = 0;
static portMUX_TYPE s_rom_lock = portMUX_INITIALIZER_UNLOCKED;

static void rom_putc(char c) {
    char done[LOG_LINE_LEN];
    bool flush = false;
    portENTER_CRITICAL_SAFE(&s_rom_lock);
    if (c == '\n' || c == '\r' || s_rom_len >= LOG_LINE_LEN - 1) {
        if (s_rom_len > 0) {
            memcpy(done, s_rom_line, (size_t)s_rom_len);
            done[s_rom_len] = '\0';
            flush = true;
        }
        s_rom_len = 0;
        if (c != '\n' && c != '\r') s_rom_line[s_rom_len++] = c;
    } else {
        s_rom_line[s_rom_len++] = c;
    }
    portEXIT_CRITICAL_SAFE(&s_rom_lock);
    if (flush) store_line(done);
}

void hal_console_capture_logs(void) {
    esp_log_set_vprintf(log_sink);
    esp_rom_install_channel_putc(1, rom_putc);
}

/*
 * O terminal mostra somente ASCII: acentos e cedilha viram a letra sem
 * acento e outros símbolos viram um caractere ASCII equivalente (ou '?').
 * Cada caractere UTF-8 vira exatamente um caractere, preservando o
 * alinhamento das telas. Vale também para textos dinâmicos (nome da rede,
 * nome do app, linhas do log técnico).
 */
static const char LATIN1_ASCII[64] =                    // U+00C0 .. U+00FF
    "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYTs"
    "aaaaaaaceeeeiiiidnooooo/ouuuuyty";

static char utf8_to_ascii(const unsigned char *p, int *consumed) {
    unsigned char c = p[0];
    int len = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
    for (int i = 1; i < len; i++) {
        if ((p[i] & 0xC0) != 0x80) { len = i; break; }  // Sequência inválida/truncada
    }
    *consumed = len;
    if (len == 2 && c == 0xC3) return LATIN1_ASCII[p[1] & 0x3F];
    if (len == 2 && c == 0xC2) {
        switch (p[1]) {
            case 0xB7: return '-';                      // ·
            case 0xBA: case 0xB0: return 'o';           // º °
            case 0xAA: return 'a';                      // ª
            case 0xB5: return 'u';                      // µ
            default:   return '?';
        }
    }
    if (len == 3 && c == 0xE2) {
        if (p[1] == 0x94 && p[2] == 0x80) return '-';  // ─
        if (p[1] == 0x80 && (p[2] == 0x93 || p[2] == 0x94)) return '-';    // – —
        if (p[1] == 0x96 && p[2] == 0xB8) return '>';  // ▸
        if (p[1] == 0x80 && p[2] == 0xBA) return '>';  // ›
    }
    return '?';
}

/* Envia o texto convertido para ASCII, em blocos. */
static void write_ascii(const char *text) {
    char chunk[128];
    int n = 0;
    const unsigned char *p = (const unsigned char *)text;
    while (*p) {
        if (*p < 0x80) {
            chunk[n++] = (char)*p++;
        } else if ((*p & 0xC0) == 0x80) {
            p++;                                        // Continuação solta: descarta
        } else {
            int used;
            chunk[n++] = utf8_to_ascii(p, &used);
            p += used;
        }
        if (n == (int)sizeof(chunk)) {
            drv_uart_write(UART_PORT, chunk, (size_t)n);
            n = 0;
        }
    }
    if (n > 0) drv_uart_write(UART_PORT, chunk, (size_t)n);
}

void hal_console_printf(const char *format, ...) {
    char buf[256];                                      // Buffer para montar a string
    va_list args;
    va_start(args, format);
    vsnprintf(buf, sizeof(buf), format, args);
    va_end(args);
    write_ascii(buf);
}

void hal_console_write(const char *text) {
    write_ascii(text);
}

bool hal_console_read_byte(uint8_t *out, uint32_t timeout_ms) {
    return drv_uart_read_byte(UART_PORT, out, timeout_ms);
}

int hal_console_log_lines(char (*out)[96], int max_lines) {
    portENTER_CRITICAL_SAFE(&s_log_lock);
    int n = s_log_count < max_lines ? s_log_count : max_lines;
    int start = (s_log_next - n + CONSOLE_LOG_LINES) % CONSOLE_LOG_LINES;
    for (int i = 0; i < n; i++) {
        memcpy(out[i], s_log[(start + i) % CONSOLE_LOG_LINES], LOG_LINE_LEN);
    }
    portEXIT_CRITICAL_SAFE(&s_log_lock);
    return n;
}

uint32_t hal_console_log_version(void) {
    return s_log_version;
}
