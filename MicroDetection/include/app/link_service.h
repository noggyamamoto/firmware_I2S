/*
 * ============================================================================
 * app/link_service – serviço de comunicação com o app Flutter (RFE06 / RFA01)
 *
 *  - Recebe os comandos do app (DISCOVER, CONNECT, PING, SESSION_START...)
 *    por UDP ou WebSocket (hal_net) e altera o estado da aplicação
 *  - net_tx (núcleo 0): consome a fila de saída, monta os pacotes do
 *    protocolo e envia ao app pareado (ou ao remetente, nas respostas)
 *  - Encerra o pareamento quando o app para de enviar PING
 *
 * O formato binário dos pacotes está em protocol.h.
 * ============================================================================
 */
#ifndef LINK_SERVICE_H
#define LINK_SERVICE_H

#include <stdbool.h>
#include <stdint.h>

/** @brief Abre os transportes (UDP e WebSocket). */
bool link_service_init(void);

/** @brief Cria as tarefas de recepção e transmissão e o monitor de PING. */
void link_service_start(void);

/** @brief Estatísticas para o menu serial. */
uint32_t link_service_packets_sent(void);
uint32_t link_service_send_errors(void);

/** @brief Fecha os transportes. */
void link_service_deinit(void);

#endif // LINK_SERVICE_H
