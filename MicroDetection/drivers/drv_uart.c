/*
 * ============================================================================
 * drivers/drv_uart – implementação com o driver UART do ESP-IDF
 * ============================================================================
 */
#include "drv_uart.h"

#include "driver/uart.h"
#include "freertos/FreeRTOS.h"

bool drv_uart_init(int port, uint32_t baudrate, size_t rx_buffer, size_t tx_buffer) {
    uart_config_t cfg = {
        .baud_rate = (int)baudrate,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    if (uart_param_config((uart_port_t)port, &cfg) != ESP_OK) return false;
    return uart_driver_install((uart_port_t)port, rx_buffer, tx_buffer, 0, NULL, 0) == ESP_OK;
}

void drv_uart_write(int port, const void *data, size_t len) {
    uart_write_bytes((uart_port_t)port, data, len);
}

bool drv_uart_read_byte(int port, uint8_t *out, uint32_t timeout_ms) {
    return uart_read_bytes((uart_port_t)port, out, 1, pdMS_TO_TICKS(timeout_ms)) == 1;
}
