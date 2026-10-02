/*
 * ============================================================================
 * Sistema de Processamento de Áudio com Feedback em Tempo Real
 * Aplicado ao Ensino de Partitura – firmware do dispositivo embarcado
 *
 * Plataformas: esp32doit-devkit-v1 e ESP32-S3-DevKitC-1 (N16R8)
 *
 * Arquitetura em camadas:
 *   app/      lógica da aplicação (fluxo de controle, protocolo, DSP, menu)
 *   hal/      abstração de hardware (funções simples e reutilizáveis)
 *   drivers/  controle direto dos periféricos (GPIO, PWM, I2S, UART, NVS, Wi-Fi)
 *   include/  cabeçalhos públicos de cada camada
 *
 * Tarefas FreeRTOS:
 *  - audio_prod  (núcleo 1, prioridade 5): microfone -> DSP -> eventos
 *  - net_tx      (núcleo 0, prioridade 4): filas de eventos -> UDP/WebSocket
 *  - net_rx      (núcleo 0, prioridade 3): comandos do app via UDP
 *  - httpd       (núcleo 0): comandos do app web via WebSocket
 *  - ui_task     (núcleo 0, prioridade 1): menu serial
 *  - status_led  (núcleo 0, prioridade 1): animação do LED de status
 *
 * O terminal serial mostra somente o menu: o ESP_LOG é desviado para um
 * histórico em memória (Menu > Log técnico) logo no início.
 * ============================================================================
 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"

#include "app_state.h"
#include "audio_pipeline.h"
#include "config.h"
#include "diagnostics.h"
#include "hal_console.h"
#include "hal_indicators.h"
#include "hal_storage.h"
#include "hal_wifi.h"
#include "link_service.h"
#include "metronome.h"
#include "serial_ui.h"
#include "ui_events.h"

static const char *TAG = "main";

/* Trava a inicialização em caso de falha crítica, explicando no terminal. */
static void fatal(const char *what, const char *action) {
    ESP_LOGE(TAG, "%s", what);
    serial_ui_fatal(what, action);
    hal_status_led_set(STATUS_ERROR);
    while (1) vTaskDelay(pdMS_TO_TICKS(1000));
}

/* Mudança no Wi-Fi: atualiza o LED de status, confirma a conexão em verde e
 * registra um aviso para o menu quando a situação ou a causa da falha muda. */
static void on_wifi_state(HalWifiState state) {
    static HalWifiState last_state = HAL_WIFI_CONNECTING;
    static HalWifiError last_error = HAL_WIFI_ERR_NONE;
    app_state_refresh_status_led();

    HalWifiDiag d;
    hal_wifi_get_diag(&d);
    if (state == HAL_WIFI_CONNECTED && last_state != HAL_WIFI_CONNECTED) {
        char ip[24];
        hal_wifi_ip_str(ip, sizeof(ip));
        hal_rgb_flash(0, 255, 0, 400);
        ui_event(UI_EV_OK, "Wi-Fi conectado a \"%s\" (IP %s)", d.ssid, ip);
    } else if (state == HAL_WIFI_AP_ONLY && last_state != HAL_WIFI_AP_ONLY) {
        ui_event(UI_EV_WARN, "Rede própria ativa: \"%s\" (senha %s)", hal_wifi_device_name(),
                 WIFI_AP_PASS);
    }
    if (d.error != last_error && d.error != HAL_WIFI_ERR_NONE) {
        char why[96];
        diagnostics_wifi_error_text(&d, why, sizeof(why));
        ui_event(UI_EV_ERROR, "Wi-Fi: %s", why);
    }
    last_state = state;
    last_error = d.error;
}

void app_main(void) {
    // --- Terminal exclusivo do menu: o ESP_LOG vai para o log técnico ---
    hal_console_capture_logs();
    if (!hal_console_init()) ESP_LOGW(TAG, "Console serial indisponível");
    serial_ui_splash();

    // --- HAL: indicadores e armazenamento ---
    if (!hal_indicators_init()) ESP_LOGW(TAG, "Falha ao configurar LEDs/buzzer");
    if (!hal_storage_init()) {
        fatal("Falha ao iniciar a memória NVS (configurações).",
              "Apague a flash (PlatformIO: Platform > Erase Flash) e grave o firmware de novo.");
    }

    // --- Estado compartilhado e metrônomo ---
    app_state_init();
    metronome_init();

    // --- Captura e processamento de áudio ---
    // Sem microfone o restante continua funcionando; o menu explica o problema
    bool audio_ok = audio_pipeline_init();
    if (!audio_ok) ui_event(UI_EV_ERROR, "Microfone (I2S) não iniciou: veja o diagnóstico");

    // --- Rede ---
    WifiCredentials cred;
    hal_storage_load_wifi(&cred);
    if (!hal_wifi_start(&cred, on_wifi_state)) {
        fatal("Falha ao iniciar o rádio Wi-Fi.",
              "Desligue e ligue o dispositivo. Se continuar, grave o firmware de novo.");
    }
    if (!link_service_init()) {
        fatal("Falha ao abrir a comunicação com o app (UDP/WebSocket).",
              "Desligue e ligue o dispositivo. Se continuar, grave o firmware de novo.");
    }

    // --- Tarefas ---
    if (audio_ok) audio_pipeline_start_task();
    link_service_start();
    serial_ui_start_task();

    ESP_LOGI(TAG, "Sistema pronto: %s", hal_wifi_device_name());
    // A função main termina, mas as tarefas continuam executando
}
