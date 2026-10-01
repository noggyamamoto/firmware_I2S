/*
 * ============================================================================
 * drivers/drv_uart – porta serial (UART0, monitor serial)
 * ============================================================================
 */
#ifndef DRV_UART_H
#define DRV_UART_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** @brief Instala o driver da UART (8N1, sem controle de fluxo). */
bool drv_uart_init(int port, uint32_t baudrate, size_t rx_buffer, size_t tx_buffer);

/** @brief Envia bytes pela UART. */
void drv_uart_write(int port, const void *data, size_t len);

/**
 * @brief Lê um byte aguardando até `timeout_ms`.
 * @return true se um byte foi lido.
 */
bool drv_uart_read_byte(int port, uint8_t *out, uint32_t timeout_ms);

#endif // DRV_UART_H
