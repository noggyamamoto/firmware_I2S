/*
 * ============================================================================
 * Implementação da captura I2S (INMP441)
 * ============================================================================
 */
#include "audio_capture.h"

#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

#include "config.h"

static const char *TAG = "i2s";

bool audio_capture_init(AudioCapture *cap, size_t max_samples_per_read) {
    memset(cap, 0, sizeof(*cap));

    // Configuração do canal: mestre, RX. 6 descritores de 256 quadros ≈ 96 ms de folga no DMA
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 6;
    chan_cfg.dma_frame_num = 256;
    chan_cfg.auto_clear = true;
    esp_err_t err = i2s_new_channel(&chan_cfg, NULL, &cap->rx_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel falhou: %s", esp_err_to_name(err));
        return false;
    }

    // Formato Philips com slots de 32 bits: o INMP441 precisa de 64 ciclos de BCLK por quadro
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT,
                                                        I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,                 // O INMP441 não usa master clock
            .bclk = I2S_BCK_PIN,
            .ws   = I2S_WS_PIN,
            .dout = I2S_GPIO_UNUSED,                 // Somente entrada
            .din  = I2S_DATA_IN_PIN,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;  // L/R do microfone em GND

    err = i2s_channel_init_std_mode(cap->rx_handle, &std_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_init_std_mode falhou: %s", esp_err_to_name(err));
        i2s_del_channel(cap->rx_handle);
        cap->rx_handle = NULL;
        return false;
    }

    cap->raw_capacity = max_samples_per_read;
    cap->raw = malloc(max_samples_per_read * sizeof(int32_t));
    if (!cap->raw) {
        ESP_LOGE(TAG, "Sem memória para o buffer I2S");
        audio_capture_deinit(cap);
        return false;
    }
    ESP_LOGI(TAG, "I2S pronto: %d Hz, 32 bits, BCK=%d WS=%d SD=%d", SAMPLE_RATE,
             I2S_BCK_PIN, I2S_WS_PIN, I2S_DATA_IN_PIN);
    return true;
}

bool audio_capture_start(AudioCapture *cap) {
    if (!cap->rx_handle) return false;
    if (cap->enabled) return true;
    esp_err_t err = i2s_channel_enable(cap->rx_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_enable falhou: %s", esp_err_to_name(err));
        return false;
    }
    cap->enabled = true;
    cap->samples_read = 0;
    cap->start_us = esp_timer_get_time();
    return true;
}

void audio_capture_stop(AudioCapture *cap) {
    if (cap->rx_handle && cap->enabled) {
        i2s_channel_disable(cap->rx_handle);
        cap->enabled = false;
    }
}

size_t audio_capture_read(AudioCapture *cap, float *out, size_t n, uint32_t timeout_ms) {
    if (!cap->enabled || n > cap->raw_capacity) return 0;
    size_t bytes_read = 0;
    esp_err_t err = i2s_channel_read(cap->rx_handle, cap->raw, n * sizeof(int32_t),
                                     &bytes_read, pdMS_TO_TICKS(timeout_ms));
    size_t got = bytes_read / sizeof(int32_t);
    if (err != ESP_OK || got != n) {
        ESP_LOGW(TAG, "Leitura I2S incompleta: %u de %u amostras (%s)", (unsigned)got,
                 (unsigned)n, esp_err_to_name(err));
        cap->samples_read += got;
        return 0;
    }

    // 24 bits úteis nos bits 31..8: desloca e normaliza para [-1, 1)
    const float scale = INPUT_GAIN / 8388608.0f;
    for (size_t i = 0; i < n; i++) {
        out[i] = (float)(cap->raw[i] >> 8) * scale;
    }
    cap->samples_read += n;
    return n;
}

int64_t audio_capture_next_sample_time_us(const AudioCapture *cap) {
    return cap->start_us + (int64_t)(cap->samples_read * 1000000ULL / SAMPLE_RATE);
}

void audio_capture_deinit(AudioCapture *cap) {
    if (cap->rx_handle) {                                   // Verifica se o handle é válido
        audio_capture_stop(cap);                            // Desabilita o canal
        i2s_del_channel(cap->rx_handle);                    // Deleta o canal, liberando recursos
        cap->rx_handle = NULL;
    }
    free(cap->raw);
    cap->raw = NULL;
}
