/*
 * ============================================================================
 * Sistema de Processamento de Áudio com Feedback em Tempo Real
 * Aplicado ao Ensino de Partitura – firmware do dispositivo embarcado
 *
 * Plataformas: esp32doit-devkit-v1 e ESP32-S3-DevKitC-1 (N16R8)
 *
 * Funcionalidades:
 *  - Captura de áudio via I2S do microfone digital INMP441 (16 kHz, 24 bits)
 *  - Filtro passa-banda real (70 Hz – 2,5 kHz) e piso de ruído adaptativo
 *  - Estimativa de altura (YIN) e segmentação em notas (ataque e soltura)
 *  - Envio dos eventos de nota via UDP para o app Flutter (protocol.h)
 *  - Metrônomo sonoro (buzzer) e visual (LED RGB) sincronizados com o app
 *  - Varredura e conexão Wi-Fi com LED de status e rede própria de reserva
 *  - Menu serial para diagnóstico e configuração
 *
 * Tarefas FreeRTOS:
 *  - audio_prod  (núcleo 1, prioridade 5): I2S -> DSP -> eventos
 *  - net_tx      (núcleo 0, prioridade 4): fila de eventos -> UDP
 *  - net_rx      (núcleo 0, prioridade 3): comandos do app
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
#include "indicators.h"
#include "metronome.h"
#include "serial_ui.h"
#include "settings.h"
#include "udp_link.h"
#include "wifi_manager.h"

static const char *TAG = "main";

/* Trava a inicialização em caso de falha crítica, sinalizando no LED. */
static void fatal(const char *msg) {
    uart_printf("FATAL: %s\n", msg);
    ESP_LOGE(TAG, "%s", msg);
    indicators_set_status(STATUS_ERROR);
    while (1) vTaskDelay(pdMS_TO_TICKS(1000));
}

void app_main(void) {
    // --- Interface serial e indicadores ---
    serial_ui_init();
    indicators_init();
    settings_init();

    // --- Estado compartilhado e metrônomo ---
    app_state_init();
    metronome_init();

    // --- Captura e processamento de áudio ---
    if (!audio_pipeline_init()) fatal("Falha ao inicializar o I2S / INMP441.");

    // --- Rede ---
    WifiCredentials cred;
    settings_load_wifi(&cred);
    if (!wifi_manager_start(&cred)) fatal("Falha ao inicializar o Wi-Fi.");
    if (!udp_link_init()) fatal("Falha ao abrir o socket UDP.");

    // --- Tarefas ---
    audio_pipeline_start_task();
    udp_link_start_tasks();
    serial_ui_start_task();

    ESP_LOGI(TAG, "Sistema pronto: %s", wifi_manager_device_name());
    // A função main termina, mas as tarefas continuam executando
}
