/*
 * ============================================================================
 * MVP para Transcrição Musical Automática
 * Plataforma: esp32doit-devkit-v1
 *
 * Funcionalidades:
 *  - Captura de áudio via I2S (INMP441) a 16 kHz, 16 bits, mono
 *  - Filtro passa‑banda (80-2000 Hz), normalização e cálculo de energia
 *  - Buffer circular com 4 blocos de 1024 amostras cada
 *  - Tarefa produtora (Core 1) preenche os blocos
 *  - Tarefa consumidora (Core 0) transmite via UDP após empacotamento
 *  - Conexão Wi‑Fi com IP e porta configuráveis
 *  - Menu serial para iniciar/parar transmissão e exibir status
 * ============================================================================
 */

// ======================== BIBLIOTECAS PADRÃO C ============================
#include <stdio.h>          // Funções de entrada/saída padrão: printf, sprintf, etc.
#include <stdlib.h>         // Funções utilitárias: malloc, free, atoi
#include <string.h>         // Manipulação de strings: memset, strlen, strncpy
#include <stdarg.h>         // Suporte a argumentos variáveis: va_list, va_start, va_end
#include <inttypes.h>       // Macros para formatar inteiros: PRIu32, etc.
#include <math.h>           // Funções matemáticas: abs, fabs (se necessário)
#include <stdbool.h>        // Tipo booleano: true, false

// ======================= BIBLIOTECAS FREERTOS =============================
#include "freertos/FreeRTOS.h"       // Kernel do FreeRTOS (tipos, macros de atraso)
#include "freertos/task.h"           // Gerenciamento de tarefas: xTaskCreate, vTaskDelete
#include "freertos/semphr.h"         // Semáforos e mutexes: xSemaphoreCreateMutex

// ======================= BIBLIOTECAS ESP-IDF ==============================
#include "driver/uart.h"             // Driver UART para comunicação serial
#include "driver/gpio.h"             // Manipulação de pinos GPIO
#include "driver/i2s_std.h"          // Driver I2S padrão (modo master/slave)
#include "esp_timer.h"               // Timer de alta resolução: esp_timer_get_time()
#include "esp_wifi.h"                // Configuração e controle do Wi-Fi
#include "esp_event.h"               // Sistema de eventos (necessário para Wi-Fi)
#include "esp_log.h"                 // Macros de log: ESP_LOGE, ESP_LOGW, ESP_LOGI
#include "lwip/err.h"                // Erros da pilha LwIP
#include "lwip/sockets.h"            // Funções de socket: socket, sendto, inet_pton
#include "esp_netif.h"               // Interface de rede (netif)
#include "nvs_flash.h"               // Armazenamento não volátil (NVS) para Wi-Fi

// ======================= CONFIGURAÇÕES DE HARDWARE ========================
// --- UART ---
#define UART_PORT               UART_NUM_0              // Porta UART0 (conectada ao monitor serial)
#define UART_TXD_PIN            GPIO_NUM_1              // Pino TX da UART0
#define UART_RXD_PIN            GPIO_NUM_3              // Pino RX da UART0
#define UART_BAUDRATE           115200                  // Taxa de transmissão serial (bps)
#define UART_RX_BUF_SIZE        2048                    // Tamanho do buffer de recepção UART
#define UART_TX_BUF_SIZE        2048                    // Tamanho do buffer de transmissão UART

// --- I2S (INMP441) ---
#define I2S_BCK_PIN             GPIO_NUM_26             // Pino do Bit Clock (BCLK) do microfone
#define I2S_WS_PIN              GPIO_NUM_25             // Pino Word Select (WS/LRCLK) do microfone
#define I2S_DATA_IN_PIN         GPIO_NUM_33             // Pino de dados de entrada (DIN) do microfone

// --- Parâmetros de áudio ---
#define SAMPLE_RATE             16000                   // Taxa de amostragem: 16 kHz
#define BITS_PER_SAMPLE         I2S_DATA_BIT_WIDTH_16BIT // Resolução: 16 bits por amostra
#define NUM_CHANNELS             1                      // Número de canais: mono
#define BLOCK_SAMPLES           1024                    // Número de amostras por bloco de processamento
#define BLOCK_BYTES             (BLOCK_SAMPLES * sizeof(int16_t)) // Tamanho do bloco em bytes

// --- Rede Wi‑Fi ---
#define WIFI_SSID               "SEU_SSID"              // Nome da rede Wi-Fi (substituir)
#define WIFI_PASS               "SUA_SENHA"             // Senha da rede Wi-Fi
#define UDP_TARGET_IP           "192.168.0.100"         // IP do dispositivo Flutter (receptor)
#define UDP_TARGET_PORT         54321                   // Porta UDP no destino

void app_main() {
    printf("Iniciando o MicroDetection...\n");
}