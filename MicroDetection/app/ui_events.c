/*
 * ============================================================================
 * app/ui_events – implementação (anel em memória protegido por spinlock)
 * ============================================================================
 */
#include "ui_events.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"

#include "hal_time.h"

#define UI_EVENTS_MAX   6

static UiEvent           s_events[UI_EVENTS_MAX];
static int               s_next = 0;
static int               s_count = 0;
static volatile uint32_t s_version = 0;
static portMUX_TYPE      s_lock = portMUX_INITIALIZER_UNLOCKED;

void ui_event(UiEventLevel level, const char *fmt, ...) {
    UiEvent ev = {.level = level, .time_s = (uint32_t)(hal_time_us() / 1000000)};
    va_list args;
    va_start(args, fmt);
    vsnprintf(ev.text, sizeof(ev.text), fmt, args);     // Formata fora da seção crítica
    va_end(args);

    portENTER_CRITICAL(&s_lock);
    s_events[s_next] = ev;
    s_next = (s_next + 1) % UI_EVENTS_MAX;
    if (s_count < UI_EVENTS_MAX) s_count++;
    s_version++;
    portEXIT_CRITICAL(&s_lock);
}

int ui_events_recent(UiEvent *out, int max) {
    portENTER_CRITICAL(&s_lock);
    int n = s_count < max ? s_count : max;
    for (int i = 0; i < n; i++) {
        out[i] = s_events[(s_next - 1 - i + UI_EVENTS_MAX) % UI_EVENTS_MAX];
    }
    portEXIT_CRITICAL(&s_lock);
    return n;
}

uint32_t ui_events_version(void) {
    return s_version;
}
