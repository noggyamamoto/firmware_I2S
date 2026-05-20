// =========================== BIBLIOTECAS PADRÃO ===========================
#include <stdio.h>      // Funções padrão de entrada/saída (fopen, fwrite, etc.)
#include <stdlib.h>     // Funções utilitárias (malloc, free, atoi)
#include <string.h>     // Manipulação de strings (memset, snprintf)
#include <stdarg.h>     // Para argumentos variáveis (vsnprintf no uart_printf)
#include <time.h>       // Funções de data/hora (não usado diretamente)
#include <stdbool.h>    // Tipo booleano (true/false)
#include <errno.h>      // Códigos de erro (errno, strerror)

// =========================== BIBLIOTECAS DO FREERTOS ======================
#include "freertos/FreeRTOS.h"     // Kernel do FreeRTOS (tasks, mutex, delays)
#include "freertos/task.h"         // Funções de tarefas (xTaskCreate, vTaskDelay)
#include "freertos/semphr.h"       // Semáforos e mutex (xSemaphoreCreateMutex)

// =========================== BIBLIOTECAS DO ESP-IDF =======================
#include "driver/uart.h"           // Driver da UART (comunicação serial)
#include "driver/gpio.h"           // Controle de GPIO (pinos)
#include "driver/spi_master.h"     // Driver SPI (para comunicação com o SD)
#include "esp_timer.h"             // Timer de alta resolução (microssegundos)
#include "esp_heap_caps.h"         // Funções de memória heap (esp_get_free_heap)
#include "esp_rom_crc.h"           // Cálculo de CRC-32 via ROM (esp_rom_crc32_le)

#include "esp_adc/adc_continuous.h"// ADC contínuo com DMA (para alta taxa)
#include "driver/sdspi_host.h"     // Driver SD via SPI
#include "sdmmc_cmd.h"             // Estruturas e comandos para cartão SD
#include "esp_vfs_fat.h"           // Sistema de arquivos FAT (montagem do SD)

// =========================== DOUBLE BUFFER CIRCULAR ========================
#define BLOCK_SAMPLES           4096                  // Amostras por bloco (4096 * 8 = 32KB)
#define BLOCK_BYTES             (BLOCK_SAMPLES * sizeof(amostra_t)) // 32768 bytes
#define CIRC_BUFFER_CAPACITY    4                     // Nº máximo de blocos em RAM
#define DMA_BUFFER_SIZE         8192                  // Buffer interno do driver DMA

// =========================== ESTRUTURAS DE DADOS ==========================
// Estrutura de uma amostra individual
typedef struct {
    uint32_t time_ms;    // Timestamp em milissegundos (teórico)
    uint8_t  n_adc;      // Número do canal (0..7)
    uint16_t adc_value;  // Valor bruto do ADC (0..4095)
} amostra_t;

// Estrutura que representa um bloco de amostras
typedef struct {
    amostra_t* data;        // Ponteiro para o array de amostras
    uint32_t   num_samples; // Número de amostras válidas neste bloco
} block_t;

// Cabeçalho do arquivo binário (atributo packed elimina padding entre campos)
typedef struct __attribute__((packed)) {
    uint32_t magic;          // Número mágico para identificar o formato (0xCEFAEDFE)
    uint32_t version;        // Versão do formato (1)
    uint32_t block_num;      // Número sequencial do bloco
    uint64_t start_time_us;  // Timestamp de início da coleta (microssegundos)
    uint32_t num_samples;    // Quantidade de amostras neste bloco
    uint32_t crc;            // CRC-32 do restante do arquivo (exceto este campo)
} block_header_t;






void app_main() {
    printf("Iniciando o MicroDetection...\n");
}