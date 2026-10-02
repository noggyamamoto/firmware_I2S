/*
 * ============================================================================
 * Protocolo de comunicação entre o dispositivo embarcado e o app Flutter
 *
 * Todos os pacotes começam com o mesmo cabeçalho de 12 bytes e usam
 * ordem de bytes little-endian (nativa do ESP32 e das plataformas do app).
 *
 * Transportes (hal_net):
 *  - UDP na porta PROTO_DEVICE_PORT (app no celular/computador). O app envia
 *    os comandos a partir de um único socket e o dispositivo responde para
 *    o IP/porta de origem do último CONNECT recebido. A descoberta é feita
 *    por broadcast (DISCOVER) e respondida com ANNOUNCE.
 *  - WebSocket em ws://IP:WS_PORT/ws (app na web, sem UDP no navegador):
 *    cada mensagem binária carrega exatamente um pacote.
 *
 * O mesmo protocolo está espelhado no app em:
 *   lib/features/connection/data/protocol/device_protocol.dart
 * ============================================================================
 */
#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdint.h>

#define PROTO_MAGIC             0x5443u     // 'T' 'C' (little-endian: 0x43 0x54)
#define PROTO_VERSION           1           // Versão do protocolo
#define PROTO_DEVICE_PORT       54322       // Porta em que o dispositivo escuta comandos
#define PROTO_NAME_LEN          24          // Tamanho fixo dos campos de nome
#define PROTO_FW_LEN            12          // Tamanho fixo do campo de versão do firmware

// ----------------------------- Tipos de pacote ------------------------------
// App -> Dispositivo
#define PKT_DISCOVER            0x01        // Broadcast de busca de dispositivos
#define PKT_CONNECT             0x02        // Pareamento com o app
#define PKT_DISCONNECT          0x03        // Desconexão segura (RU17)
#define PKT_PING                0x04        // Heartbeat (a cada ~1 s)
#define PKT_CONFIG              0x05        // Configuração do metrônomo e streams
#define PKT_SESSION_START       0x06        // Início da execução (zera o relógio)
#define PKT_SESSION_STOP        0x07        // Fim/interrupção da execução
#define PKT_SET_TEMPO           0x08        // Altera o BPM na próxima batida

// Dispositivo -> App
#define PKT_ANNOUNCE            0x81        // Resposta ao DISCOVER
#define PKT_CONNECT_ACK         0x82        // Resposta ao CONNECT
#define PKT_PONG                0x83        // Resposta ao PING (status)
#define PKT_NOTE_ON             0x84        // Início de nota detectada
#define PKT_NOTE_OFF            0x85        // Fim de nota detectada
#define PKT_PITCH               0x86        // Estimativa contínua de altura (afinador)
#define PKT_BEAT                0x87        // Batida do metrônomo
#define PKT_AUDIO_FRAME         0x88        // Quadro de áudio bruto (modo diagnóstico)

// ----------------------------- Flags de configuração ------------------------
#define CFG_FLAG_METRO_SOUND    (1u << 0)   // Metrônomo sonoro (buzzer) – RFE02
#define CFG_FLAG_METRO_VISUAL   (1u << 1)   // Metrônomo visual (LED RGB) – RFE03
#define CFG_FLAG_STREAM_AUDIO   (1u << 2)   // Envia quadros de áudio brutos
#define CFG_FLAG_STREAM_PITCH   (1u << 3)   // Envia PKT_PITCH a cada janela

// ----------------------------- Estados do dispositivo -----------------------
#define DEV_STATE_IDLE          0           // Sem app pareado
#define DEV_STATE_CONNECTED     1           // App pareado, aguardando execução
#define DEV_STATE_SESSION       2           // Execução em andamento

#pragma pack(push, 1)

/* Cabeçalho comum a todos os pacotes (12 bytes). */
typedef struct {
    uint16_t magic;             // PROTO_MAGIC
    uint8_t  version;           // PROTO_VERSION
    uint8_t  type;              // PKT_*
    uint32_t sequence;          // Número sequencial (detecção de perdas)
    uint32_t timestamp_ms;      // Tempo desde o início da sessão (ou do boot)
} PacketHeader;

/* PKT_CONNECT */
typedef struct {
    PacketHeader h;
    char app_name[PROTO_NAME_LEN];      // Nome do app/dispositivo cliente
} PktConnect;

/* PKT_CONFIG */
typedef struct {
    PacketHeader h;
    uint16_t bpm;                       // Andamento em batidas por minuto
    uint8_t  beats_per_bar;             // 2 = binário, 3 = ternário, 4 = quaternário
    uint8_t  flags;                     // CFG_FLAG_*
} PktConfig;

/* PKT_SESSION_START
 * session_id identifica a execução: um SESSION_START repetido com o mesmo
 * id (reenvio por perda de pacote) é ignorado e NÃO zera o relógio de novo,
 * o que manteria o app e o dispositivo dessincronizados. 0 = sem id. */
typedef struct {
    PacketHeader h;
    uint16_t bpm;                       // Andamento inicial
    uint8_t  beats_per_bar;             // Fórmula de compasso (numerador)
    uint8_t  count_in_bars;             // Compassos de contagem (RFA05 = 2)
    uint8_t  flags;                     // CFG_FLAG_*
    uint8_t  session_id;                // Identificador da execução (1..255)
    uint8_t  reserved[2];
} PktSessionStart;

/* PKT_SET_TEMPO
 * at_beat = índice da batida (desde o início da sessão, contagem incluída)
 * a partir da qual o novo andamento vale. Assim a troca acontece exatamente
 * no início da frase calculado pelo app, mesmo que o pacote chegue antes.
 * 0 = na próxima batida (comportamento anterior). */
typedef struct {
    PacketHeader h;
    uint16_t bpm;                       // Novo andamento
    uint16_t at_beat;                   // Batida em que o novo andamento começa
} PktSetTempo;

/* PKT_ANNOUNCE */
typedef struct {
    PacketHeader h;
    char     name[PROTO_NAME_LEN];      // Nome amigável do dispositivo
    char     firmware[PROTO_FW_LEN];    // Versão do firmware
    uint8_t  mac[6];                    // Endereço MAC (identificador único)
    uint8_t  state;                     // DEV_STATE_*
    int8_t   rssi;                      // Intensidade do sinal Wi-Fi (dBm)
    uint16_t sample_rate;               // Taxa de amostragem do microfone
    uint16_t reserved;
} PktAnnounce;

/* PKT_CONNECT_ACK */
typedef struct {
    PacketHeader h;
    uint8_t  accepted;                  // 1 = pareado, 0 = recusado (já em uso)
    uint8_t  reserved[3];
} PktConnectAck;

/* PKT_PONG */
typedef struct {
    PacketHeader h;
    uint8_t  state;                     // DEV_STATE_*
    int8_t   rssi;                      // dBm
    uint16_t reserved;
    float    noise_floor_db;            // Piso de ruído estimado (dBFS)
    uint32_t dropped_events;            // Eventos descartados por fila cheia
} PktPong;

/* PKT_NOTE_ON – timestamp do cabeçalho = instante do ataque (quantizado) */
typedef struct {
    PacketHeader h;
    uint8_t  midi;                      // Nota MIDI estimada
    uint8_t  reserved[3];
    float    frequency_hz;              // Frequência fundamental (Hz)
    float    level_db;                  // Nível do ataque (dBFS)
    float    confidence;                // 0..1 (1 - aperiodicidade do YIN)
} PktNoteOn;

/* PKT_NOTE_OFF – timestamp do cabeçalho = instante da soltura (quantizado) */
typedef struct {
    PacketHeader h;
    uint8_t  midi;                      // Nota MIDI encerrada
    uint8_t  reserved[3];
    uint32_t duration_ms;               // Duração medida da nota
    float    frequency_hz;              // Frequência média durante a nota
} PktNoteOff;

/* PKT_PITCH */
typedef struct {
    PacketHeader h;
    float    frequency_hz;              // 0 quando não há altura definida
    float    confidence;                // 0..1
    float    level_db;                  // Nível RMS (dBFS)
} PktPitch;

/* PKT_BEAT */
typedef struct {
    PacketHeader h;
    uint8_t  beat_in_bar;               // 0 = tempo forte
    uint8_t  count_in;                  // 1 durante os compassos de contagem
    uint16_t bar_index;                 // Índice do compasso desde o início
    uint16_t bpm;                       // Andamento vigente
    uint8_t  session_id;                // Eco do SESSION_START (0 = sessão local)
    uint8_t  reserved;
} PktBeat;

/* PKT_AUDIO_FRAME – cabeçalho do quadro bruto, seguido de num_samples int16 */
typedef struct {
    PacketHeader h;
    uint32_t num_samples;               // Quantidade de amostras
    float    energy;                    // Energia média do quadro
} PktAudioFrameHeader;

#pragma pack(pop)

#endif // PROTOCOL_H
