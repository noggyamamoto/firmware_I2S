/*
 * ============================================================================
 * hal/hal_system – implementação com esp_system
 * ============================================================================
 */
#include "hal_system.h"

#include "esp_system.h"

void hal_system_restart(void) {
    esp_restart();
}

uint32_t hal_system_free_heap(void) {
    return esp_get_free_heap_size();
}
