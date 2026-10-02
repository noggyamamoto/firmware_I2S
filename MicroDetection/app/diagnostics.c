/*
 * ============================================================================
 * app/diagnostics – implementação
 * ============================================================================
 */
#include "diagnostics.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "app_state.h"
#include "config.h"
#include "hal_time.h"
#include "link_service.h"

static void set_title(DiagProblem *p, UiEventLevel level, const char *fmt, ...) {
    memset(p, 0, sizeof(*p));
    p->level = level;
    va_list args;
    va_start(args, fmt);
    vsnprintf(p->title, sizeof(p->title), fmt, args);
    va_end(args);
}

static void add_action(DiagProblem *p, const char *fmt, ...) {
    if (p->n_actions >= DIAG_MAX_ACTIONS) return;
    va_list args;
    va_start(args, fmt);
    vsnprintf(p->actions[p->n_actions++], sizeof(p->actions[0]), fmt, args);
    va_end(args);
}

const char *diagnostics_auth_name(DrvWifiAuth auth) {
    switch (auth) {
        case DRV_WIFI_AUTH_OPEN:        return "aberta";
        case DRV_WIFI_AUTH_WEP:         return "WEP";
        case DRV_WIFI_AUTH_WPA2:        return "WPA2";
        case DRV_WIFI_AUTH_WPA3:        return "WPA3";
        case DRV_WIFI_AUTH_WPA2_WPA3:   return "WPA2/WPA3";
        case DRV_WIFI_AUTH_ENTERPRISE:  return "corporativa";
        default:                        return "outra";
    }
}

const char *diagnostics_mic_text(MicStatus status) {
    switch (status) {
        case MIC_STATUS_STARTING:       return "verificando";
        case MIC_STATUS_OK:             return "OK";
        case MIC_STATUS_NO_SIGNAL:      return "SEM SINAL";
        case MIC_STATUS_READ_ERROR:     return "ERRO DE LEITURA (I2S)";
        case MIC_STATUS_CLIPPING:       return "SATURADO";
        case MIC_STATUS_INIT_FAILED:    return "NÃO INICIOU";
    }
    return "?";
}

void diagnostics_wifi_error_text(const HalWifiDiag *d, char *buf, int len) {
    switch (d->error) {
        case HAL_WIFI_ERR_NONE:             snprintf(buf, len, "sem erro"); break;
        case HAL_WIFI_ERR_NOT_CONFIGURED:   snprintf(buf, len, "nenhuma rede cadastrada"); break;
        case HAL_WIFI_ERR_INVALID_PASSWORD: snprintf(buf, len, "senha com tamanho inválido"); break;
        case HAL_WIFI_ERR_SSID_NOT_FOUND:   snprintf(buf, len, "rede \"%s\" não encontrada", d->ssid); break;
        case HAL_WIFI_ERR_WRONG_PASSWORD:   snprintf(buf, len, "senha incorreta para \"%s\"", d->ssid); break;
        case HAL_WIFI_ERR_SECURITY:         snprintf(buf, len, "segurança da rede incompatível"); break;
        case HAL_WIFI_ERR_WEAK_SIGNAL:      snprintf(buf, len, "sinal fraco da rede \"%s\"", d->ssid); break;
        case HAL_WIFI_ERR_REJECTED:         snprintf(buf, len, "o modem recusou a conexão"); break;
        case HAL_WIFI_ERR_NO_IP:            snprintf(buf, len, "o modem não entregou endereço IP"); break;
        case HAL_WIFI_ERR_CONNECTION_LOST:  snprintf(buf, len, "a conexão com \"%s\" caiu", d->ssid); break;
        case HAL_WIFI_ERR_OTHER:            snprintf(buf, len, "falha de conexão (código %u)", d->reason); break;
    }
}

/* Problema de Wi-Fi atual (retorna 0 se está tudo certo). */
static int wifi_problem(DiagProblem *p) {
    HalWifiDiag d;
    hal_wifi_get_diag(&d);
    if (d.state == HAL_WIFI_CONNECTED) {
        int8_t rssi = hal_wifi_rssi();
        if (rssi != 0 && rssi < -75) {
            set_title(p, UI_EV_WARN, "Sinal Wi-Fi fraco (%d dBm): as notas podem atrasar", rssi);
            add_action(p, "Aproxime o dispositivo do modem (ideal: acima de -70 dBm).");
            add_action(p, "Evite paredes e objetos de metal entre o dispositivo e o modem.");
            return 1;
        }
        return 0;
    }
    if (d.error == HAL_WIFI_ERR_NONE) {                 // Primeira tentativa em andamento
        if (d.scanning) return 0;
        set_title(p, UI_EV_INFO, "Conectando a \"%s\"... (tentativa %u de %u)", d.ssid, d.attempt,
                  d.max_attempts);
        add_action(p, "Aguarde alguns segundos. Detalhes: Menu 2 > 4.");
        return 1;
    }

    UiEventLevel level = UI_EV_ERROR;
    switch (d.error) {
        case HAL_WIFI_ERR_NOT_CONFIGURED:
            set_title(p, level, "Nenhuma rede Wi-Fi cadastrada");
            add_action(p, "Menu 2 > 1: escolha a rede na lista e digite a senha.");
            break;
        case HAL_WIFI_ERR_INVALID_PASSWORD:
            set_title(p, level, "Senha da rede \"%s\" com tamanho inválido", d.ssid);
            add_action(p, "Senhas WPA/WPA2 têm de 8 a 63 caracteres. Confira no modem.");
            add_action(p, "Menu 2 > 1: escolha a rede e digite a senha novamente.");
            break;
        case HAL_WIFI_ERR_SSID_NOT_FOUND:
            set_title(p, level, "Rede \"%s\" não encontrada", d.ssid);
            if (d.similar_ssid[0]) {
                add_action(p, "Existe a rede \"%s\" (nome difere em maiúsculas/espaços): "
                              "escolha-a no Menu 2 > 1.", d.similar_ssid);
            }
            add_action(p, "O ESP32 só vê redes de 2,4 GHz: se o modem separa \"Casa\" e "
                          "\"Casa_5G\", use a de 2,4 GHz.");
            add_action(p, "Confira o nome exato no Menu 2 > 1 e se o modem está ligado e perto.");
            break;
        case HAL_WIFI_ERR_WRONG_PASSWORD:
            set_title(p, level, "Senha incorreta para a rede \"%s\"", d.ssid);
            add_action(p, "Menu 2 > 1: digite a senha de novo (maiúsculas contam; TAB mostra).");
            add_action(p, "Se a senha estiver certa, reinicie o modem e use o Menu 2 > 3.");
            add_action(p, "Modem em \"somente WPA3\"? Troque para \"WPA2/WPA3\" ou \"WPA2\".");
            break;
        case HAL_WIFI_ERR_SECURITY:
            set_title(p, level, "Segurança da rede \"%s\" incompatível (%s)", d.ssid,
                      diagnostics_auth_name(d.ssid_auth));
            add_action(p, "No modem, use WPA2-Pessoal ou WPA2/WPA3 (redes corporativas não).");
            add_action(p, "Depois use o Menu 2 > 3 para tentar de novo.");
            break;
        case HAL_WIFI_ERR_WEAK_SIGNAL:
            if (d.ssid_seen) {
                set_title(p, level, "Sinal fraco da rede \"%s\" (%d dBm)", d.ssid, d.ssid_rssi);
            } else {
                set_title(p, level, "Sinal fraco da rede \"%s\"", d.ssid);
            }
            add_action(p, "Aproxime o dispositivo do modem (ideal: acima de -70 dBm).");
            add_action(p, "Evite paredes e objetos de metal entre eles; depois Menu 2 > 3.");
            break;
        case HAL_WIFI_ERR_REJECTED: {
            uint8_t mac[6];
            hal_wifi_mac(mac);
            set_title(p, level, "O modem recusou a conexão (código %u)", d.reason);
            add_action(p, "Filtro de MAC ou limite de aparelhos no modem? MAC: %02X:%02X:%02X:%02X:%02X:%02X",
                       mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
            add_action(p, "Reinicie o modem e use o Menu 2 > 3 para tentar de novo.");
            break;
        }
        case HAL_WIFI_ERR_NO_IP:
            set_title(p, level, "Conectou a \"%s\", mas o modem não entregou IP", d.ssid);
            add_action(p, "Verifique se o DHCP do modem está ligado e se há IPs livres.");
            add_action(p, "Reinicie o modem e use o Menu 2 > 3.");
            break;
        case HAL_WIFI_ERR_CONNECTION_LOST:
            set_title(p, UI_EV_WARN, "A conexão com \"%s\" caiu (código %u)", d.ssid, d.reason);
            add_action(p, "Reconectando automaticamente. Se repetir, aproxime do modem.");
            break;
        default:
            set_title(p, level, "Falha ao conectar em \"%s\" (código %u)", d.ssid, d.reason);
            add_action(p, "Menu 2 > 3 para tentar de novo ou reinicie o modem.");
            break;
    }
    if (d.state == HAL_WIFI_AP_ONLY) {
        add_action(p, "Enquanto isso: celular na rede \"%s\" (senha %s), app no IP 192.168.4.1.",
                   hal_wifi_device_name(), WIFI_AP_PASS);
    } else if (d.error != HAL_WIFI_ERR_NOT_CONFIGURED) {
        add_action(p, "Tentando de novo (tentativa %u de %u)...", d.attempt, d.max_attempts);
    }
    return 1;
}

static int mic_problem(DiagProblem *p) {
    switch (audio_pipeline_mic_status()) {
        case MIC_STATUS_NO_SIGNAL:
            set_title(p, UI_EV_ERROR, "Microfone sem sinal: nenhuma nota será detectada");
            add_action(p, "Confira os fios do INMP441: SD->GPIO%d, SCK->GPIO%d, WS->GPIO%d.",
                       I2S_DATA_IN_PIN, I2S_BCK_PIN, I2S_WS_PIN);
            add_action(p, "VDD no 3V3, GND comum e L/R no GND (canal esquerdo).");
            add_action(p, "Depois confira o nível no Menu 3 > 3 (afinador).");
            return 1;
        case MIC_STATUS_READ_ERROR:
            set_title(p, UI_EV_ERROR, "O microfone (I2S) não está entregando amostras");
            add_action(p, "Reinicie o dispositivo (Menu 5).");
            add_action(p, "Se persistir, confira se os pinos I2S do config.h estão livres.");
            return 1;
        case MIC_STATUS_INIT_FAILED:
            set_title(p, UI_EV_ERROR, "Não foi possível iniciar o microfone (I2S)");
            add_action(p, "Confira os pinos I2S no config.h e grave o firmware de novo.");
            add_action(p, "Sem microfone as notas não são detectadas; o resto funciona.");
            return 1;
        case MIC_STATUS_CLIPPING:
            set_title(p, UI_EV_WARN, "Áudio saturado: o som chega alto demais ao microfone");
            add_action(p, "Afaste o microfone do alto-falante ou baixe o volume do teclado.");
            add_action(p, "Se necessário, reduza INPUT_GAIN no config.h.");
            return 1;
        default:
            return 0;
    }
}

/* Falhas de envio ao app e eventos perdidos nos últimos segundos. */
static int link_problem(DiagProblem *p) {
    static uint32_t last_errors = 0, last_dropped = 0;
    static int64_t window_start = 0;
    static int recent_errors = 0, recent_dropped = 0;
    int64_t now = hal_time_us();
    uint32_t errors = link_service_send_errors();
    uint32_t dropped = app_state_dropped_events();
    if (now - window_start > 5000000) {             // Janela de 5 s
        recent_errors = (int)(errors - last_errors);
        recent_dropped = (int)(dropped - last_dropped);
        last_errors = errors;
        last_dropped = dropped;
        window_start = now;
    }
    if (!app_state_is_paired()) return 0;
    if (recent_dropped > 0) {
        set_title(p, UI_EV_WARN, "Rede lenta: %d eventos não puderam ser enviados ao app",
                  recent_dropped);
        add_action(p, "Aproxime o dispositivo e o celular do modem.");
        add_action(p, "Desligue o envio de áudio bruto (Menu 3 > 5), se estiver ligado.");
        return 1;
    }
    if (recent_errors > 5) {
        set_title(p, UI_EV_WARN, "Falhas ao enviar dados ao app (%d em 5 s)", recent_errors);
        add_action(p, "Confira se o app continua aberto e na mesma rede Wi-Fi.");
        return 1;
    }
    return 0;
}

/* Orientação quando a rede está ok mas nenhum app conectou. */
static int pairing_hint(DiagProblem *p) {
    if (app_state_is_paired() || hal_wifi_state() == HAL_WIFI_CONNECTING) return 0;
    char ip[24];
    hal_wifi_ip_str(ip, sizeof(ip));
    set_title(p, UI_EV_INFO, "Pronto. Aguardando o app se conectar");
    add_action(p, "No app: Conectar dispositivo > Buscar (celular na mesma rede Wi-Fi).");
    add_action(p, "Não apareceu? Use \"Conectar pelo IP\" com %s.", ip);
    return 1;
}

int diagnostics_collect(DiagProblem *out, int max) {
    int n = 0;
    if (n < max) n += mic_problem(&out[n]);
    if (n < max) n += wifi_problem(&out[n]);
    if (n < max) n += link_problem(&out[n]);
    if (n == 0 && max > 0) n += pairing_hint(&out[n]);
    return n;
}
