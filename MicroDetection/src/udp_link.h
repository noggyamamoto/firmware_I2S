/*
 * ============================================================================
 * Enlace UDP com o app Flutter (RFE06 / RFA01)
 *
 *  - net_rx_task (núcleo 0): recebe e trata comandos do app
 *  - net_tx_task (núcleo 0): consome a fila de saída, monta e envia pacotes
 *
 * O formato binário dos pacotes está em protocol.h.
 * ============================================================================
 */
#ifndef UDP_LINK_H
#define UDP_LINK_H

#include <stdbool.h>
#include <stdint.h>

/** @brief Cria o socket UDP na porta PROTO_DEVICE_PORT. */
bool udp_link_init(void);

/** @brief Cria as tarefas de recepção e transmissão. */
void udp_link_start_tasks(void);

/** @brief Estatísticas para o menu serial. */
uint32_t udp_link_packets_sent(void);
uint32_t udp_link_send_errors(void);

/** @brief Fecha o socket. */
void udp_link_deinit(void);

#endif // UDP_LINK_H
