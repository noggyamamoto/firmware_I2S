/*
 * ============================================================================
 * drivers/drv_i2s_mic – microfone digital INMP441 via I2S
 *
 * O INMP441 entrega amostras de 24 bits (complemento de 2, MSB primeiro)
 * alinhadas à esquerda em slots de 32 bits, no formato I2S Philips. Com o
 * pino L/R ligado ao GND, o microfone transmite no slot esquerdo.
 * As leituras usam DMA (6 descritores de 256 quadros ≈ 96 ms de folga).
 * ============================================================================
 */
#ifndef DRV_I2S_MIC_H
#define DRV_I2S_MIC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** Pinos e taxa de amostragem do microfone. */
typedef struct {
    int bck_pin;                    // Bit clock (SCK)
    int ws_pin;                     // Word select (WS/LRCLK)
    int data_pin;                   // Dados (SD)
    uint32_t sample_rate;           // Hz
} DrvI2sMicConfig;

/** @brief Cria o canal I2S em modo mestre RX (Philips, 32 bits, mono esquerdo). */
bool drv_i2s_mic_init(const DrvI2sMicConfig *cfg, size_t max_samples_per_read);

/** @brief Habilita o DMA e começa a captar. */
bool drv_i2s_mic_start(void);

/** @brief Desabilita o canal. */
void drv_i2s_mic_stop(void);

/**
 * @brief Lê até `n` amostras de 24 bits (já deslocadas para int32).
 * @return Quantidade de amostras lidas (menor que `n` em caso de erro/timeout).
 */
size_t drv_i2s_mic_read(int32_t *out, size_t n, uint32_t timeout_ms);

/** @brief Libera o canal e os buffers. */
void drv_i2s_mic_deinit(void);

#endif // DRV_I2S_MIC_H
