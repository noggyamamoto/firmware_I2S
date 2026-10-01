/*
 * ============================================================================
 * drivers/drv_pwm – implementação com o periférico LEDC do ESP-IDF
 * ============================================================================
 */
#include "drv_pwm.h"

#include "driver/ledc.h"

#define PWM_MODE LEDC_LOW_SPEED_MODE            // Único modo disponível no ESP32-S3

bool drv_pwm_timer_init(int timer, uint32_t freq_hz, uint8_t resolution_bits) {
    ledc_timer_config_t cfg = {
        .speed_mode = PWM_MODE,
        .duty_resolution = (ledc_timer_bit_t)resolution_bits,
        .timer_num = (ledc_timer_t)timer,
        .freq_hz = freq_hz,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    return ledc_timer_config(&cfg) == ESP_OK;
}

bool drv_pwm_channel_init(int channel, int timer, int pin) {
    ledc_channel_config_t cfg = {
        .gpio_num = pin,
        .speed_mode = PWM_MODE,
        .channel = (ledc_channel_t)channel,
        .timer_sel = (ledc_timer_t)timer,
        .duty = 0,
        .hpoint = 0,
    };
    return ledc_channel_config(&cfg) == ESP_OK;
}

void drv_pwm_set_duty(int channel, uint32_t duty) {
    ledc_set_duty(PWM_MODE, (ledc_channel_t)channel, duty);
    ledc_update_duty(PWM_MODE, (ledc_channel_t)channel);
}

void drv_pwm_set_freq(int timer, uint32_t freq_hz) {
    ledc_set_freq(PWM_MODE, (ledc_timer_t)timer, freq_hz);
}
