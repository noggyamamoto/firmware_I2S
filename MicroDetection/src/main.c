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
#define WIFI_SSID               "SSID"              // Nome da rede Wi-Fi (substituir)
#define WIFI_PASS               "SENHA"             // Senha da rede Wi-Fi
#define UDP_TARGET_IP           "192.168.0.100"     // IP do dispositivo Flutter (receptor)
#define UDP_TARGET_PORT         54321               // Porta UDP no destino

// ======================= ESTRUTURAS DE DADOS ==============================
/*
 * Cabeçalho do frame de áudio enviado via UDP.
 * Atributo packed garante que não haja padding entre os campos.
 */
#pragma pack(push, 1)           // Salva o alinhamento atual e define empacotamento byte a byte
typedef struct {
    uint32_t timestamp_ms;      // Timestamp desde o início da captura (em milissegundos)
    uint32_t sequence_number;   // Número sequencial do pacote (para detecção de perdas)
    uint32_t num_samples;       // Quantidade de amostras de áudio contidas neste frame
    float    energy;            // Energia média do quadro (útil para detecção de onset)
} AudioFrameHeader;             // Total: 16 bytes (4+4+4+4)
#pragma pack(pop)               // Restaura o alinhamento anterior

/*
 * Estrutura que agrupa cabeçalho + dados de áudio.
 */
typedef struct {
    AudioFrameHeader header;                    // Cabeçalho de 16 bytes
    int16_t samples[BLOCK_SAMPLES];             // 1024 amostras de 16 bits (2048 bytes)
} AudioFrame;                                   // Total: 2064 bytes por frame

// ======================= Buffer Circular para AudioFrame ================
#define CIRC_BUFFER_CAPACITY 4              // Capacidade máxima (nº de AudioFrames no buffer)

/*
 * Estrutura que implementa um buffer circular sincronizado.
 * Possui um array de tamanho fixo e índices de leitura/escrita.
 */
typedef struct {
    AudioFrame buffer[CIRC_BUFFER_CAPACITY]; // Array estático com até 4 frames
    int head;                               // Índice da próxima posição de leitura (consumidor)
    int tail;                               // Índice da próxima posição de escrita (produtor)
    int count;                              // Número atual de elementos no buffer
    SemaphoreHandle_t mutex;                // Mutex para acesso concorrente seguro
} CircularBuffer;

/**
 * @brief Inicializa o buffer circular criando o mutex e zerando índices.
 * @param cb Ponteiro para a estrutura CircularBuffer.
 */
static void circ_buffer_init(CircularBuffer *cb) {
    cb->head = 0;                                           // Inicia leitura no índice 0
    cb->tail = 0;                                           // Inicia escrita no índice 0
    cb->count = 0;                                          // Nenhum elemento armazenado
    cb->mutex = xSemaphoreCreateMutex();                    // Cria o mutex do FreeRTOS
    if (cb->mutex == NULL) {                                // Verifica se a criação falhou
        ESP_LOGE("CircBuffer", "Falha ao criar mutex");     // Log de erro crítico
        abort();                                            // Aborta execução (crítico)
    }
}

/**
 * @brief Tenta inserir um frame no buffer. Retorna true se bem-sucedido.
 * @param cb Ponteiro para o buffer.
 * @param frame Ponteiro para o AudioFrame a ser copiado.
 * @return true se o frame foi inserido; false se o buffer está cheio.
 */
static bool circ_buffer_push(CircularBuffer *cb, const AudioFrame *frame) {
    // Obtém o mutex (bloqueante) para acesso exclusivo
    if (xSemaphoreTake(cb->mutex, portMAX_DELAY) != pdTRUE)
        return false;                                       // Retorna falso se não conseguiu o mutex
    if (cb->count >= CIRC_BUFFER_CAPACITY) {                // Verifica se buffer está cheio
        xSemaphoreGive(cb->mutex);                          // Libera o mutex
        return false;                                       // Não foi possível inserir
    }
    cb->buffer[cb->tail] = *frame;                          // Copia o frame para a posição de escrita
    cb->tail = (cb->tail + 1) % CIRC_BUFFER_CAPACITY;       // Avança o índice de escrita (circular)
    cb->count++;                                            // Incrementa o contador de elementos
    xSemaphoreGive(cb->mutex);                              // Libera o mutex
    return true;                                            // Sucesso
}

/**
 * @brief Remove e obtém um frame do buffer.
 * @param cb Ponteiro para o buffer.
 * @param out Ponteiro onde o frame removido será armazenado.
 * @return true se havia um frame disponível; false se o buffer estava vazio.
 */
static bool circ_buffer_pop(CircularBuffer *cb, AudioFrame *out) {
    // Obtém o mutex (bloqueante)
    if (xSemaphoreTake(cb->mutex, portMAX_DELAY) != pdTRUE)
        return false;                                       // Falha ao obter mutex
    if (cb->count == 0) {                                   // Buffer vazio?
        xSemaphoreGive(cb->mutex);                          // Libera mutex
        return false;                                       // Nada a remover
    }
    *out = cb->buffer[cb->head];                            // Copia o frame da posição de leitura
    cb->head = (cb->head + 1) % CIRC_BUFFER_CAPACITY;       // Avança índice de leitura (circular)
    cb->count--;                                            // Decrementa contador
    xSemaphoreGive(cb->mutex);                              // Libera mutex
    return true;                                            // Sucesso
}

/**
 * @brief Retorna o número de elementos atualmente no buffer para debug.
 * @return Quantidade de elementos.
 */
static int circ_buffer_count(CircularBuffer *cb) {
    return cb->count;                                       // Retorna o contador interno
}


void app_main(void) {
     // --- Configura UART ---
    uart_config_t uart_cfg = {
        .baud_rate = UART_BAUDRATE,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_APB,
    };
    uart_param_config(UART_PORT, &uart_cfg);              // Aplica as configurações
    uart_set_pin(UART_PORT, UART_TXD_PIN, UART_RXD_PIN,   // Define os pinos TX e RX
                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE); // RTS e CTS não usados
    uart_driver_install(UART_PORT, UART_RX_BUF_SIZE,       // Instala o driver UART
                        UART_TX_BUF_SIZE, 0, NULL, 0);

    






}