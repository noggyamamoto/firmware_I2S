/*
 * ============================================================================
 * app/ui_events – avisos para o menu serial
 *
 * Os módulos da aplicação registram aqui os acontecimentos que interessam ao
 * usuário (app conectado, sessão iniciada, Wi-Fi caiu...). O menu exibe os
 * mais recentes dentro da própria tela, já que o terminal mostra somente o
 * menu (o ESP_LOG vai para o log técnico).
 *
 * Pode ser chamado de qualquer tarefa ou callback de timer: não bloqueia.
 * ============================================================================
 */
#ifndef UI_EVENTS_H
#define UI_EVENTS_H

#include <stdint.h>

typedef enum {
    UI_EV_INFO,
    UI_EV_OK,
    UI_EV_WARN,
    UI_EV_ERROR,
} UiEventLevel;

typedef struct {
    UiEventLevel level;
    uint32_t     time_s;                // Segundos desde o boot
    char         text[80];
} UiEvent;

/** @brief Registra um aviso (printf). */
void ui_event(UiEventLevel level, const char *fmt, ...);

/** @brief Avisos mais recentes, do mais novo para o mais antigo. */
int ui_events_recent(UiEvent *out, int max);

/** @brief Contador que muda a cada novo aviso (para atualizar a tela). */
uint32_t ui_events_version(void);

#endif // UI_EVENTS_H
