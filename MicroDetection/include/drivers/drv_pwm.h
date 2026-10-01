/*
 * ============================================================================
 * drivers/drv_pwm – PWM por hardware (periférico LEDC)
 *
 * Usado pelo LED RGB do metrônomo visual (brilho de cada cor) e pelo buzzer
 * passivo do metrônomo sonoro (frequência do clique).
 * O ESP32-S3 possui apenas o modo de baixa velocidade, usado em ambas as placas.
 * ============================================================================
 */
#ifndef DRV_PWM_H
#define DRV_PWM_H

#include <stdbool.h>
#include <stdint.h>

/** @brief Configura um temporizador PWM (0..3) com frequência e resolução (bits). */
bool drv_pwm_timer_init(int timer, uint32_t freq_hz, uint8_t resolution_bits);

/** @brief Associa um canal PWM (0..7) a um pino e a um temporizador (duty inicial 0). */
bool drv_pwm_channel_init(int channel, int timer, int pin);

/** @brief Altera o ciclo de trabalho do canal (0 .. 2^resolução - 1). */
void drv_pwm_set_duty(int channel, uint32_t duty);

/** @brief Altera a frequência do temporizador. */
void drv_pwm_set_freq(int timer, uint32_t freq_hz);

#endif // DRV_PWM_H
