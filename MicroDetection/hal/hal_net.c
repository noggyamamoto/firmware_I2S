/*
 * ============================================================================
 * hal/hal_net – implementação com lwIP (UDP) e esp_http_server (WebSocket)
 * ============================================================================
 */
#include "hal_net.h"

#include <stdio.h>
#include <string.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

#include "config.h"
#include "protocol.h"

static const char *TAG = "net";

#define RX_BUFFER_SIZE  128                     // Maior comando do app

static int s_sock = -1;                         // Socket UDP (recepção e envio)
static httpd_handle_t s_http = NULL;            // Servidor HTTP/WebSocket
static hal_net_rx_cb s_on_receive = NULL;
static volatile uint32_t s_sent = 0;
static volatile uint32_t s_errors = 0;

// ======================= UDP ================================================

static void udp_rx_task(void *arg) {
    (void)arg;
    uint8_t buf[RX_BUFFER_SIZE];
    while (1) {
        struct sockaddr_in from;
        socklen_t from_len = sizeof(from);
        int len = recvfrom(s_sock, buf, sizeof(buf), 0, (struct sockaddr *)&from, &from_len);
        if (len > 0 && s_on_receive) {
            NetPeer peer = {
                .kind = NET_PEER_UDP,
                .ip = from.sin_addr.s_addr,
                .port = from.sin_port,
                .ws_fd = -1,
            };
            s_on_receive(&peer, buf, (size_t)len);
        }
    }
}

static bool udp_open(void) {
    s_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_sock < 0) {
        ESP_LOGE(TAG, "Falha ao criar socket UDP");
        return false;
    }
    int yes = 1;
    setsockopt(s_sock, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes));

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PROTO_DEVICE_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(s_sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "Falha no bind da porta %d", PROTO_DEVICE_PORT);
        close(s_sock);
        s_sock = -1;
        return false;
    }
    ESP_LOGI(TAG, "Aguardando o app na porta UDP %d", PROTO_DEVICE_PORT);
    return true;
}

// ======================= WEBSOCKET ==========================================

#if CONFIG_HTTPD_WS_SUPPORT

static esp_err_t ws_handler(httpd_req_t *req) {
    if (req->method == HTTP_GET) {
        ESP_LOGI(TAG, "Cliente WebSocket conectado (fd %d)", httpd_req_to_sockfd(req));
        return ESP_OK;                              // Handshake concluído
    }

    uint8_t buf[RX_BUFFER_SIZE];
    httpd_ws_frame_t frame = {0};
    esp_err_t err = httpd_ws_recv_frame(req, &frame, 0);    // Somente o tamanho
    if (err != ESP_OK) return err;
    if (frame.len > sizeof(buf)) return ESP_ERR_INVALID_SIZE;
    frame.payload = buf;
    err = httpd_ws_recv_frame(req, &frame, frame.len);
    if (err != ESP_OK) return err;

    if (frame.type == HTTPD_WS_TYPE_BINARY && s_on_receive) {
        int fd = httpd_req_to_sockfd(req);
        struct sockaddr_in6 addr;                   // lwIP pode usar IPv4 mapeado em IPv6
        socklen_t addr_len = sizeof(addr);
        uint32_t ip = 0;
        if (getpeername(fd, (struct sockaddr *)&addr, &addr_len) == 0) {
            if (addr.sin6_family == AF_INET) {
                ip = ((struct sockaddr_in *)&addr)->sin_addr.s_addr;
            } else {
                ip = addr.sin6_addr.un.u32_addr[3];
            }
        }
        NetPeer peer = {.kind = NET_PEER_WEBSOCKET, .ip = ip, .port = 0, .ws_fd = fd};
        s_on_receive(&peer, buf, frame.len);
    }
    return ESP_OK;
}

// Página simples de diagnóstico em http://IP/
static esp_err_t root_handler(httpd_req_t *req) {
    static const char page[] =
        "PartituraIoT " FIRMWARE_VERSION "\n"
        "WebSocket: ws://<IP>/ws (pacotes binarios do protocol.h)\n"
        "UDP: porta 54322\n";
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, page, HTTPD_RESP_USE_STRLEN);
}

static bool ws_open(void) {
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = WS_PORT;
    cfg.core_id = 0;                                // Rede no núcleo 0 (áudio no 1)
    cfg.lru_purge_enable = true;
    if (httpd_start(&s_http, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao iniciar o servidor WebSocket");
        return false;
    }
    const httpd_uri_t ws = {
        .uri = "/ws",
        .method = HTTP_GET,
        .handler = ws_handler,
        .is_websocket = true,
    };
    const httpd_uri_t root = {.uri = "/", .method = HTTP_GET, .handler = root_handler};
    httpd_register_uri_handler(s_http, &ws);
    httpd_register_uri_handler(s_http, &root);
    ESP_LOGI(TAG, "Aguardando o app web em ws://<IP>:%d/ws", WS_PORT);
    return true;
}

static bool ws_send(int fd, const void *data, size_t len) {
    if (!s_http || httpd_ws_get_fd_info(s_http, fd) != HTTPD_WS_CLIENT_WEBSOCKET) return false;
    httpd_ws_frame_t frame = {
        .final = true,
        .fragmented = false,
        .type = HTTPD_WS_TYPE_BINARY,
        .payload = (uint8_t *)data,
        .len = len,
    };
    return httpd_ws_send_frame_async(s_http, fd, &frame) == ESP_OK;
}

#else

static bool ws_open(void) {
    ESP_LOGW(TAG, "WebSocket desativado (CONFIG_HTTPD_WS_SUPPORT): app web indisponível");
    return true;
}

static bool ws_send(int fd, const void *data, size_t len) {
    (void)fd; (void)data; (void)len;
    return false;
}

#endif

// ======================= INTERFACE ==========================================

bool hal_net_init(hal_net_rx_cb on_receive) {
    s_on_receive = on_receive;
    if (!udp_open()) return false;
    ws_open();                                      // Opcional: falha não impede o UDP
    return true;
}

void hal_net_start(void) {
    xTaskCreatePinnedToCore(udp_rx_task, "net_rx", 4096, NULL, 3, NULL, 0);
}

bool hal_net_send(const NetPeer *to, const void *data, size_t len) {
    bool ok = false;
    if (to->kind == NET_PEER_UDP && s_sock >= 0) {
        struct sockaddr_in dest = {0};
        dest.sin_family = AF_INET;
        dest.sin_addr.s_addr = to->ip;
        dest.sin_port = to->port;
        ok = sendto(s_sock, data, len, 0, (const struct sockaddr *)&dest, sizeof(dest)) == (int)len;
    } else if (to->kind == NET_PEER_WEBSOCKET) {
        ok = ws_send(to->ws_fd, data, len);
    }
    if (ok) {
        s_sent++;
    } else {
        s_errors++;
    }
    return ok;
}

bool hal_net_peer_equal(const NetPeer *a, const NetPeer *b) {
    if (a->kind != b->kind) return false;
    if (a->kind == NET_PEER_WEBSOCKET) return a->ws_fd == b->ws_fd;
    return a->ip == b->ip && a->port == b->port;
}

void hal_net_peer_str(const NetPeer *peer, char *buf, int len) {
    const uint8_t *ip = (const uint8_t *)&peer->ip;
    if (peer->kind == NET_PEER_WEBSOCKET) {
        snprintf(buf, len, "ws %u.%u.%u.%u (fd %d)", ip[0], ip[1], ip[2], ip[3], peer->ws_fd);
    } else {
        snprintf(buf, len, "udp %u.%u.%u.%u:%u", ip[0], ip[1], ip[2], ip[3], ntohs(peer->port));
    }
}

uint32_t hal_net_packets_sent(void) { return s_sent; }
uint32_t hal_net_send_errors(void) { return s_errors; }

void hal_net_deinit(void) {
    if (s_http) {
        httpd_stop(s_http);
        s_http = NULL;
    }
    if (s_sock >= 0) {
        close(s_sock);
        s_sock = -1;
    }
}
