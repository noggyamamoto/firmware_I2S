/*
 * ============================================================================
 * drivers/drv_wifi – rádio Wi-Fi (estação e ponto de acesso)
 *
 * Operações diretas do rádio: inicialização da pilha de rede, modos STA/AP,
 * conexão, varredura e leitura de RSSI/MAC/IP. A política de conexão
 * (novas tentativas, rede própria de reserva, LED de status) fica na HAL
 * (hal_wifi).
 * ============================================================================
 */
#ifndef DRV_WIFI_H
#define DRV_WIFI_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    DRV_WIFI_EVENT_STA_DISCONNECTED,    // Estação desconectada (arg = motivo)
    DRV_WIFI_EVENT_STA_GOT_IP,          // Endereço IP recebido (arg = IPv4)
    DRV_WIFI_EVENT_AP_CLIENT_JOINED,    // Um aparelho entrou na rede própria
} DrvWifiEvent;

typedef void (*drv_wifi_event_cb)(DrvWifiEvent event, uint32_t arg);

/** Rede encontrada na varredura. */
typedef struct {
    char ssid[33];
    int8_t rssi;
    uint8_t channel;
} DrvWifiAp;

/** @brief Inicializa netif, loop de eventos e o driver (sem economia de energia). */
bool drv_wifi_init(drv_wifi_event_cb callback);

/** @brief Liga o rádio no modo estação. */
bool drv_wifi_start(void);

/** @brief Define a rede (SSID/senha) usada pela estação. */
void drv_wifi_set_station(const char *ssid, const char *password);

/** @brief Ativa o ponto de acesso (modo AP+STA). */
void drv_wifi_enable_ap(const char *ssid, const char *password, uint8_t channel);

/** @brief Desativa o ponto de acesso (volta ao modo estação). */
void drv_wifi_disable_ap(void);

void drv_wifi_connect(void);
void drv_wifi_disconnect(void);

/** @brief Varredura bloqueante (~2 s). @return quantidade de redes ou -1. */
int drv_wifi_scan(DrvWifiAp *out, int max);

/** @brief RSSI da rede conectada (0 se desconectado). */
int8_t drv_wifi_rssi(void);

/** @brief Endereço MAC da estação. */
void drv_wifi_mac(uint8_t mac[6]);

/** @brief Desliga o rádio e libera o driver. */
void drv_wifi_deinit(void);

#endif // DRV_WIFI_H
