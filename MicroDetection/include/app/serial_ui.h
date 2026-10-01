/*
 * ============================================================================
 * app/serial_ui – interface serial de controle e diagnóstico (115200 bps)
 *
 * Menu de texto no monitor serial: captura local, status, varredura e
 * configuração do Wi-Fi, metrônomo de teste, afinador e reinício.
 * ============================================================================
 */
#ifndef SERIAL_UI_H
#define SERIAL_UI_H

/** @brief Cria a tarefa do menu (núcleo 0, prioridade baixa). */
void serial_ui_start_task(void);

#endif // SERIAL_UI_H
