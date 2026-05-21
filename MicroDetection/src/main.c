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

void app_main() {
    printf("Iniciando o MicroDetection...\n");
}