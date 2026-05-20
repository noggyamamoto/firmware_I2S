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






void app_main() {
    printf("Iniciando o MicroDetection...\n");
}