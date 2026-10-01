/*
 * ============================================================================
 * drivers/drv_gpio – saídas digitais (GPIO)
 *
 * Controle direto dos pinos de saída, como o LED embutido da placa
 * (GPIO2 no ESP32 DevKit V1), usado como LED de status da rede (RFE01).
 * ============================================================================
 */
#ifndef DRV_GPIO_H
#define DRV_GPIO_H

#include <stdbool.h>

/** @brief Configura o pino como saída digital (sem pull-up/pull-down) e o deixa em nível baixo. */
bool drv_gpio_output_init(int pin);

/** @brief Escreve o nível lógico no pino (true = alto). */
void drv_gpio_write(int pin, bool level);

#endif // DRV_GPIO_H
