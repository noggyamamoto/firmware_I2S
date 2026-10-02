/*
 * ============================================================================
 * hal/hal_wifi – conexão Wi-Fi (RFE01 / RU01)
 *
 *  1. Varre as redes disponíveis (o resultado fica guardado para diagnóstico)
 *  2. Conecta à rede configurada, com novas tentativas espaçadas (backoff)
 *  3. Se não conseguir, ativa uma rede própria (modo AP) "PartituraIoT-XXXX",
 *     permitindo que o celular/computador se conecte diretamente ao
 *     dispositivo, e continua tentando a rede configurada em segundo plano
 *     (sem atrapalhar quem estiver usando a rede própria)
 *  4. Classifica cada falha (senha incorreta, rede inexistente, sinal fraco,
 *     segurança incompatível, roteador recusou, sem IP) para que o menu
 *     explique ao usuário o que fazer
 *  5. A cada mudança de estado avisa a aplicação (callback), que atualiza o
 *     LED de status
 * ============================================================================
 */
#ifndef HAL_WIFI_H
#define HAL_WIFI_H

#include <stdbool.h>
#include <stdint.h>

#include "drv_wifi.h"
#include "hal_storage.h"

typedef enum {
    HAL_WIFI_CONNECTING,                // Tentando a rede configurada
    HAL_WIFI_CONNECTED,                 // Conectado à rede configurada
    HAL_WIFI_AP_ONLY,                   // Somente a rede própria está ativa
} HalWifiState;

/** Causa da última falha de conexão. */
typedef enum {
    HAL_WIFI_ERR_NONE,
    HAL_WIFI_ERR_NOT_CONFIGURED,        // Nenhuma rede cadastrada
    HAL_WIFI_ERR_INVALID_PASSWORD,      // Senha com tamanho inválido (WPA2: 8 a 63)
    HAL_WIFI_ERR_SSID_NOT_FOUND,        // Rede não encontrada (nome, 5 GHz, desligada, longe)
    HAL_WIFI_ERR_WRONG_PASSWORD,        // Autenticação recusada / handshake sem resposta
    HAL_WIFI_ERR_SECURITY,              // Segurança incompatível (ex.: rede corporativa)
    HAL_WIFI_ERR_WEAK_SIGNAL,           // Sinal fraco demais
    HAL_WIFI_ERR_REJECTED,              // Roteador recusou (filtro MAC, limite de aparelhos)
    HAL_WIFI_ERR_NO_IP,                 // Associado, mas o roteador não entregou IP (DHCP)
    HAL_WIFI_ERR_CONNECTION_LOST,       // Estava conectado e a conexão caiu
    HAL_WIFI_ERR_OTHER,                 // Outro motivo (ver código)
} HalWifiError;

/** Situação da conexão para o menu e o diagnóstico. */
typedef struct {
    HalWifiState state;
    HalWifiError error;                 // Última falha (NONE quando conectado)
    uint16_t     reason;                // Código bruto do ESP-IDF da última falha
    uint8_t      attempt;               // Tentativa atual
    uint8_t      max_attempts;          // Tentativas antes da rede própria
    bool         scanning;              // Varredura em andamento
    char         ssid[33];              // Rede configurada
    // Dados da rede configurada na última varredura
    bool         ssid_seen;             // Apareceu na varredura
    int8_t       ssid_rssi;
    uint8_t      ssid_channel;
    DrvWifiAuth  ssid_auth;
    char         similar_ssid[33];      // Nome parecido encontrado (maiúsculas, espaços)
    uint8_t      ap_clients;            // Aparelhos na rede própria
} HalWifiDiag;

typedef void (*hal_wifi_state_cb)(HalWifiState state);

/** @brief Inicializa o rádio e inicia a conexão. */
bool hal_wifi_start(const WifiCredentials *cred, hal_wifi_state_cb on_state);

/** @brief Troca as credenciais e reconecta. */
void hal_wifi_reconnect(const WifiCredentials *cred);

/**
 * @brief Varre as redes (bloqueante, ~2–3 s). Pausa as tentativas de conexão
 *        durante a varredura. @return quantidade de redes ou -1.
 */
int hal_wifi_scan(DrvWifiAp *out, int max);

/** @brief Valida uma senha antes de salvar. @return HAL_WIFI_ERR_NONE se ok. */
HalWifiError hal_wifi_check_password(const char *password);

void hal_wifi_get_diag(HalWifiDiag *out);
HalWifiState hal_wifi_state(void);
bool hal_wifi_is_connected(void);
bool hal_wifi_ap_active(void);
int8_t hal_wifi_rssi(void);
void hal_wifi_ip_str(char *buf, int len);
void hal_wifi_mac(uint8_t mac[6]);
const char *hal_wifi_device_name(void);

/** @brief Desliga o Wi-Fi e libera os recursos. */
void hal_wifi_stop(void);

#endif // HAL_WIFI_H
