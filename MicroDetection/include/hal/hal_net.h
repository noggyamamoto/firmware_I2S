/*
 * ============================================================================
 * hal/hal_net – enlace de dados com o app (UDP e WebSocket)
 *
 * Os pacotes do protocolo (protocol.h) trafegam por dois transportes com a
 * mesma interface de "datagramas":
 *
 *  - UDP, porta PROTO_DEVICE_PORT: app no celular e no computador
 *    (descoberta por broadcast);
 *  - WebSocket, ws://IP:WS_PORT/ws: app na web (o navegador não tem UDP –
 *    RNFA02). Cada mensagem binária carrega um pacote.
 *
 * A aplicação recebe os pacotes por callback e responde para o NetPeer de
 * origem, sem saber qual transporte foi usado.
 * ============================================================================
 */
#ifndef HAL_NET_H
#define HAL_NET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    NET_PEER_NONE = 0,
    NET_PEER_UDP,
    NET_PEER_WEBSOCKET,
} NetPeerKind;

/** Endereço de um app (origem/destino de pacotes). */
typedef struct {
    NetPeerKind kind;
    uint32_t ip;            // IPv4 (ordem de rede)
    uint16_t port;          // Porta UDP (ordem de rede)
    int ws_fd;              // Socket do cliente WebSocket
} NetPeer;

typedef void (*hal_net_rx_cb)(const NetPeer *from, const uint8_t *data, size_t len);

/** @brief Abre o socket UDP e o servidor WebSocket. */
bool hal_net_init(hal_net_rx_cb on_receive);

/** @brief Cria a tarefa de recepção UDP (núcleo 0). */
void hal_net_start(void);

/** @brief Envia um pacote para o app. */
bool hal_net_send(const NetPeer *to, const void *data, size_t len);

/** @brief Compara dois endereços. */
bool hal_net_peer_equal(const NetPeer *a, const NetPeer *b);

/** @brief Texto do endereço (ex.: "udp 192.168.0.10:50000"). */
void hal_net_peer_str(const NetPeer *peer, char *buf, int len);

/** @brief Estatísticas para o menu serial. */
uint32_t hal_net_packets_sent(void);
uint32_t hal_net_send_errors(void);

/** @brief Fecha o socket e o servidor. */
void hal_net_deinit(void);

#endif // HAL_NET_H
