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
 *  - net_tx      (núcleo 0, prioridade 4): fila de eventos -> UDP/WebSocket
 *  - net_rx      (núcleo 0, prioridade 3): comandos do app via UDP
 *  - httpd       (núcleo 0): comandos do app web via WebSocket
 *  - ui_task     (núcleo 0, prioridade 1): menu serial
 *  - status_led  (núcleo 0, prioridade 1): animação do LED de status
 * ============================================================================
 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"

#include "app_state.h"
#include "audio_pipeline.h"
#include "config.h"
#include "hal_console.h"
#include "hal_indicators.h"
#include "hal_storage.h"
#include "hal_wifi.h"
#include "link_service.h"
#include "metronome.h"
#include "serial_ui.h"

static const char *TAG = "main";

/* Trava a inicialização em caso de falha crítica, sinalizando no LED. */
static void fatal(const char *msg) {
    hal_console_printf("FATAL: %s\n", msg);
    ESP_LOGE(TAG, "%s", msg);
    hal_status_led_set(STATUS_ERROR);
    while (1) vTaskDelay(pdMS_TO_TICKS(1000));
}

/* Mudança no Wi-Fi: atualiza o LED de status e confirma a conexão em verde. */
static void on_wifi_state(HalWifiState state) {
    app_state_refresh_status_led();
    if (state == HAL_WIFI_CONNECTED) hal_rgb_flash(0, 255, 0, 400);
}

void app_main(void) {
    // --- HAL: console, indicadores e armazenamento ---
    if (!hal_console_init()) ESP_LOGW(TAG, "Console serial indisponível");
    if (!hal_indicators_init()) ESP_LOGW(TAG, "Falha ao configurar LEDs/buzzer");
    if (!hal_storage_init()) fatal("Falha ao inicializar a NVS.");

    // --- Estado compartilhado e metrônomo ---
    app_state_init();
    metronome_init();

    // --- Captura e processamento de áudio ---
    if (!audio_pipeline_init()) fatal("Falha ao inicializar o I2S / INMP441.");

    // --- Rede ---
    WifiCredentials cred;
    hal_storage_load_wifi(&cred);
    if (!hal_wifi_start(&cred, on_wifi_state)) fatal("Falha ao inicializar o Wi-Fi.");
    if (!link_service_init()) fatal("Falha ao abrir a comunicação (UDP/WebSocket).");

    // --- Tarefas ---
    audio_pipeline_start_task();
    link_service_start();
    serial_ui_start_task();

    ESP_LOGI(TAG, "Sistema pronto: %s", hal_wifi_device_name());
    // A função main termina, mas as tarefas continuam executando
}
