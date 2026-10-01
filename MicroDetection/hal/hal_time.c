/*
 * ============================================================================
 * hal/hal_time – implementação com esp_timer
 * ============================================================================
 */
#include "hal_time.h"

#include "esp_timer.h"

int64_t hal_time_us(void) {
    return esp_timer_get_time();
}

HalTimer hal_timer_create(hal_timer_cb callback, void *arg, const char *name) {
    const esp_timer_create_args_t args = {.callback = callback, .arg = arg, .name = name};
    esp_timer_handle_t handle = NULL;
    if (esp_timer_create(&args, &handle) != ESP_OK) return NULL;
    return (HalTimer)handle;
}

void hal_timer_start_once(HalTimer timer, uint64_t delay_us) {
    esp_timer_handle_t h = (esp_timer_handle_t)timer;
    esp_timer_stop(h);                              // Ignora erro se não estava ativo
    esp_timer_start_once(h, delay_us);
}

void hal_timer_start_periodic(HalTimer timer, uint64_t period_us) {
    esp_timer_handle_t h = (esp_timer_handle_t)timer;
    esp_timer_stop(h);
    esp_timer_start_periodic(h, period_us);
}

void hal_timer_stop(HalTimer timer) {
    esp_timer_stop((esp_timer_handle_t)timer);
}
