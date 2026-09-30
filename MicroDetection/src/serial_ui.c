/*
 * ============================================================================
 * Implementação do menu serial
 * ============================================================================
 */
#include "serial_ui.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/uart.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app_state.h"
#include "audio_pipeline.h"
#include "config.h"
#include "dsp.h"
#include "metronome.h"
#include "note_tracker.h"
#include "protocol.h"
#include "settings.h"
#include "udp_link.h"
#include "wifi_manager.h"

// ======================= FUNÇÕES UART AUXILIARES ==========================

void serial_ui_init(void) {
    uart_config_t uart_cfg = {
        .baud_rate = UART_BAUDRATE,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    uart_param_config(UART_PORT, &uart_cfg);                // Aplica as configurações
    uart_driver_install(UART_PORT, UART_RX_BUF_SIZE,        // Instala o driver UART
                        UART_TX_BUF_SIZE, 0, NULL, 0);
}

/**
 * @brief Função printf-like que envia a string formatada pela UART.
 */
void uart_printf(const char *format, ...) {
    char buf[256];                                      // Buffer para montar a string
    va_list args;
    va_start(args, format);                             // Inicia a lista de argumentos
    vsnprintf(buf, sizeof(buf), format, args);          // Formata a string
    va_end(args);                                       // Finaliza a lista
    uart_write_bytes(UART_PORT, buf, strlen(buf));      // Envia pela UART
}

/**
 * @brief Lê uma linha (até ENTER) com eco. Retorna o tamanho lido
 *        ou -1 se nada foi digitado dentro do tempo limite.
 */
static int uart_readline(char *out, int max_len, uint32_t timeout_ms, bool echo) {
    int len = 0;
    uint32_t waited = 0;
    while (len < max_len - 1) {
        uint8_t c;
        int rd = uart_read_bytes(UART_PORT, &c, 1, pdMS_TO_TICKS(100));
        if (rd <= 0) {
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
            uart_printf("\b \b");
            continue;
        }
        if (echo) uart_write_bytes(UART_PORT, (const char *)&c, 1);
        out[len++] = (char)c;
    }
    out[len] = '\0';
    uart_printf("\n");
    return len;
}

/**
 * @brief Exibe o menu de opções no terminal serial.
 */
static void print_menu(void) {
    uart_printf("\n===== MENU - PARTITURA IoT (%s) =====\n", FIRMWARE_VERSION);
    uart_printf("1 - Iniciar captura local (teste sem o app)\n");
    uart_printf("2 - Parar captura\n");
    uart_printf("3 - Status\n");
    uart_printf("4 - Escanear redes Wi-Fi\n");
    uart_printf("5 - Configurar rede Wi-Fi\n");
    uart_printf("6 - Liga/desliga envio de áudio bruto (diagnóstico)\n");
    uart_printf("7 - Testar metrônomo (2 compassos)\n");
    uart_printf("8 - Afinador (5 s de leitura contínua)\n");
    uart_printf("9 - Reiniciar dispositivo\n");
    uart_printf("0 - Repetir menu\n");
    uart_printf("Escolha: ");
}

static void print_status(void) {
    char ip[32];
    wifi_manager_ip_str(ip, sizeof(ip));
    SessionInfo s = app_state_session();
    uart_printf("Dispositivo:       %s\n", wifi_manager_device_name());
    uart_printf("Wi-Fi:             %s%s, IP %s, RSSI %d dBm\n",
                wifi_manager_is_connected() ? "conectado" : "desconectado",
                wifi_manager_ap_active() ? " (rede própria ativa)" : "", ip, wifi_manager_rssi());
    uart_printf("App pareado:       %s\n",
                app_state_is_paired() ? app_state_peer_name() : "nenhum");
    uart_printf("Sessão:            %s%s\n", s.active ? "ATIVA" : "PARADA",
                s.active && s.local ? " (local)" : "");
    uart_printf("Flags:             0x%02x (som=%d luz=%d áudio=%d altura=%d)\n",
                app_state_flags(), (app_state_flags() & CFG_FLAG_METRO_SOUND) != 0,
                (app_state_flags() & CFG_FLAG_METRO_VISUAL) != 0,
                (app_state_flags() & CFG_FLAG_STREAM_AUDIO) != 0,
                (app_state_flags() & CFG_FLAG_STREAM_PITCH) != 0);
    uart_printf("Notas detectadas:  %" PRIu32 "\n", app_state_notes_detected());
    uart_printf("Pacotes enviados:  %" PRIu32 " (erros: %" PRIu32 ")\n",
                udp_link_packets_sent(), udp_link_send_errors());
    uart_printf("Eventos perdidos:  %" PRIu32 "\n", app_state_dropped_events());
    uart_printf("Frames no buffer:  %d (descartados: %" PRIu32 ")\n",
                circ_buffer_count(app_state_audio_buffer()), app_state_audio_buffer()->overruns);
    uart_printf("Nível de entrada:  %.1f dBFS (piso de ruído %.1f dBFS)\n",
                audio_pipeline_level_db(), app_state_noise_floor());
    uart_printf("Processamento:     %" PRIu32 " us por janela de %d ms\n",
                audio_pipeline_process_time_us(), HOP_SAMPLES * 1000 / SAMPLE_RATE);
    uart_printf("Memória livre:     %" PRIu32 " bytes\n", esp_get_free_heap_size());
}

static void configure_wifi(void) {
    WifiCredentials cred;
    settings_load_wifi(&cred);
    uart_printf("SSID atual: \"%s\"\nNovo SSID: ", cred.ssid);
    char ssid[SETTINGS_SSID_MAX + 1];
    if (uart_readline(ssid, sizeof(ssid), 60000, true) <= 0) {
        uart_printf("Cancelado.\n");
        return;
    }
    uart_printf("Senha: ");
    char pass[SETTINGS_PASS_MAX + 1];
    if (uart_readline(pass, sizeof(pass), 60000, false) < 0) pass[0] = '\0';
    strncpy(cred.ssid, ssid, SETTINGS_SSID_MAX);
    strncpy(cred.password, pass, SETTINGS_PASS_MAX);
    if (settings_save_wifi(&cred)) {
        uart_printf("Credenciais salvas. Conectando a \"%s\"...\n", cred.ssid);
    } else {
        uart_printf("Falha ao salvar na NVS, tentando conectar mesmo assim...\n");
    }
    wifi_manager_reconnect(&cred);
}

static void tuner(void) {
    for (int i = 0; i < 25; i++) {
        float hz = audio_pipeline_last_pitch_hz();
        float level = audio_pipeline_level_db();
        if (hz > 0) {
            static const char *names[12] = {"C", "C#", "D", "D#", "E", "F",
                                            "F#", "G", "G#", "A", "A#", "B"};
            float midi_f = dsp_hz_to_midi(hz);
            int midi = (int)(midi_f + 0.5f);
            int cents = (int)((midi_f - midi) * 100.0f);
            uart_printf("  %7.1f Hz  %s%d (MIDI %d) %+d cents  %.1f dBFS\n", hz,
                        names[midi % 12], midi / 12 - 1, midi, cents, level);
        } else {
            uart_printf("  ---  sem altura definida  %.1f dBFS\n", level);
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

/**
 * @brief Tarefa de interface do usuário (menu serial).
 */
static void ui_task(void *pvParameters) {
    (void)pvParameters;
    vTaskDelay(pdMS_TO_TICKS(2000));            // Aguarda a inicialização do Wi-Fi
    uart_printf("\n*** Sistema Iniciado ***\n");

    while (1) {
        print_menu();                           // Exibe as opções
        char input[8];
        if (uart_readline(input, sizeof(input), 3600000, true) < 0) continue;
        int op = atoi(input);                   // Converte para inteiro
        switch (op) {
            case 1:
                if (!app_state_session().active) {
                    app_state_session_start(100, 4, 0, app_state_flags(), true);
                    uart_printf("Captura INICIADA. As notas aparecerão no log.\n");
                } else {
                    uart_printf("Captura já está ativa.\n");
                }
                break;
            case 2:
                if (app_state_session().active) {
                    app_state_session_stop();
                    uart_printf("Captura PARADA.\n");
                } else {
                    uart_printf("Nenhuma captura em andamento.\n");
                }
                break;
            case 3:
                print_status();
                break;
            case 4:
                uart_printf("Escaneando...\n");
                uart_printf("%d redes encontradas.\n", wifi_manager_scan(uart_printf));
                break;
            case 5:
                configure_wifi();
                break;
            case 6: {
                uint8_t flags = app_state_flags() ^ CFG_FLAG_STREAM_AUDIO;
                app_state_set_flags(flags);
                uart_printf("Envio de áudio bruto %s.\n",
                            (flags & CFG_FLAG_STREAM_AUDIO) ? "LIGADO" : "DESLIGADO");
                break;
            }
            case 7:
                metronome_test(100, 4);
                uart_printf("Metrônomo de teste: 100 BPM, 4 tempos.\n");
                break;
            case 8:
                tuner();
                break;
            case 9:
                uart_printf("Reiniciando...\n");
                vTaskDelay(pdMS_TO_TICKS(200));
                esp_restart();
                break;
            case 0:
                break;
            default:
                uart_printf("Opção inválida.\n");
        }
    }
}

void serial_ui_start_task(void) {
    // UI: núcleo 0, prioridade 1
    xTaskCreatePinnedToCore(ui_task, "ui_task", 4096, NULL, 1, NULL, 0);
}
