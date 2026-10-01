/*
 * ============================================================================
 * drivers/drv_gpio – implementação com o driver GPIO do ESP-IDF
 * ============================================================================
 */
#include "drv_gpio.h"

#include "driver/gpio.h"

bool drv_gpio_output_init(int pin) {
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << pin,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    if (gpio_config(&io) != ESP_OK) return false;
    gpio_set_level((gpio_num_t)pin, 0);
    return true;
}

void drv_gpio_write(int pin, bool level) {
    gpio_set_level((gpio_num_t)pin, level ? 1 : 0);
}
