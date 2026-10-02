/*
 * ============================================================================
 * Configurações de hardware, áudio, DSP e rede
 *
 * Os pinos são escolhidos automaticamente conforme o alvo de compilação:
 *  - ESP32 (esp32doit-devkit-v1)(testes iniciais)
 *  - ESP32-S3 (ESP32-S3-DevKitC-1 N16R8, testes com o circuito montado)
 *
 * Ligação do microfone INMP441 (I2S):
 *   INMP441  |  ESP32   | ESP32-S3
 *   ---------+----------+---------
 *   VDD      |  3V3     |  3V3
 *   GND      |  GND     |  GND
 *   L/R      |  GND     |  GND      (GND = canal esquerdo)
 *   SCK      |  GPIO26  |  GPIO5    (bit clock)
 *   WS       |  GPIO25  |  GPIO6    (word select)
 *   SD       |  GPIO33  |  GPIO4    (dados)
 * ============================================================================
 */
#ifndef CONFIG_H
#define CONFIG_H

#include "sdkconfig.h"

#define FIRMWARE_VERSION        "1.1.0"                 // Versão enviada no ANNOUNCE
#define DEVICE_NAME_PREFIX      "PartituraIoT"          // Prefixo do nome do dispositivo

// ======================= PINOS ==============================================
#if CONFIG_IDF_TARGET_ESP32S3
// --- I2S (INMP441) ---
#define I2S_BCK_PIN             5              // Bit Clock (SCK)
#define I2S_WS_PIN              6              // Word Select (WS/LRCLK)
#define I2S_DATA_IN_PIN         4              // Dados (SD)
// --- Indicadores ---
#define STATUS_LED_PIN          2              // LED de status da rede (RFE01)
#define RGB_LED_R_PIN           15             // LED RGB do metrônomo visual (RFE03)
#define RGB_LED_G_PIN           16
#define RGB_LED_B_PIN           17
#define BUZZER_PIN              18             // Buzzer passivo do metrônomo (RFE02)
#else
// --- I2S (INMP441) ---
#define I2S_BCK_PIN             26             // Bit Clock (SCK)
#define I2S_WS_PIN              25             // Word Select (WS/LRCLK)
#define I2S_DATA_IN_PIN         33             // Dados (SD)
// --- Indicadores ---
#define STATUS_LED_PIN          2              // LED azul on-board do DevKit
#define RGB_LED_R_PIN           27
#define RGB_LED_G_PIN           14
#define RGB_LED_B_PIN           13
#define BUZZER_PIN              18
#endif

#define RGB_LED_COMMON_ANODE    0                       // 1 se o LED RGB for de anodo comum

// --- UART ---
#define UART_PORT               0                       // Porta UART0 (monitor serial)
#define UART_BAUDRATE           115200                  // Taxa de transmissão serial (bps)
#define UART_RX_BUF_SIZE        2048                    // Buffer de recepção UART
#define UART_TX_BUF_SIZE        4096                    // Buffer de transmissão UART (uma tela do menu)

// --- Menu serial (serial_ui) ---
#define SERIAL_UI_ANSI          1                       // 1 = cores e cursor ANSI (PlatformIO, PuTTY, screen)
                                                        // 0 = texto puro (ex.: Monitor Serial da IDE Arduino)
#define SERIAL_UI_IDLE_RESET_MS 20000                   // Sem interação por 20 s: o menu volta ao início
#define CONSOLE_LOG_LINES       40                      // Linhas guardadas do log técnico (não impresso)

// ======================= ÁUDIO ==============================================
#define SAMPLE_RATE             16000                   // Taxa de amostragem: 16 kHz
#define HOP_SAMPLES             256                     // Amostras lidas por iteração (16 ms)
#define WINDOW_SAMPLES          1024                    // Janela de análise do YIN (64 ms)
#define BLOCK_SAMPLES           1024                    // Amostras por quadro bruto (modo diagnóstico)
#define INPUT_GAIN              4.0f                    // Ganho digital aplicado ao sinal do INMP441

// ======================= DSP ================================================
#define BANDPASS_LOW_HZ         70.0f                   // Corte inferior do passa-banda
#define BANDPASS_HIGH_HZ        2500.0f                 // Corte superior do passa-banda
#define PITCH_MIN_HZ            65.0f                   // Dó 2 (C2)
#define PITCH_MAX_HZ            2100.0f                 // Dó 7 (C7)
#define YIN_THRESHOLD           0.15f                   // Limiar de aperiodicidade do YIN
#define MIN_CONFIDENCE          0.70f                   // Confiança mínima para aceitar a altura

// --- Segmentação de notas (onset/offset) ---
#define GATE_MARGIN_DB          12.0f                   // Margem acima do piso de ruído
#define GATE_MIN_DB             -62.0f                  // Limiar absoluto mínimo (dBFS)
#define RELEASE_HYSTERESIS_DB   3.0f                    // Histerese para o fim de nota
#define DECAY_RELEASE_DB        30.0f                   // Queda máxima em relação ao pico da nota
#define REATTACK_DB             6.0f                    // Salto de energia que indica novo ataque
#define STABLE_HOPS             2                       // Janelas com a mesma nota para confirmar
#define ONSET_TIMEOUT_HOPS      8                       // Janelas máximas para confirmar a altura
#define RELEASE_HOPS            2                       // Janelas abaixo do limiar para soltar
#define MIN_NOTE_MS             60                      // Duração mínima de uma nota válida
#define QUANTIZE_MS             10                      // Quantização temporal (RNFE04)
#define CLICK_MASK_MS           45                      // Ignora ataques durante o clique do buzzer

// ======================= REDE ===============================================
// Credenciais padrão (podem ser alteradas pelo menu serial e ficam salvas na NVS)
#define WIFI_DEFAULT_SSID       "JOAO_2.4G"                  // Nome da rede Wi-Fi (substituir)
#define WIFI_DEFAULT_PASS       "30226280!"                 // Senha da rede Wi-Fi
#define WIFI_MAX_RETRIES        8                       // Tentativas antes de ativar o modo AP
#define WIFI_FATAL_RETRIES      3                       // Idem quando a causa não se resolve sozinha
                                                        // (senha incorreta, rede inexistente)
#define WIFI_COUNTRY_CODE       "BR"                    // Canais 1–13 (Anatel): alcança modems nos canais 12/13
#define WIFI_DHCP_TIMEOUT_MS    12000                   // Associado à rede, mas sem IP após esse tempo
#define WIFI_AP_PASS            "partitura123"          // Senha da rede própria (modo AP)
#define WIFI_AP_CHANNEL         6                       // Canal da rede própria

#define WS_PORT                 80                      // Servidor WebSocket do app web (ws://IP/ws)

#define SESSION_TIMEOUT_MS      5000                    // Sem PING por esse tempo = app perdido
#define EVENT_QUEUE_LENGTH      32                      // Fila prioritária: notas, batidas e respostas
#define STREAM_QUEUE_LENGTH     16                      // Fila de fluxo contínuo: altura e áudio bruto
#define WS_SEND_TIMEOUT_S       1                       // Tempo máximo de envio por WebSocket (padrão: 5 s)

#endif // CONFIG_H
