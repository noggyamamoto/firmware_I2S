/*
 * ============================================================================
 * app/serial_ui – menu serial de controle e diagnóstico (115200 bps)
 *
 * O terminal mostra somente o menu: a tela é redesenhada no lugar (códigos
 * ANSI) em vermelho e branco, com o estado do dispositivo no topo, os
 * problemas atuais com o que fazer para resolvê-los e as opções.
 *
 *  - Navegação: setas ↑/↓ + ENTER, ou o número da opção; ESC/← volta
 *  - Após qualquer interação, 20 s sem tecla reiniciam o menu
 *    (SERIAL_UI_IDLE_RESET_MS), descartando o que estava sendo digitado
 *  - Telas ao vivo: status, detalhes do Wi-Fi, afinador, monitor de notas
 *    e log técnico
 *
 * Funciona no monitor do PlatformIO (monitor_filters = direct), PuTTY,
 * screen/minicom. Em terminais sem ANSI (ex.: Monitor Serial da IDE
 * Arduino) use SERIAL_UI_ANSI 0 no config.h.
 * ============================================================================
 */
#ifndef SERIAL_UI_H
#define SERIAL_UI_H

/** @brief Limpa o terminal e mostra a tela de inicialização (antes das tarefas). */
void serial_ui_splash(void);

/** @brief Mostra uma falha que impede o funcionamento e o que fazer. */
void serial_ui_fatal(const char *what, const char *action);

/** @brief Cria a tarefa do menu (núcleo 0, prioridade baixa). */
void serial_ui_start_task(void);

#endif // SERIAL_UI_H
