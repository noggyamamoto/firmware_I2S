/*
 * ============================================================================
 * app/serial_ui – implementação do menu serial
 *
 * Cada tela é montada em um buffer (uma linha por vez) e enviada de uma só
 * vez. Nas atualizações, somente as linhas que mudaram são reescritas
 * (posicionamento de cursor ANSI), o que evita o "pisca" e economiza a UART.
 * ============================================================================
 */
#include "serial_ui.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app_state.h"
#include "audio_pipeline.h"
#include "config.h"
#include "diagnostics.h"
#include "dsp.h"
#include "hal_console.h"
#include "hal_net.h"
#include "hal_storage.h"
#include "hal_system.h"
#include "hal_time.h"
#include "hal_wifi.h"
#include "link_service.h"
#include "metronome.h"
#include "protocol.h"
#include "ui_events.h"

// ======================= ESTILO (vermelho e branco) ==========================

#if SERIAL_UI_ANSI
#define ST_RESET    "\033[0m"
#define ST_BANNER   "\033[1;97;41m"     // Branco em negrito sobre vermelho
#define ST_SELECTED "\033[1;97;41m"     // Item selecionado
#define ST_TITLE    "\033[1;97m"        // Branco em negrito
#define ST_TEXT     "\033[0;97m"        // Branco
#define ST_SOFT     "\033[0;37m"        // Branco suave (dicas)
#define ST_KEY      "\033[1;91m"        // Vermelho (atalhos e destaques)
#define ST_ERROR    "\033[1;91m"        // Vermelho em negrito
#define ST_RULE     "\033[0;31m"        // Linhas divisórias
#else
#define ST_RESET    ""
#define ST_BANNER   ""
#define ST_SELECTED ""
#define ST_TITLE    ""
#define ST_TEXT     ""
#define ST_SOFT     ""
#define ST_KEY      ""
#define ST_ERROR    ""
#define ST_RULE     ""
#endif

#define WIDTH           76              // Colunas úteis da tela (terminal de 80)
#define MAX_LINES       48
#define SCREEN_BYTES    6144

// ======================= BUFFER DE TELA ======================================

typedef struct {
    char data[SCREEN_BYTES];
    int  len;
    int  lines;
    int  start[MAX_LINES + 1];          // Início de cada linha em data
} ScreenBuf;

static ScreenBuf s_screen_buf;          // Tela em montagem
static ScreenBuf s_shown;               // Última tela enviada
static char      s_out[SCREEN_BYTES + 1024];   // Tela + códigos de cursor
static bool      s_full_redraw = true;  // Próximo envio limpa o terminal
static int       s_cursor_row = -1;     // Cursor visível (campo de texto) ou -1
static int       s_cursor_col = 0;

/* Quantidade de caracteres visíveis (UTF-8 conta uma vez). */
static int visible_len(const char *s, int bytes) {
    int n = 0;
    for (int i = 0; i < bytes && s[i]; i++) {
        if (((unsigned char)s[i] & 0xC0) != 0x80) n++;
    }
    return n;
}

static void buf_reset(void) {
    s_screen_buf.len = 0;
    s_screen_buf.lines = 0;
    s_cursor_row = -1;
}

/* Acrescenta uma linha: estilo + texto (+ preenchimento até a largura). */
static void vline(const char *style, bool pad, const char *fmt, va_list args) {
    ScreenBuf *b = &s_screen_buf;
    if (b->lines >= MAX_LINES) return;
    char text[320];
    vsnprintf(text, sizeof(text), fmt, args);
    int room = SCREEN_BYTES - b->len - 1;
    int pad_n = pad ? WIDTH - visible_len(text, (int)strlen(text)) : 0;
    if (pad_n < 0) pad_n = 0;
    int n = snprintf(b->data + b->len, room, "%s%s%*s%s", style, text, pad_n, "", ST_RESET);
    if (n < 0 || n >= room) return;
    b->start[b->lines++] = b->len;
    b->len += n + 1;                    // Terminador separa as linhas
}

static void line(const char *style, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vline(style, false, fmt, args);
    va_end(args);
}

static void line_padded(const char *style, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vline(style, true, fmt, args);
    va_end(args);
}

static void blank(void) { line("", ""); }

static void rule(void) {
    char bar[WIDTH * 3 + 1];                            // "─" ocupa 3 bytes em UTF-8
    for (int i = 0; i < WIDTH; i++) memcpy(bar + i * 3, "─", 3);
    bar[WIDTH * 3] = '\0';
    line(ST_RULE, "%s", bar);
}

/* Texto longo quebrado em linhas de até `width` colunas, com recuo. */
static void wrapped(const char *style, const char *indent, const char *text) {
    int width = WIDTH - visible_len(indent, (int)strlen(indent));
    const char *p = text;
    const char *first = indent;
    char cont[32];
    snprintf(cont, sizeof(cont), "%*s", visible_len(indent, (int)strlen(indent)), "");
    while (*p) {
        int cols = 0, last_space = -1, i = 0;
        while (p[i] && cols < width) {
            if (p[i] == ' ') last_space = i;
            if (((unsigned char)p[i] & 0xC0) != 0x80) cols++;
            i++;
        }
        while (p[i] && ((unsigned char)p[i] & 0xC0) == 0x80) i++;   // Não corta um caractere
        int cut = (p[i] && last_space > 0) ? last_space : i;
        line(style, "%s%.*s", first, cut, p);
        p += cut;
        while (*p == ' ') p++;
        first = cont;
    }
}

static const char *line_at(const ScreenBuf *b, int i) {
    return b->data + b->start[i];
}

/* Envia a tela: completa (limpando) ou somente as linhas que mudaram. */
static void flush(void) {
    ScreenBuf *b = &s_screen_buf;
    int o = 0;
    const int cap = (int)sizeof(s_out) - 64;
#if SERIAL_UI_ANSI
    if (s_full_redraw) {
        o += snprintf(s_out + o, cap - o, "\033[?25l\033[2J\033[H");
        for (int i = 0; i < b->lines && o < cap; i++) {
            o += snprintf(s_out + o, cap - o, "%s\033[K\r\n", line_at(b, i));
        }
        o += snprintf(s_out + o, cap - o, "\033[J");
    } else {
        for (int i = 0; i < b->lines && o < cap; i++) {
            if (i < s_shown.lines && strcmp(line_at(b, i), line_at(&s_shown, i)) == 0) continue;
            o += snprintf(s_out + o, cap - o, "\033[%d;1H%s\033[K", i + 1, line_at(b, i));
        }
        if (b->lines < s_shown.lines && o < cap) {
            o += snprintf(s_out + o, cap - o, "\033[%d;1H\033[J", b->lines + 1);
        }
    }
    if (o < cap) {
        if (s_cursor_row >= 0) {
            o += snprintf(s_out + o, cap - o, "\033[%d;%dH\033[?25h", s_cursor_row + 1,
                          s_cursor_col + 1);
        } else {
            o += snprintf(s_out + o, cap - o, "\033[?25l\033[%d;1H", b->lines + 1);
        }
    }
    if (o > cap) o = cap;
    bool changed = s_full_redraw || b->lines != s_shown.lines ||
                   memcmp(b->data, s_shown.data, (size_t)b->len) != 0;
    if (changed || s_cursor_row >= 0) {
        s_out[o] = '\0';
        hal_console_write(s_out);
    }
#else
    // Terminal sem ANSI: reimprime a tela inteira somente quando ela muda
    bool changed = s_full_redraw || b->lines != s_shown.lines ||
                   memcmp(b->data, s_shown.data, (size_t)b->len) != 0;
    if (!changed) return;
    o += snprintf(s_out + o, cap - o, "\r\n\r\n");
    for (int i = 0; i < b->lines && o < cap; i++) {
        o += snprintf(s_out + o, cap - o, "%s\r\n", line_at(b, i));
    }
    if (o > cap) o = cap;
    s_out[o] = '\0';
    hal_console_write(s_out);
#endif
    memcpy(&s_shown, b, sizeof(ScreenBuf));
    s_full_redraw = false;
}

// ======================= TECLADO =============================================

typedef enum { K_NONE, K_UP, K_DOWN, K_LEFT, K_RIGHT, K_ENTER, K_ESC, K_BKSP, K_TAB, K_CHAR } KeyKind;

typedef struct {
    KeyKind kind;
    char ch;
} Key;

static int64_t s_last_cr_us = 0;

static Key read_key(uint32_t timeout_ms) {
    Key k = {K_NONE, 0};
    uint8_t c;
    if (!hal_console_read_byte(&c, timeout_ms)) return k;
    int64_t now = hal_time_us();
    if (c == 0x1B) {                                    // ESC ou sequência de seta
        uint8_t c2;
        if (!hal_console_read_byte(&c2, 30)) { k.kind = K_ESC; return k; }
        if (c2 != '[' && c2 != 'O') { k.kind = K_ESC; return k; }
        uint8_t c3;
        if (!hal_console_read_byte(&c3, 30)) return k;
        while ((c3 >= '0' && c3 <= '9') || c3 == ';') { // Parâmetros (ex.: ESC [ 1 ; 5 A)
            if (!hal_console_read_byte(&c3, 30)) return k;
        }
        switch (c3) {
            case 'A': k.kind = K_UP; break;
            case 'B': k.kind = K_DOWN; break;
            case 'C': k.kind = K_RIGHT; break;
            case 'D': k.kind = K_LEFT; break;
            default: break;
        }
        return k;
    }
    if (c == '\r') {
        s_last_cr_us = now;
        k.kind = K_ENTER;
    } else if (c == '\n') {
        if (now - s_last_cr_us > 100000) k.kind = K_ENTER;  // LF sozinho; CR+LF conta uma vez
    } else if (c == 0x08 || c == 0x7F) {
        k.kind = K_BKSP;
    } else if (c == '\t') {
        k.kind = K_TAB;
    } else if (c >= 0x20) {
        k.kind = K_CHAR;
        k.ch = (char)c;
    }
    return k;
}

// ======================= ESTADO DO MENU ======================================

typedef enum {
    SCR_MAIN,
    SCR_WIFI_MENU,
    SCR_AUDIO_MENU,
    SCR_STATUS,
    SCR_WIFI_LIST,
    SCR_WIFI_DETAIL,
    SCR_INPUT,
    SCR_TUNER,
    SCR_NOTES,
    SCR_LOG,
    SCR_CONFIRM_RESTART,
} Screen;

typedef enum { INPUT_SSID, INPUT_PASSWORD } InputPurpose;

typedef struct {
    const char *text;                   // Rótulo fixo
    const char *(*dynamic)(void);       // Ou rótulo calculado
    void (*action)(void);
    bool submenu;
} MenuItem;

static Screen   s_screen = SCR_MAIN;
static int      s_sel_main = 0, s_sel_wifi = 0, s_sel_audio = 0;
static bool     s_interacted = false;   // Houve tecla desde o último reinício
static int64_t  s_last_key_us = 0;
static int64_t  s_swallow_enter_until = 0;
static char     s_notice[96] = "";      // Mensagem curta acima do rodapé

static struct {
    InputPurpose purpose;
    char text[HAL_PASS_MAX + 1];
    int  len;
    int  max;
    bool reveal;
    char error[96];
} s_in;

static char        s_pending_ssid[HAL_SSID_MAX + 1];
static DrvWifiAuth s_pending_auth = DRV_WIFI_AUTH_OTHER;
static bool        s_pending_auth_known = false;

#define MAX_NETS 20
static DrvWifiAp s_nets[MAX_NETS];
static int       s_net_count = 0;
static int       s_list_sel = 0;

static void set_notice(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(s_notice, sizeof(s_notice), fmt, args);
    va_end(args);
}

static void go(Screen screen) {
    s_screen = screen;
    s_full_redraw = true;
}

static void reset_menu(void) {
    s_screen = SCR_MAIN;
    s_sel_main = s_sel_wifi = s_sel_audio = 0;
    memset(&s_in, 0, sizeof(s_in));
    s_interacted = false;
    s_full_redraw = true;
    set_notice("Menu reiniciado após %d s sem interação.", SERIAL_UI_IDLE_RESET_MS / 1000);
}

// ======================= CABEÇALHO E PAINÉIS ================================

static void format_uptime(uint32_t s, char *buf, int len) {
    snprintf(buf, len, "%02" PRIu32 ":%02" PRIu32 ":%02" PRIu32, s / 3600, (s / 60) % 60, s % 60);
}

static void render_header(void) {
    line_padded(ST_BANNER, "  PARTITURA IoT  ·  firmware %s  ·  %s", FIRMWARE_VERSION,
                hal_wifi_device_name());

    HalWifiDiag d;
    hal_wifi_get_diag(&d);
    char ip[24];
    hal_wifi_ip_str(ip, sizeof(ip));
    switch (d.state) {
        case HAL_WIFI_CONNECTED:
            line(ST_TEXT, " Wi-Fi   " ST_TITLE "conectado" ST_TEXT " a \"%s\"  ·  IP %s  ·  %d dBm",
                 d.ssid, ip, hal_wifi_rssi());
            break;
        case HAL_WIFI_AP_ONLY:
            line(ST_TEXT, " Wi-Fi   " ST_ERROR "rede própria" ST_TEXT " \"%s\"  ·  IP %s",
                 hal_wifi_device_name(), ip);
            break;
        case HAL_WIFI_CONNECTING:
            line(ST_TEXT, " Wi-Fi   " ST_KEY "conectando" ST_TEXT " a \"%s\"...", d.ssid);
            break;
    }

    NetPeer peer;
    char where[40] = "";
    if (app_state_get_peer(&peer)) hal_net_peer_str(&peer, where, sizeof(where));
    if (app_state_is_paired()) {
        line(ST_TEXT, " App     " ST_TITLE "\"%s\"" ST_TEXT " conectado (%s)", app_state_peer_name(),
             where);
    } else {
        line(ST_TEXT, " App     " ST_SOFT "aguardando conexão");
    }

    MicStatus mic = audio_pipeline_mic_status();
    bool mic_ok = mic == MIC_STATUS_OK || mic == MIC_STATUS_STARTING;
    SessionInfo s = app_state_session();
    char session[48];
    if (!s.active) {
        snprintf(session, sizeof(session), ST_SOFT "parada");
    } else if (s.local) {
        snprintf(session, sizeof(session), ST_TITLE "captura local");
    } else {
        snprintf(session, sizeof(session), ST_TITLE "execução do app (%u BPM)", s.bpm);
    }
    line(ST_TEXT, " Mic     %s%s" ST_TEXT "    Sessão  %s", mic_ok ? ST_TITLE : ST_ERROR,
         diagnostics_mic_text(mic), session);
    rule();
}

/* Problemas atuais e o que fazer. */
static void render_problems(int max_problems) {
    DiagProblem problems[3];
    int n = diagnostics_collect(problems, max_problems < 3 ? max_problems : 3);
    for (int i = 0; i < n; i++) {
        DiagProblem *p = &problems[i];
        bool bad = p->level == UI_EV_ERROR || p->level == UI_EV_WARN;
        line(bad ? ST_ERROR : ST_TITLE, " %s %s", bad ? "!" : "i", p->title);
        for (int a = 0; a < p->n_actions; a++) wrapped(ST_TEXT, "   > ", p->actions[a]);
    }
    if (n > 0) rule();
}

static void render_events(int max) {
    UiEvent ev[4];
    int n = ui_events_recent(ev, max < 4 ? max : 4);
    if (n == 0) return;
    line(ST_SOFT, " Últimos avisos");
    for (int i = 0; i < n; i++) {
        char t[12];
        format_uptime(ev[i].time_s, t, sizeof(t));
        bool bad = ev[i].level == UI_EV_ERROR || ev[i].level == UI_EV_WARN;
        line(bad ? ST_ERROR : ST_TEXT, "  [%s] %s", t, ev[i].text);
    }
    rule();
}

static void render_footer(const char *keys) {
    if (s_notice[0]) line(ST_KEY, " %s", s_notice);
    line(ST_SOFT, " %s", keys);
    line(ST_SOFT, " Sem tecla por %d s, o menu volta ao início.", SERIAL_UI_IDLE_RESET_MS / 1000);
}

// ======================= MENUS ===============================================

static void act_status(void) { go(SCR_STATUS); }
static void act_wifi_menu(void) { go(SCR_WIFI_MENU); }
static void act_audio_menu(void) { go(SCR_AUDIO_MENU); }
static void act_log(void) { go(SCR_LOG); }
static void act_restart(void) { go(SCR_CONFIRM_RESTART); }

static void start_input(InputPurpose purpose) {
    memset(&s_in, 0, sizeof(s_in));
    s_in.purpose = purpose;
    s_in.max = purpose == INPUT_SSID ? HAL_SSID_MAX : HAL_PASS_MAX;
    go(SCR_INPUT);
}

static bool app_session_running(void) {
    SessionInfo s = app_state_session();
    return s.active && !s.local;
}

static void act_scan(void) {
    if (app_session_running()) {
        set_notice("Encerre a execução no app antes de procurar redes.");
        return;
    }
    // Mostra o aviso antes da varredura (bloqueia ~2–3 s)
    buf_reset();
    render_header();
    blank();
    line(ST_TITLE, " Procurando redes Wi-Fi (2,4 GHz)... aguarde.");
    flush();
    s_net_count = hal_wifi_scan(s_nets, MAX_NETS);
    if (s_net_count < 0) s_net_count = 0;
    s_list_sel = 0;
    go(SCR_WIFI_LIST);
}

static void act_type_ssid(void) {
    s_pending_auth_known = false;
    start_input(INPUT_SSID);
}

static void act_reconnect(void) {
    WifiCredentials cred;
    hal_storage_load_wifi(&cred);
    hal_wifi_reconnect(&cred);
    ui_event(UI_EV_INFO, "Nova tentativa de conexão com \"%s\"", cred.ssid);
    go(SCR_WIFI_DETAIL);
}

static void act_wifi_detail(void) { go(SCR_WIFI_DETAIL); }

static const char *label_capture(void) {
    SessionInfo s = app_state_session();
    return s.active && s.local ? "Parar captura local" : "Iniciar captura local (teste sem o app)";
}

static void act_capture(void) {
    SessionInfo s = app_state_session();
    if (s.active && !s.local) {
        set_notice("O app está em execução: a captura já está ativa.");
    } else if (s.active) {
        app_state_session_stop();
    } else {
        app_state_session_start(100, 4, 0, app_state_flags(), true, 0);
        go(SCR_NOTES);
    }
}

static void act_notes(void) { go(SCR_NOTES); }
static void act_tuner(void) { go(SCR_TUNER); }

static void act_metronome(void) {
    if (app_state_session().active) {
        set_notice("Pare a captura/execução antes de testar o metrônomo.");
        return;
    }
    metronome_test(100, 4);
    set_notice("Metrônomo de teste: 2 compassos a 100 BPM (buzzer + LED RGB).");
}

static const char *label_raw_audio(void) {
    return (app_state_flags() & CFG_FLAG_STREAM_AUDIO) ? "Envio de áudio bruto: LIGADO"
                                                       : "Envio de áudio bruto: desligado";
}

static void act_raw_audio(void) {
    app_state_set_flags(app_state_flags() ^ CFG_FLAG_STREAM_AUDIO);
}

static const MenuItem MAIN_ITEMS[] = {
    {"Status do sistema", NULL, act_status, false},
    {"Rede Wi-Fi", NULL, act_wifi_menu, true},
    {"Captura e áudio", NULL, act_audio_menu, true},
    {"Log técnico", NULL, act_log, false},
    {"Reiniciar dispositivo", NULL, act_restart, false},
};

static const MenuItem WIFI_ITEMS[] = {
    {"Escolher rede da lista", NULL, act_scan, false},
    {"Digitar o nome da rede (rede oculta)", NULL, act_type_ssid, false},
    {"Tentar conectar de novo", NULL, act_reconnect, false},
    {"Detalhes da conexão", NULL, act_wifi_detail, false},
};

static const MenuItem AUDIO_ITEMS[] = {
    {NULL, label_capture, act_capture, false},
    {"Monitor de notas", NULL, act_notes, false},
    {"Afinador e nível do microfone", NULL, act_tuner, false},
    {"Testar metrônomo (2 compassos)", NULL, act_metronome, false},
    {NULL, label_raw_audio, act_raw_audio, false},
};

#define COUNT(a) ((int)(sizeof(a) / sizeof((a)[0])))

static void render_menu(const char *title, const MenuItem *items, int n, int sel) {
    line(ST_TITLE, " %s", title);
    for (int i = 0; i < n; i++) {
        const char *label = items[i].dynamic ? items[i].dynamic() : items[i].text;
        if (i == sel) {
            line_padded(ST_SELECTED, "  ▸ %d  %s%s", i + 1, label, items[i].submenu ? "  ›" : "");
        } else {
            line(ST_TEXT, "    " ST_KEY "%d" ST_TEXT "  %s%s", i + 1, label,
                 items[i].submenu ? "  ›" : "");
        }
    }
    blank();
}

/* Trata uma tecla em uma tela de menu. */
static void menu_key(const MenuItem *items, int n, int *sel, Key k, Screen parent, bool has_parent) {
    switch (k.kind) {
        case K_UP:    *sel = (*sel + n - 1) % n; break;
        case K_DOWN:  *sel = (*sel + 1) % n; break;
        case K_ENTER:
        case K_RIGHT: items[*sel].action(); break;
        case K_ESC:
        case K_LEFT:
        case K_BKSP:  if (has_parent) go(parent); break;
        case K_CHAR:
            if (k.ch >= '1' && k.ch < '1' + n) {
                *sel = k.ch - '1';
                // Terminais que enviam a linha inteira ("2\r\n"): o ENTER que vem
                // logo depois do número não pode escolher a opção da próxima tela
                s_swallow_enter_until = hal_time_us() + 300000;
                items[*sel].action();
            } else if (k.ch == '0' && has_parent) {
                go(parent);
            }
            break;
        default: break;
    }
}

// ======================= TELAS ===============================================

static void render_main(void) {
    render_problems(2);
    render_events(3);
    render_menu("MENU PRINCIPAL", MAIN_ITEMS, COUNT(MAIN_ITEMS), s_sel_main);
    render_footer("↑/↓ mover · ENTER escolher · 1-5 atalho");
}

static void render_wifi_menu(void) {
    render_problems(1);
    render_menu("REDE WI-FI", WIFI_ITEMS, COUNT(WIFI_ITEMS), s_sel_wifi);
    render_footer("↑/↓ mover · ENTER escolher · 1-4 atalho · ESC voltar");
}

static void render_audio_menu(void) {
    render_problems(1);
    render_menu("CAPTURA E ÁUDIO", AUDIO_ITEMS, COUNT(AUDIO_ITEMS), s_sel_audio);
    render_footer("↑/↓ mover · ENTER escolher · 1-5 atalho · ESC voltar");
}

static void render_status(void) {
    char ip[24];
    hal_wifi_ip_str(ip, sizeof(ip));
    SessionInfo s = app_state_session();
    uint8_t flags = app_state_flags();
    char up[12];
    format_uptime((uint32_t)(hal_time_us() / 1000000), up, sizeof(up));
    line(ST_TITLE, " STATUS DO SISTEMA" ST_SOFT "   (atualiza sozinho)");
    line(ST_TEXT, " Ligado há:          %s", up);
    line(ST_TEXT, " IP:                 %s", ip);
    line(ST_TEXT, " Sessão:             %s%s", s.active ? "ATIVA" : "parada",
         s.active && s.local ? " (captura local)" : "");
    line(ST_TEXT, " Metrônomo:          som %s · luz %s",
         (flags & CFG_FLAG_METRO_SOUND) ? "ligado" : "desligado",
         (flags & CFG_FLAG_METRO_VISUAL) ? "ligada" : "desligada");
    line(ST_TEXT, " Envio de altura:    %s · áudio bruto %s",
         (flags & CFG_FLAG_STREAM_PITCH) ? "ligado" : "desligado",
         (flags & CFG_FLAG_STREAM_AUDIO) ? "ligado" : "desligado");
    line(ST_TEXT, " Notas detectadas:   %" PRIu32, app_state_notes_detected());
    line(ST_TEXT, " Pacotes enviados:   %" PRIu32 " (erros: %" PRIu32 ")",
         link_service_packets_sent(), link_service_send_errors());
    line(ST_TEXT, " Eventos perdidos:   %" PRIu32 " (altura/áudio descartados: %" PRIu32 ")",
         app_state_dropped_events(), app_state_dropped_stream());
    line(ST_TEXT, " Frames no buffer:   %d de %d (descartados: %" PRIu32 ")",
         circ_buffer_count(app_state_audio_buffer()), CIRC_BUFFER_CAPACITY,
         app_state_audio_buffer()->overruns);
    line(ST_TEXT, " Nível de entrada:   %.1f dBFS (piso de ruído %.1f dBFS)",
         audio_pipeline_level_db(), app_state_noise_floor());
    line(ST_TEXT, " Processamento:      %" PRIu32 " us por janela de %d ms",
         audio_pipeline_process_time_us(), HOP_SAMPLES * 1000 / SAMPLE_RATE);
    line(ST_TEXT, " Memória livre:      %" PRIu32 " bytes", hal_system_free_heap());
    rule();
    render_problems(2);
    render_footer("ESC volta ao menu");
}

static void render_wifi_detail(void) {
    HalWifiDiag d;
    hal_wifi_get_diag(&d);
    char ip[24], err[96];
    hal_wifi_ip_str(ip, sizeof(ip));
    diagnostics_wifi_error_text(&d, err, sizeof(err));
    uint8_t mac[6];
    hal_wifi_mac(mac);
    static const char *states[] = {"conectando", "CONECTADO", "rede própria (modo AP)"};
    line(ST_TITLE, " DETALHES DA CONEXÃO WI-FI" ST_SOFT "   (atualiza sozinho)");
    line(ST_TEXT, " Rede configurada:   %s", d.ssid[0] ? d.ssid : "(nenhuma)");
    line(ST_TEXT, " Situação:           %s%s", states[d.state],
         d.scanning ? " · procurando redes" : "");
    line(ST_TEXT, " IP:                 %s", ip);
    if (d.state == HAL_WIFI_CONNECTED) {
        line(ST_TEXT, " Sinal:              %d dBm", hal_wifi_rssi());
    } else if (d.ssid_seen) {
        line(ST_TEXT, " Na última busca:    %d dBm · canal %u · %s", d.ssid_rssi, d.ssid_channel,
             diagnostics_auth_name(d.ssid_auth));
    } else if (d.ssid[0]) {
        line(ST_ERROR, " Na última busca:    rede NÃO encontrada");
    }
    if (d.error != HAL_WIFI_ERR_NONE) {
        line(ST_ERROR, " Última falha:       %s (código %u)", err, d.reason);
    }
    if (d.state != HAL_WIFI_CONNECTED && d.error != HAL_WIFI_ERR_NOT_CONFIGURED) {
        line(ST_TEXT, " Tentativa:          %u de %u%s", d.attempt, d.max_attempts,
             d.state == HAL_WIFI_AP_ONLY ? " (depois, a cada 15 s)" : "");
    }
    if (d.state == HAL_WIFI_AP_ONLY) {
        line(ST_TEXT, " Rede própria:       \"%s\" · senha %s · %u aparelho(s)",
             hal_wifi_device_name(), WIFI_AP_PASS, d.ap_clients);
    }
    line(ST_TEXT, " MAC do dispositivo: %02X:%02X:%02X:%02X:%02X:%02X · país %s", mac[0], mac[1],
         mac[2], mac[3], mac[4], mac[5], WIFI_COUNTRY_CODE);
    rule();
    render_problems(1);
    render_footer("ESC volta ao menu Wi-Fi");
}

static void render_wifi_list(void) {
    line(ST_TITLE, " ESCOLHA A REDE" ST_SOFT "   (somente redes de 2,4 GHz aparecem)");
    if (s_net_count == 0) line(ST_ERROR, " Nenhuma rede encontrada. Aproxime o dispositivo do modem.");
    HalWifiDiag d;
    hal_wifi_get_diag(&d);
    int total = s_net_count + 1;                        // + "digitar outro nome"
    const int window = 12;
    int top = s_list_sel - window + 1;
    if (top < 0) top = 0;
    for (int i = top; i < total && i < top + window; i++) {
        char text[96];
        if (i < s_net_count) {
            const DrvWifiAp *n = &s_nets[i];
            const char *sig = n->rssi >= -60 ? "ótimo" : n->rssi >= -70 ? "bom"
                            : n->rssi >= -80 ? "fraco" : "muito fraco";
            snprintf(text, sizeof(text), "%-24.24s %4d dBm %-11s %-9s%s", n->ssid, n->rssi, sig,
                     diagnostics_auth_name(n->auth),
                     strcmp(n->ssid, d.ssid) == 0 ? " ← atual" : "");
        } else {
            snprintf(text, sizeof(text), "Outra rede: digitar o nome");
        }
        if (i == s_list_sel) {
            line_padded(ST_SELECTED, "  ▸ %2d  %s", i + 1, text);
        } else {
            line(ST_TEXT, "    " ST_KEY "%2d" ST_TEXT "  %s", i + 1, text);
        }
    }
    if (total > window) line(ST_SOFT, "    (%d redes – use ↑/↓ para ver todas)", s_net_count);
    blank();
    render_footer("↑/↓ mover · ENTER escolher · ESC voltar");
}

static void render_input(void) {
    bool secret = s_in.purpose == INPUT_PASSWORD;
    if (secret) {
        line(ST_TITLE, " SENHA DA REDE \"%s\"%s%s%s", s_pending_ssid,
             s_pending_auth_known ? " (" : "",
             s_pending_auth_known ? diagnostics_auth_name(s_pending_auth) : "",
             s_pending_auth_known ? ")" : "");
        line(ST_SOFT, " Maiúsculas e minúsculas contam. TAB mostra/oculta a senha.");
        if (!s_pending_auth_known) line(ST_SOFT, " Rede aberta (sem senha)? Deixe vazio e tecle ENTER.");
    } else {
        line(ST_TITLE, " NOME DA REDE (SSID)");
        line(ST_SOFT, " Digite exatamente como aparece no celular (2,4 GHz).");
    }
    line(ST_SOFT, " ENTER confirma · ESC cancela · %d s sem tecla cancela",
         SERIAL_UI_IDLE_RESET_MS / 1000);
    blank();
    if (s_in.error[0]) line(ST_ERROR, " %s", s_in.error);
    else blank();

    // Campo (última linha da tela, onde fica o cursor)
    char shown[HAL_PASS_MAX * 3 + 1];
    int chars = visible_len(s_in.text, s_in.len);
    if (secret && !s_in.reveal) {
        int i;
        for (i = 0; i < chars && i < HAL_PASS_MAX; i++) shown[i] = '*';
        shown[i] = '\0';
    } else {
        memcpy(shown, s_in.text, (size_t)s_in.len);
        shown[s_in.len] = '\0';
    }
    const char *prompt = secret ? " Senha: " : " Nome: ";
    line(ST_TITLE, "%s" ST_TEXT "%s", prompt, shown);
    s_cursor_row = s_screen_buf.lines - 1;
    s_cursor_col = visible_len(prompt, (int)strlen(prompt)) + chars;
}

static void render_tuner(void) {
    static const char *names[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    float hz = audio_pipeline_last_pitch_hz();
    float level = audio_pipeline_level_db();
    line(ST_TITLE, " AFINADOR E NÍVEL DO MICROFONE" ST_SOFT "   (ao vivo)");
    blank();
    if (hz > 0) {
        float midi_f = dsp_hz_to_midi(hz);
        int midi = (int)(midi_f + 0.5f);
        int cents = (int)((midi_f - midi) * 100.0f);
        line(ST_TITLE, "   Nota  %s%d  (MIDI %d)    %.1f Hz    %+d cents", names[midi % 12],
             midi / 12 - 1, midi, hz, cents);
        char bar[64];
        int pos = 10 + cents / 5;                       // ±50 cents em 21 posições
        if (pos < 0) pos = 0;
        if (pos > 20) pos = 20;
        for (int i = 0; i < 21; i++) bar[i] = i == 10 ? '|' : '-';
        bar[21] = '\0';
        line(ST_TEXT, "   Afinação  [%.*s" ST_KEY "#" ST_TEXT "%s]", pos, bar, bar + pos + 1);
    } else {
        line(ST_SOFT, "   Nota  ---  (toque uma nota no teclado)");
        blank();
    }
    char meter[48];
    int filled = (int)((level + 90.0f) / 90.0f * 40.0f);    // -90..0 dBFS
    if (filled < 0) filled = 0;
    if (filled > 40) filled = 40;
    memset(meter, '#', (size_t)filled);
    memset(meter + filled, '.', (size_t)(40 - filled));
    meter[40] = '\0';
    line(ST_TEXT, "   Nível     [%s] %.1f dBFS", meter, level);
    line(ST_SOFT, "   Piso de ruído %.1f dBFS · microfone: %s", app_state_noise_floor(),
         diagnostics_mic_text(audio_pipeline_mic_status()));
    blank();
    render_problems(1);
    render_footer("ESC volta");
}

static void render_notes(void) {
    static const char *names[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    SessionInfo s = app_state_session();
    line(ST_TITLE, " MONITOR DE NOTAS" ST_SOFT "   (ao vivo)");
    if (!s.active) {
        line(ST_KEY, " Captura parada: ENTER inicia a captura local (100 BPM, 4 tempos).");
    } else if (s.local) {
        line(ST_TEXT, " Captura local ativa: toque no teclado. ENTER para a captura.");
    } else {
        line(ST_TEXT, " Execução do app em andamento: notas enviadas ao app.");
    }
    blank();
    NoteEvent notes[8];
    int n = audio_pipeline_recent_notes(notes, 8, NULL);
    if (n == 0) line(ST_SOFT, "   (nenhuma nota detectada ainda)");
    for (int i = 0; i < n; i++) {
        const NoteEvent *e = &notes[i];
        if (e->type == NOTE_EVENT_ON) {
            line(i == 0 ? ST_TITLE : ST_TEXT, "   ON   %-3s%d  %7.1f Hz  t=%6" PRIu32 " ms  confiança %.2f",
                 names[e->midi % 12], e->midi / 12 - 1, e->frequency_hz, e->time_ms, e->confidence);
        } else {
            line(i == 0 ? ST_TITLE : ST_SOFT, "   OFF  %-3s%d  duração %" PRIu32 " ms",
                 names[e->midi % 12], e->midi / 12 - 1, e->duration_ms);
        }
    }
    blank();
    render_problems(1);
    render_footer("ENTER inicia/para · ESC volta");
}

static void render_log(void) {
    static char lines[20][96];
    int n = hal_console_log_lines(lines, 20);
    line(ST_TITLE, " LOG TÉCNICO" ST_SOFT "   (últimas %d linhas do ESP_LOG)", n);
    for (int i = 0; i < n; i++) {
        char c = lines[i][0];
        line(c == 'E' || c == 'W' ? ST_ERROR : ST_SOFT, " %.*s", WIDTH + 10, lines[i]);
    }
    blank();
    render_footer("ESC volta ao menu");
}

static void render_confirm_restart(void) {
    line(ST_TITLE, " REINICIAR O DISPOSITIVO?");
    line(ST_TEXT, " O app será desconectado e a execução em andamento, encerrada.");
    blank();
    line(ST_KEY, " ENTER = reiniciar · ESC = cancelar");
}

static void render(void) {
    buf_reset();
    render_header();
    switch (s_screen) {
        case SCR_MAIN:            render_main(); break;
        case SCR_WIFI_MENU:       render_wifi_menu(); break;
        case SCR_AUDIO_MENU:      render_audio_menu(); break;
        case SCR_STATUS:          render_status(); break;
        case SCR_WIFI_LIST:       render_wifi_list(); break;
        case SCR_WIFI_DETAIL:     render_wifi_detail(); break;
        case SCR_INPUT:           render_input(); break;
        case SCR_TUNER:           render_tuner(); break;
        case SCR_NOTES:           render_notes(); break;
        case SCR_LOG:             render_log(); break;
        case SCR_CONFIRM_RESTART: render_confirm_restart(); break;
    }
    flush();
}

// ======================= CAMPOS DE TEXTO =====================================

static void save_wifi(const char *ssid, const char *password) {
    WifiCredentials cred;
    memset(&cred, 0, sizeof(cred));
    strncpy(cred.ssid, ssid, HAL_SSID_MAX);
    strncpy(cred.password, password, HAL_PASS_MAX);
    if (!hal_storage_save_wifi(&cred)) {
        ui_event(UI_EV_WARN, "Não foi possível salvar a rede na memória (NVS)");
    }
    ui_event(UI_EV_INFO, "Rede \"%s\" salva. Conectando...", cred.ssid);
    hal_wifi_reconnect(&cred);
    memset(&s_in, 0, sizeof(s_in));                     // Não deixa a senha na RAM do menu
    go(SCR_WIFI_DETAIL);
}

static void input_confirm(void) {
    s_in.text[s_in.len] = '\0';
    if (s_in.purpose == INPUT_SSID) {
        if (s_in.len == 0) {
            snprintf(s_in.error, sizeof(s_in.error), "Digite o nome da rede.");
            return;
        }
        strncpy(s_pending_ssid, s_in.text, HAL_SSID_MAX);
        s_pending_ssid[HAL_SSID_MAX] = '\0';
        start_input(INPUT_PASSWORD);
        return;
    }
    if (hal_wifi_check_password(s_in.text) != HAL_WIFI_ERR_NONE) {
        snprintf(s_in.error, sizeof(s_in.error),
                 "Senha inválida: precisa ter de 8 a 63 caracteres (digitados: %d).",
                 visible_len(s_in.text, s_in.len));
        return;
    }
    if (s_in.len == 0 && s_pending_auth_known && s_pending_auth != DRV_WIFI_AUTH_OPEN) {
        snprintf(s_in.error, sizeof(s_in.error), "Esta rede é protegida: digite a senha.");
        return;
    }
    char password[HAL_PASS_MAX + 1];
    memcpy(password, s_in.text, sizeof(password));
    save_wifi(s_pending_ssid, password);
    memset(password, 0, sizeof(password));
}

static void input_key(Key k) {
    switch (k.kind) {
        case K_ENTER: input_confirm(); break;
        case K_ESC:
            memset(&s_in, 0, sizeof(s_in));
            go(SCR_WIFI_MENU);
            break;
        case K_TAB: s_in.reveal = !s_in.reveal; break;
        case K_BKSP:
            while (s_in.len > 0 && ((unsigned char)s_in.text[s_in.len - 1] & 0xC0) == 0x80) {
                s_in.len--;                             // Bytes de continuação UTF-8
            }
            if (s_in.len > 0) s_in.len--;
            s_in.text[s_in.len] = '\0';
            s_in.error[0] = '\0';
            break;
        case K_CHAR:
            if (s_in.len < s_in.max) {
                s_in.text[s_in.len++] = k.ch;
                s_in.text[s_in.len] = '\0';
            } else {
                snprintf(s_in.error, sizeof(s_in.error), "Limite de %d caracteres.", s_in.max);
            }
            if (s_in.error[0] && s_in.len < s_in.max) s_in.error[0] = '\0';
            break;
        default: break;
    }
}

// ======================= TECLAS POR TELA =====================================

/* Rede escolhida na lista: pede a senha (ou conecta direto se for aberta). */
static void list_choose(void) {
    if (s_list_sel >= s_net_count) {
        act_type_ssid();
        return;
    }
    const DrvWifiAp *n = &s_nets[s_list_sel];
    strncpy(s_pending_ssid, n->ssid, HAL_SSID_MAX);
    s_pending_ssid[HAL_SSID_MAX] = '\0';
    s_pending_auth = n->auth;
    s_pending_auth_known = true;
    if (n->auth == DRV_WIFI_AUTH_OPEN) {
        save_wifi(s_pending_ssid, "");
    } else {
        start_input(INPUT_PASSWORD);
    }
}

static void list_key(Key k) {
    int total = s_net_count + 1;
    switch (k.kind) {
        case K_UP:   s_list_sel = (s_list_sel + total - 1) % total; break;
        case K_DOWN: s_list_sel = (s_list_sel + 1) % total; break;
        case K_ESC:
        case K_LEFT:
        case K_BKSP: go(SCR_WIFI_MENU); break;
        case K_ENTER:
        case K_RIGHT: list_choose(); break;
        case K_CHAR:
            if (k.ch >= '1' && k.ch <= '9' && k.ch - '1' < total) {
                s_list_sel = k.ch - '1';
                s_swallow_enter_until = hal_time_us() + 300000;
                list_choose();
            }
            break;
        default: break;
    }
}

static void handle_key(Key k) {
    if (k.kind == K_NONE) return;
    // ENTER que chegou junto com o número da opção (terminal em modo linha)
    if (k.kind == K_ENTER && hal_time_us() < s_swallow_enter_until) return;
    s_swallow_enter_until = 0;
    s_notice[0] = '\0';

    switch (s_screen) {
        case SCR_MAIN:
            menu_key(MAIN_ITEMS, COUNT(MAIN_ITEMS), &s_sel_main, k, SCR_MAIN, false);
            break;
        case SCR_WIFI_MENU:
            menu_key(WIFI_ITEMS, COUNT(WIFI_ITEMS), &s_sel_wifi, k, SCR_MAIN, true);
            break;
        case SCR_AUDIO_MENU:
            menu_key(AUDIO_ITEMS, COUNT(AUDIO_ITEMS), &s_sel_audio, k, SCR_MAIN, true);
            break;
        case SCR_WIFI_LIST:
            list_key(k);
            break;
        case SCR_INPUT:
            input_key(k);
            break;
        case SCR_WIFI_DETAIL:
            if (k.kind == K_ESC || k.kind == K_LEFT || k.kind == K_BKSP || k.kind == K_ENTER) {
                go(SCR_WIFI_MENU);
            }
            break;
        case SCR_NOTES:
            if (k.kind == K_ENTER) act_capture();
            else if (k.kind == K_ESC || k.kind == K_LEFT || k.kind == K_BKSP) go(SCR_AUDIO_MENU);
            break;
        case SCR_TUNER:
            if (k.kind != K_UP && k.kind != K_DOWN) go(SCR_AUDIO_MENU);
            break;
        case SCR_STATUS:
        case SCR_LOG:
            if (k.kind != K_UP && k.kind != K_DOWN) go(SCR_MAIN);
            break;
        case SCR_CONFIRM_RESTART:
            if (k.kind == K_ENTER) {
                buf_reset();
                render_header();
                line(ST_TITLE, " Reiniciando...");
                flush();
                app_state_clear_peer();
                vTaskDelay(pdMS_TO_TICKS(300));
                hal_system_restart();
            } else {
                go(SCR_MAIN);
            }
            break;
    }
}

/* Intervalo de atualização automática de cada tela (ms). */
static uint32_t refresh_period_ms(void) {
#if !SERIAL_UI_ANSI
    return s_screen == SCR_TUNER ? 2000 : 10000;    // Sem ANSI cada atualização rola a tela
#else
    switch (s_screen) {
        case SCR_TUNER: return 200;
        case SCR_NOTES:
        case SCR_LOG:
        case SCR_WIFI_DETAIL: return 400;
        case SCR_INPUT: return 1000;
        default: return 700;
    }
#endif
}

// ======================= TAREFA ==============================================

static void ui_task(void *pvParameters) {
    (void)pvParameters;
    s_full_redraw = true;
    int64_t last_render = 0;

    while (1) {
        Key k = read_key(50);
        int64_t now = hal_time_us();
        bool dirty = false;

        if (k.kind != K_NONE) {
            s_interacted = true;
            s_last_key_us = now;
            handle_key(k);
            dirty = true;
        } else if (s_interacted &&
                   now - s_last_key_us >= (int64_t)SERIAL_UI_IDLE_RESET_MS * 1000) {
            reset_menu();                       // 20 s sem tecla após uma interação
            dirty = true;
        }

        if (dirty || now - last_render >= (int64_t)refresh_period_ms() * 1000) {
            render();
            last_render = now;
        }
    }
}

void serial_ui_splash(void) {
    buf_reset();
    line_padded(ST_BANNER, "  PARTITURA IoT  ·  firmware %s", FIRMWARE_VERSION);
    blank();
    line(ST_TEXT, "  Iniciando: microfone, Wi-Fi e comunicação com o app...");
    s_full_redraw = true;
    flush();
}

void serial_ui_fatal(const char *what, const char *action) {
    buf_reset();
    line_padded(ST_BANNER, "  PARTITURA IoT  ·  FALHA NA INICIALIZAÇÃO");
    blank();
    wrapped(ST_ERROR, "  ! ", what);
    blank();
    wrapped(ST_TEXT, "  O que fazer: ", action);
    s_full_redraw = true;
    flush();
}

void serial_ui_start_task(void) {
    // UI: núcleo 0, prioridade 1 (a mais baixa da aplicação)
    xTaskCreatePinnedToCore(ui_task, "ui_task", 6144, NULL, 1, NULL, 0);
}
