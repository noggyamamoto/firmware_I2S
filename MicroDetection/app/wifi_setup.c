/*
 * ============================================================================
 * app/wifi_setup – implementacao (texto simples, funciona em qualquer
 * monitor serial: PlatformIO, Arduino IDE, PuTTY...)
 * ============================================================================
 */
#include "wifi_setup.h"

#include <stdio.h>
#include <string.h>

#include "config.h"
#include "hal_console.h"
#include "hal_time.h"
#include "hal_wifi.h"

typedef enum {
    READ_OK,                // Texto digitado + ENTER
    READ_EMPTY,             // ENTER sem texto
    READ_CANCEL,            // ESC
    READ_TIMEOUT,           // Ninguem respondeu
} ReadResult;

static int64_t s_last_enter_us = 0;

/* Mostra o tempo restante na linha do campo (somente terminais com ANSI). */
static void show_prompt(const char *prompt, int seconds_left) {
#if SERIAL_UI_ANSI
    if (seconds_left >= 0) {
        hal_console_printf("\r\033[K%s(%2d s) ", prompt, seconds_left);
        return;
    }
#endif
    (void)seconds_left;
    hal_console_printf("%s", prompt);
}

/*
 * Le uma linha com eco. `first_key_ms` = tempo maximo ate a primeira tecla
 * (0 = sem limite); depois dela, WIFI_SETUP_IDLE_MS entre teclas.
 */
static ReadResult read_line(const char *prompt, char *buf, int max, bool secret,
                            uint32_t first_key_ms) {
    int len = 0;
    bool typed = false;
    int64_t start = hal_time_us();
    int64_t last_key = start;
    buf[0] = '\0';

    show_prompt(prompt, first_key_ms ? (int)(first_key_ms / 1000) : -1);
#if SERIAL_UI_ANSI
    int last_shown = (int)(first_key_ms / 1000);
#else
    if (first_key_ms) hal_console_printf("(%d s) ", (int)(first_key_ms / 1000));
#endif

    while (1) {
        int64_t now = hal_time_us();
        if (!typed && first_key_ms) {
            int64_t left_ms = (int64_t)first_key_ms - (now - start) / 1000;
            if (left_ms <= 0) {
                hal_console_write("\r\n");
                return READ_TIMEOUT;
            }
#if SERIAL_UI_ANSI
            int left_s = (int)((left_ms + 999) / 1000);
            if (left_s != last_shown) {
                show_prompt(prompt, left_s);
                last_shown = left_s;
            }
#endif
        } else if (typed && now - last_key > (int64_t)WIFI_SETUP_IDLE_MS * 1000) {
            hal_console_write("\r\n");
            return READ_TIMEOUT;
        }

        uint8_t c;
        if (!hal_console_read_byte(&c, 100)) continue;
        now = hal_time_us();

        if (c == '\r' || c == '\n') {
            // CR+LF (ou LF logo depois do ENTER anterior) conta uma vez so
            if (c == '\n' && now - s_last_enter_us < 300000) continue;
            s_last_enter_us = now;
            hal_console_write("\r\n");
            buf[len] = '\0';
            return len > 0 ? READ_OK : READ_EMPTY;
        }
        typed = true;
        last_key = now;
        if (c == 0x1B) {                                // ESC (setas tambem comecam com ESC)
            uint8_t extra;
            while (hal_console_read_byte(&extra, 20)) {}
            hal_console_write("\r\n");
            return READ_CANCEL;
        }
        if (c == 0x08 || c == 0x7F) {                   // Apagar
            if (len > 0) {
                while (len > 0 && ((unsigned char)buf[len - 1] & 0xC0) == 0x80) len--;
                if (len > 0) len--;
                buf[len] = '\0';
                hal_console_write("\b \b");
            }
            continue;
        }
        if (c < 0x20 || len >= max) continue;           // Controle ou limite atingido
        buf[len++] = (char)c;
        buf[len] = '\0';
        if (((unsigned char)c & 0xC0) == 0x80) continue;    // Continuacao UTF-8: sem eco
        char echo[2] = {secret ? '*' : (char)c, '\0'};
        hal_console_write(echo);
    }
}

/* Descarta o que sobrou no teclado (ex.: LF do ultimo ENTER) antes do menu. */
static void drain_input(void) {
    uint8_t c;
    while (hal_console_read_byte(&c, 150)) {}
}

static void banner(void) {
    WifiCredentials def;
    hal_storage_default_wifi(&def);
#if SERIAL_UI_ANSI
    hal_console_write("\033[2J\033[H\033[?25h");
#endif
    hal_console_write("\r\n========================================================================\r\n");
    hal_console_printf("  PARTITURA IoT - firmware %s - REDE WI-FI\r\n", FIRMWARE_VERSION);
    hal_console_write("========================================================================\r\n");
    hal_console_printf("  Rede padrao: \"%s\"\r\n", def.ssid);
    hal_console_write("  Digite o nome da rede (SSID) a que o dispositivo deve se conectar\r\n");
    hal_console_write("  e tecle ENTER. Depois, digite a senha.\r\n");
    hal_console_printf("  ENTER sem digitar, ESC ou %d s sem tecla: usa a rede padrao.\r\n",
                       WIFI_SETUP_TIMEOUT_MS / 1000);
    hal_console_write("  Se a rede digitada nao conectar, a rede padrao e usada.\r\n\r\n");
}

bool wifi_setup_ask(WifiCredentials *out) {
    memset(out, 0, sizeof(*out));
    banner();

    char ssid[HAL_SSID_MAX + 1];
    ReadResult r = read_line("  Nome da rede: ", ssid, HAL_SSID_MAX, false, WIFI_SETUP_TIMEOUT_MS);
    if (r != READ_OK) {
        hal_console_write(r == READ_TIMEOUT ? "  Nenhuma resposta: usando a rede padrao.\r\n"
                                            : "  Usando a rede padrao.\r\n");
        drain_input();
        return false;
    }

    char pass[HAL_PASS_MAX + 1];
    for (int attempt = 0; attempt < 3; attempt++) {
        r = read_line("  Senha (ENTER vazio = rede aberta): ", pass, HAL_PASS_MAX, true, 0);
        if (r == READ_CANCEL || r == READ_TIMEOUT) {
            hal_console_write("  Cancelado: usando a rede padrao.\r\n");
            memset(pass, 0, sizeof(pass));
            drain_input();
            return false;
        }
        if (r == READ_EMPTY) pass[0] = '\0';
        if (hal_wifi_check_password(pass) == HAL_WIFI_ERR_NONE) {
            strncpy(out->ssid, ssid, HAL_SSID_MAX);
            strncpy(out->password, pass, HAL_PASS_MAX);
            memset(pass, 0, sizeof(pass));
            hal_console_printf("  Conectando a \"%s\" (reserva: rede padrao)...\r\n", out->ssid);
            drain_input();
            return true;
        }
        hal_console_write("  Senha invalida: senhas WPA/WPA2 tem de 8 a 63 caracteres.\r\n");
    }
    memset(pass, 0, sizeof(pass));
    hal_console_write("  Usando a rede padrao.\r\n");
    drain_input();
    return false;
}
