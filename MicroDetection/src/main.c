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

// ======================= CAPTURADOR I2S ===================================
/*
 * Estrutura que representa o periférico I2S.
 */
typedef struct {
    i2s_chan_handle_t rx_handle;    // Handle (descritor) do canal RX do I2S
} I2SAudioCapturer;

/**
 * @brief Inicializa o driver I2S no modo mestre RX (Philips).
 * @param capt Ponteiro para a estrutura I2SAudioCapturer.
 * @return true se a inicialização foi bem-sucedida.
 */
static bool i2s_capturer_init(I2SAudioCapturer *capt) {
    // Configuração básica do canal: modo mestre, canal RX, seleção automática do controlador
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;                              // Habilita limpeza automática de DMA
    // Cria o novo canal (parâmetro TX = NULL, pois é apenas recepção)
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, NULL, &capt->rx_handle));

    // Configuração do formato padrão (Philips/I2S)
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),  // Configura o clock com base na taxa de amostragem
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(BITS_PER_SAMPLE, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,                         // Master clock não usado
            .bclk = I2S_BCK_PIN,                             // Pino do bit clock
            .ws   = I2S_WS_PIN,                              // Pino do word select
            .dout = I2S_GPIO_UNUSED,                         // Saída não usada (somente entrada)
            .din  = I2S_DATA_IN_PIN,                         // Pino de dados de entrada
            .invert_flags = {
                .mclk_inv = false,                           // Não inverte MCLK
                .bclk_inv = false,                           // Não inverte BCLK
                .ws_inv   = false,                           // Não inverte WS
            },
        },
    };
    // Inicializa o canal no modo padrão com as configurações acima
    esp_err_t ret = i2s_channel_init_std_mode(capt->rx_handle, &std_cfg);
    return (ret == ESP_OK);                                  // Retorna true se deu certo
}

/**
 * @brief Habilita a captura (inicia o DMA).
 */
static void i2s_capturer_start(I2SAudioCapturer *capt) {
    i2s_channel_enable(capt->rx_handle);                     // Ativa o canal I2S
}

/**
 * @brief Para a captura (desabilita o canal).
 */
static void i2s_capturer_stop(I2SAudioCapturer *capt) {
    i2s_channel_disable(capt->rx_handle);                    // Desativa o canal I2S
}

/**
 * @brief Lê um bloco de amostras do I2S (bloqueante até ter dados suficientes).
 * @param capt Ponteiro para o capturador.
 * @param buffer Buffer onde armazenar as amostras (int16_t).
 * @param num_samples Número de amostras desejadas.
 * @return Número de bytes lidos com sucesso.
 */
static size_t i2s_capturer_read(I2SAudioCapturer *capt, int16_t *buffer, size_t num_samples) {
    size_t bytes_read = 0;                                   // Inicializa contador de bytes lidos
    // Leitura bloqueante: aguarda até que todo o buffer seja preenchido
    esp_err_t ret = i2s_channel_read(capt->rx_handle, buffer, num_samples * sizeof(int16_t),
                                     &bytes_read, portMAX_DELAY);
    return (ret == ESP_OK) ? bytes_read : 0;                 // Retorna bytes lidos ou 0 em caso de erro
}

/**
 * @brief Libera os recursos do capturador I2S (desabilita canal e deleta handle).
 */
static void i2s_capturer_deinit(I2SAudioCapturer *capt) {
    if (capt->rx_handle) {                                   // Verifica se o handle é válido
        i2s_channel_disable(capt->rx_handle);               // Desabilita o canal
        i2s_del_channel(capt->rx_handle);                   // Deleta o canal, liberando recursos
    }
}

// ======================= PROCESSADOR DE ÁUDIO =============================
#define FILTER_ORDER 2               // Ordem do filtro IIR (2ª ordem -> 2 polos/zeros)

/*
 * Estrutura que mantém os coeficientes e o estado do filtro.
 */
typedef struct {
    float b[FILTER_ORDER + 1];       // Coeficientes do numerador (b0, b1, b2)
    float a[FILTER_ORDER + 1];       // Coeficientes do denominador (a0, a1, a2) (a0 sempre 1)
    float x_delay[FILTER_ORDER + 1]; // Atrasos das amostras de entrada [x[n-1], x[n-2]]
    float y_delay[FILTER_ORDER + 1]; // Atrasos das amostras de saída   [y[n-1], y[n-2]]
} AudioProcessor;

/**
 * @brief Inicializa o processador com coeficientes placeholder.
 * ATENÇÃO: Estes coeficientes são apenas ilustrativos.
 * Em produção, será substituído por coeficientes reais projetados para o filtro passa‑banda de 80-2000 Hz.
 */
static void audio_processor_init(AudioProcessor *proc) {
    // Coeficientes placeholder (não funcionais para filtro PB real)
    proc->b[0] = 0.1f;  proc->b[1] = -0.1f; proc->b[2] = 0.0f;
    proc->a[0] = 1.0f;  proc->a[1] = -0.5f;  proc->a[2] = 0.0f;

    // Zera os buffers de estado (garante condição inicial nula)
    memset(proc->x_delay, 0, sizeof(proc->x_delay));
    memset(proc->y_delay, 0, sizeof(proc->y_delay));

    // Log de aviso (visível no monitor serial)
    ESP_LOGW("AudioProc", "Usando coeficientes placeholder – substitua pelos reais!");
}

/**
 * @brief Aplica o filtro PB, normalização e calcula a energia média do quadro.
 * @param proc Ponteiro para o processador.
 * @param input Amostras de entrada (int16_t).
 * @param output Buffer de saída (pode ser o mesmo que input, para processamento in-place).
 * @param num_samples Número de amostras a processar.
 * @return Energia média do quadro processado.
 */
static float audio_processor_process(AudioProcessor *proc,
                                     const int16_t *input,
                                     int16_t *output,
                                     size_t num_samples) {
    float energy = 0.0f;            // Acumulador da energia (soma dos quadrados)
    int16_t max_val = 0;            // Valor absoluto máximo (para possível normalização)

    // Itera sobre cada amostra do bloco
    for (size_t i = 0; i < num_samples; i++) {
        // Converte amostra de 16 bits para float no intervalo [-1.0, 1.0)
        float xn = input[i] / 32768.0f;

        // --- Filtro IIR de 2ª ordem (forma direta I) ---
        // yn = b0*x[n] + b1*x[n-1] + b2*x[n-2] - a1*y[n-1] - a2*y[n-2]
        float yn = proc->b[0] * xn
                 + proc->b[1] * proc->x_delay[0]
                 + proc->b[2] * proc->x_delay[1]
                 - proc->a[1] * proc->y_delay[0]
                 - proc->a[2] * proc->y_delay[1];

        // Atualiza os buffers de atraso (shift register)
        proc->x_delay[1] = proc->x_delay[0];    // x[n-2] = x[n-1]
        proc->x_delay[0] = xn;                  // x[n-1] = x[n]
        proc->y_delay[1] = proc->y_delay[0];    // y[n-2] = y[n-1]
        proc->y_delay[0] = yn;                  // y[n-1] = y[n]

        // Converte o resultado float de volta para inteiro de 16 bits
        int16_t y_int = (int16_t)(yn * 32767.0f);

        // Se um buffer de saída foi fornecido, escreve a amostra processada
        if (output) {
            output[i] = y_int;
        } else {
            // Se output é NULL, usa a amostra original (fallback)
            y_int = input[i];
        }

        // Atualiza o máximo absoluto encontrado (para possível normalização de nível)
        if (abs(y_int) > abs(max_val)) max_val = y_int;

        // Acumula o quadrado da amostra para o cálculo da energia
        energy += (float)y_int * (float)y_int;
    }

    // Calcula a energia média do quadro (soma dos quadrados dividida pelo número de amostras)
    energy /= (float)num_samples;
    return energy;                  // Retorna a energia média
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