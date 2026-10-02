/*
 * ============================================================================
 * drivers/drv_i2s_mic – implementação com o driver i2s_std do ESP-IDF 5
 * ============================================================================
 */
#include "drv_i2s_mic.h"

#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "drv_i2s";

static i2s_chan_handle_t s_rx = NULL;   // Canal RX do I2S
static int32_t *s_raw = NULL;           // Palavras de 32 bits lidas via DMA
static size_t s_capacity = 0;           // Capacidade do buffer (amostras)
static bool s_enabled = false;

bool drv_i2s_mic_init(const DrvI2sMicConfig *cfg, size_t max_samples_per_read) {
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 6;
    chan_cfg.dma_frame_num = 256;
    chan_cfg.auto_clear = true;
    esp_err_t err = i2s_new_channel(&chan_cfg, NULL, &s_rx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel falhou: %s", esp_err_to_name(err));
        return false;
    }

    // Formato Philips com slots de 32 bits: o INMP441 precisa de 64 ciclos de BCLK por quadro
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(cfg->sample_rate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT,
                                                        I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,                 // O INMP441 não usa master clock
            .bclk = (gpio_num_t)cfg->bck_pin,
            .ws   = (gpio_num_t)cfg->ws_pin,
            .dout = I2S_GPIO_UNUSED,                 // Somente entrada
            .din  = (gpio_num_t)cfg->data_pin,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;  // L/R do microfone em GND

    err = i2s_channel_init_std_mode(s_rx, &std_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_init_std_mode falhou: %s", esp_err_to_name(err));
        drv_i2s_mic_deinit();
        return false;
    }

    // Pull-down no SD: com o microfone desligado ou o fio solto a linha fica
    // em 0 (em vez de flutuar), e a ausência de sinal é detectada com certeza.
    // O INMP441 aciona a linha com força suficiente quando está ativo.
    gpio_pulldown_en((gpio_num_t)cfg->data_pin);

    s_capacity = max_samples_per_read;
    s_raw = malloc(max_samples_per_read * sizeof(int32_t));
    if (!s_raw) {
        ESP_LOGE(TAG, "Sem memória para o buffer I2S");
        drv_i2s_mic_deinit();
        return false;
    }
    ESP_LOGI(TAG, "I2S pronto: %lu Hz, 32 bits, BCK=%d WS=%d SD=%d",
             (unsigned long)cfg->sample_rate, cfg->bck_pin, cfg->ws_pin, cfg->data_pin);
    return true;
}

bool drv_i2s_mic_start(void) {
    if (!s_rx) return false;
    if (s_enabled) return true;
    esp_err_t err = i2s_channel_enable(s_rx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_enable falhou: %s", esp_err_to_name(err));
        return false;
    }
    s_enabled = true;
    return true;
}

void drv_i2s_mic_stop(void) {
    if (s_rx && s_enabled) {
        i2s_channel_disable(s_rx);
        s_enabled = false;
    }
}

size_t drv_i2s_mic_read(int32_t *out, size_t n, uint32_t timeout_ms) {
    if (!s_enabled || n > s_capacity) return 0;
    size_t bytes_read = 0;
    esp_err_t err = i2s_channel_read(s_rx, s_raw, n * sizeof(int32_t), &bytes_read,
                                     pdMS_TO_TICKS(timeout_ms));
    size_t got = bytes_read / sizeof(int32_t);
    if (err != ESP_OK || got != n) {
        ESP_LOGW(TAG, "Leitura I2S incompleta: %u de %u amostras (%s)", (unsigned)got,
                 (unsigned)n, esp_err_to_name(err));
    }
    // 24 bits úteis nos bits 31..8
    for (size_t i = 0; i < got; i++) out[i] = s_raw[i] >> 8;
    return got;
}

void drv_i2s_mic_deinit(void) {
    if (s_rx) {
        drv_i2s_mic_stop();
        i2s_del_channel(s_rx);                      // Libera o canal
        s_rx = NULL;
    }
    free(s_raw);
    s_raw = NULL;
    s_capacity = 0;
}
