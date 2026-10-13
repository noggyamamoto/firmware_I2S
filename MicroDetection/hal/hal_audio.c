/*
 * ============================================================================
 * hal/hal_audio – implementação sobre drivers/drv_i2s_mic
 * ============================================================================
 */
#include "hal_audio.h"

#include <stdlib.h>

#include "config.h"
#include "drv_i2s_mic.h"
#include "hal_time.h"

static int32_t *s_raw = NULL;
static size_t s_capacity = 0;
static uint64_t s_samples_read = 0;     // Total de amostras desde o start
static int64_t s_start_us = 0;          // Instante da amostra 0 (reancorado quando necessário)
static int64_t s_block_us = 0;          // Instante da primeira amostra do último bloco lido
static volatile uint32_t s_resyncs = 0; // Vezes em que o relógio de amostras foi corrigido

// Taxa de amostragem medida (amostras recebidas / tempo do sistema), a cada
// RATE_WINDOW_US. Confere se o I2S entrega de fato SAMPLE_RATE amostras por
// segundo: uma taxa diferente desloca todas as alturas (ex.: 4x = 2 oitavas)
#define RATE_WINDOW_US  2000000
static uint64_t s_rate_samples = 0;     // Amostras desde s_rate_t0
static int64_t s_rate_t0 = 0;
static volatile float s_rate_measured = 0.0f;

// O DMA guarda no máximo ~96 ms de áudio (6 x 256 amostras). Se a última
// amostra lida está mais "velha" do que isso, amostras foram perdidas (tarefa
// atrasada); se está no futuro, o relógio do I2S anda diferente do sistema.
// Nos dois casos o relógio de amostras é reancorado no relógio do sistema,
// para que os instantes das notas não se afastem do metrônomo.
#define MAX_LAG_US      120000
#define MAX_LEAD_US     5000

bool hal_audio_init(size_t max_samples_per_read) {
    const DrvI2sMicConfig cfg = {
        .bck_pin = I2S_BCK_PIN,
        .ws_pin = I2S_WS_PIN,
        .data_pin = I2S_DATA_IN_PIN,
        .sample_rate = SAMPLE_RATE,
    };
    s_raw = malloc(max_samples_per_read * sizeof(int32_t));
    if (!s_raw) return false;
    s_capacity = max_samples_per_read;
    return drv_i2s_mic_init(&cfg, max_samples_per_read);
}

bool hal_audio_start(void) {
    if (!drv_i2s_mic_start()) return false;
    s_samples_read = 0;
    s_start_us = hal_time_us();
    s_rate_t0 = s_start_us;
    s_rate_samples = 0;
    return true;
}

void hal_audio_stop(void) {
    drv_i2s_mic_stop();
}

size_t hal_audio_read(float *out, size_t n, uint32_t timeout_ms) {
    if (n > s_capacity) return 0;
    size_t got = drv_i2s_mic_read(s_raw, n, timeout_ms);
    s_rate_samples += got;
    int64_t t = hal_time_us();
    if (t - s_rate_t0 >= RATE_WINDOW_US) {
        s_rate_measured = (float)((double)s_rate_samples * 1e6 / (double)(t - s_rate_t0));
        s_rate_t0 = t;
        s_rate_samples = 0;
    }
    if (got != n) {
        s_samples_read += got;                  // Mantém o relógio alinhado ao DMA
        return 0;
    }
    // 24 bits úteis: normaliza para [-1, 1) e aplica o ganho de entrada
    const float scale = INPUT_GAIN / 8388608.0f;
    for (size_t i = 0; i < n; i++) out[i] = (float)s_raw[i] * scale;
    s_samples_read += n;

    int64_t now = hal_time_us();
    int64_t end = hal_audio_next_sample_time_us();
    if (now - end > MAX_LAG_US || end - now > MAX_LEAD_US) {
        s_start_us = now - (int64_t)(s_samples_read * 1000000ULL / SAMPLE_RATE);
        s_resyncs++;
    }
    s_block_us = hal_audio_next_sample_time_us() - (int64_t)n * 1000000LL / SAMPLE_RATE;
    return n;
}

int64_t hal_audio_next_sample_time_us(void) {
    return s_start_us + (int64_t)(s_samples_read * 1000000ULL / SAMPLE_RATE);
}

int64_t hal_audio_block_time_us(void) {
    return s_block_us;
}

float hal_audio_measured_rate(void) {
    return s_rate_measured;
}

uint32_t hal_audio_resync_count(void) {
    return s_resyncs;
}

void hal_audio_deinit(void) {
    drv_i2s_mic_deinit();
    free(s_raw);
    s_raw = NULL;
    s_capacity = 0;
}
