/*
 * ============================================================================
 * app/serial_ui – implementação do menu serial
 * ============================================================================
 */
#include "serial_ui.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app_state.h"
#include "audio_pipeline.h"
#include "config.h"
#include "dsp.h"
#include "hal_console.h"
#include "hal_net.h"
#include "hal_storage.h"
#include "hal_system.h"
#include "hal_wifi.h"
#include "link_service.h"
#include "metronome.h"
#include "note_tracker.h"
#include "protocol.h"

/**
 * @brief Exibe o menu de opções no terminal serial.
 */
static void print_menu(void) {
    hal_console_printf("\n===== MENU - PARTITURA IoT (%s) =====\n", FIRMWARE_VERSION);
    hal_console_printf("1 - Iniciar captura local (teste sem o app)\n");
    hal_console_printf("2 - Parar captura\n");
    hal_console_printf("3 - Status\n");
    hal_console_printf("4 - Escanear redes Wi-Fi\n");
    hal_console_printf("5 - Configurar rede Wi-Fi\n");
    hal_console_printf("6 - Liga/desliga envio de áudio bruto (diagnóstico)\n");
    hal_console_printf("7 - Testar metrônomo (2 compassos)\n");
    hal_console_printf("8 - Afinador (5 s de leitura contínua)\n");
    hal_console_printf("9 - Reiniciar dispositivo\n");
    hal_console_printf("0 - Repetir menu\n");
    hal_console_printf("Escolha: ");
}

static void print_status(void) {
    char ip[32];
    hal_wifi_ip_str(ip, sizeof(ip));
    SessionInfo s = app_state_session();
    hal_console_printf("Dispositivo:       %s\n", hal_wifi_device_name());
    hal_console_printf("Wi-Fi:             %s%s, IP %s, RSSI %d dBm\n",
                hal_wifi_is_connected() ? "conectado" : "desconectado",
                hal_wifi_ap_active() ? " (rede própria ativa)" : "", ip, hal_wifi_rssi());
    NetPeer peer;
    char where[40] = "";
    if (app_state_get_peer(&peer)) hal_net_peer_str(&peer, where, sizeof(where));
    hal_console_printf("App pareado:       %s%s%s%s\n",
                       app_state_is_paired() ? app_state_peer_name() : "nenhum",
                       where[0] ? " (" : "", where, where[0] ? ")" : "");
    hal_console_printf("Sessão:            %s%s\n", s.active ? "ATIVA" : "PARADA",
                s.active && s.local ? " (local)" : "");
    hal_console_printf("Flags:             0x%02x (som=%d luz=%d áudio=%d altura=%d)\n",
                app_state_flags(), (app_state_flags() & CFG_FLAG_METRO_SOUND) != 0,
                (app_state_flags() & CFG_FLAG_METRO_VISUAL) != 0,
                (app_state_flags() & CFG_FLAG_STREAM_AUDIO) != 0,
                (app_state_flags() & CFG_FLAG_STREAM_PITCH) != 0);
    hal_console_printf("Notas detectadas:  %" PRIu32 "\n", app_state_notes_detected());
    hal_console_printf("Pacotes enviados:  %" PRIu32 " (erros: %" PRIu32 ")\n",
                link_service_packets_sent(), link_service_send_errors());
    hal_console_printf("Eventos perdidos:  %" PRIu32 "\n", app_state_dropped_events());
    hal_console_printf("Frames no buffer:  %d (descartados: %" PRIu32 ")\n",
                circ_buffer_count(app_state_audio_buffer()), app_state_audio_buffer()->overruns);
    hal_console_printf("Nível de entrada:  %.1f dBFS (piso de ruído %.1f dBFS)\n",
                audio_pipeline_level_db(), app_state_noise_floor());
    hal_console_printf("Processamento:     %" PRIu32 " us por janela de %d ms\n",
                audio_pipeline_process_time_us(), HOP_SAMPLES * 1000 / SAMPLE_RATE);
    hal_console_printf("Memória livre:     %" PRIu32 " bytes\n", hal_system_free_heap());
}

static void configure_wifi(void) {
    WifiCredentials cred;
    hal_storage_load_wifi(&cred);
    hal_console_printf("SSID atual: \"%s\"\nNovo SSID: ", cred.ssid);
    char ssid[HAL_SSID_MAX + 1];
    if (hal_console_readline(ssid, sizeof(ssid), 60000, true) <= 0) {
        hal_console_printf("Cancelado.\n");
        return;
    }
    hal_console_printf("Senha: ");
    char pass[HAL_PASS_MAX + 1];
    if (hal_console_readline(pass, sizeof(pass), 60000, false) < 0) pass[0] = '\0';
    strncpy(cred.ssid, ssid, HAL_SSID_MAX);
    strncpy(cred.password, pass, HAL_PASS_MAX);
    if (hal_storage_save_wifi(&cred)) {
        hal_console_printf("Credenciais salvas. Conectando a \"%s\"...\n", cred.ssid);
    } else {
        hal_console_printf("Falha ao salvar na NVS, tentando conectar mesmo assim...\n");
    }
    hal_wifi_reconnect(&cred);
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
            hal_console_printf("  %7.1f Hz  %s%d (MIDI %d) %+d cents  %.1f dBFS\n", hz,
                        names[midi % 12], midi / 12 - 1, midi, cents, level);
        } else {
            hal_console_printf("  ---  sem altura definida  %.1f dBFS\n", level);
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
    hal_console_printf("\n*** Sistema Iniciado ***\n");

    while (1) {
        print_menu();                           // Exibe as opções
        char input[8];
        if (hal_console_readline(input, sizeof(input), 3600000, true) < 0) continue;
        int op = atoi(input);                   // Converte para inteiro
        switch (op) {
            case 1:
                if (!app_state_session().active) {
                    app_state_session_start(100, 4, 0, app_state_flags(), true);
                    hal_console_printf("Captura INICIADA. As notas aparecerão no log.\n");
                } else {
                    hal_console_printf("Captura já está ativa.\n");
                }
                break;
            case 2:
                if (app_state_session().active) {
                    app_state_session_stop();
                    hal_console_printf("Captura PARADA.\n");
                } else {
                    hal_console_printf("Nenhuma captura em andamento.\n");
                }
                break;
            case 3:
                print_status();
                break;
            case 4:
                hal_console_printf("Escaneando...\n");
                hal_console_printf("%d redes encontradas.\n", hal_wifi_scan(hal_console_printf));
                break;
            case 5:
                configure_wifi();
                break;
            case 6: {
                uint8_t flags = app_state_flags() ^ CFG_FLAG_STREAM_AUDIO;
                app_state_set_flags(flags);
                hal_console_printf("Envio de áudio bruto %s.\n",
                            (flags & CFG_FLAG_STREAM_AUDIO) ? "LIGADO" : "DESLIGADO");
                break;
            }
            case 7:
                metronome_test(100, 4);
                hal_console_printf("Metrônomo de teste: 100 BPM, 4 tempos.\n");
                break;
            case 8:
                tuner();
                break;
            case 9:
                hal_console_printf("Reiniciando...\n");
                vTaskDelay(pdMS_TO_TICKS(200));
                hal_system_restart();
                break;
            case 0:
                break;
            default:
                hal_console_printf("Opção inválida.\n");
        }
    }
}

void serial_ui_start_task(void) {
    // UI: núcleo 0, prioridade 1
    xTaskCreatePinnedToCore(ui_task, "ui_task", 4096, NULL, 1, NULL, 0);
}
