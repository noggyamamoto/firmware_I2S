/*
 * ============================================================================
 * Dispositivo falso para testar o app sem hardware (Linux/macOS)
 *
 * Usa o mesmo protocol.h do firmware e responde ao app pela rede:
 * DISCOVER -> ANNOUNCE, CONNECT -> CONNECT_ACK, PING -> PONG e, após
 * SESSION_START, envia batidas do metrônomo e uma escala de Dó
 * (NOTE_ON/NOTE_OFF) no andamento pedido.
 *
 * Compilar:  make -C test/host fake_device
 * Executar:  ./fake_device [porta]   (padrão: 54322)
 * ============================================================================
 */
#include <arpa/inet.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "protocol.h"

static int sock;
static uint32_t seq = 0;
static struct sockaddr_in peer;
static int paired = 0;

static uint64_t now_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

static void header(PacketHeader *h, uint8_t type, uint32_t ts) {
    h->magic = PROTO_MAGIC;
    h->version = PROTO_VERSION;
    h->type = type;
    h->sequence = seq++;
    h->timestamp_ms = ts;
}

static void send_to(const struct sockaddr_in *to, const void *data, size_t len) {
    sendto(sock, data, len, 0, (const struct sockaddr *)to, sizeof(*to));
}

int main(int argc, char **argv) {
    int port = argc > 1 ? atoi(argv[1]) : PROTO_DEVICE_PORT;
    sock = socket(AF_INET, SOCK_DGRAM, 0);
    int yes = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        return 1;
    }
    printf("fake_device ouvindo na porta %d\n", port);
    fflush(stdout);

    int session = 0;
    uint64_t t0 = 0;
    uint16_t bpm = 120;
    uint8_t beats = 4, count_in = 2;
    int beat_index = 0, note_index = 0, note_on = 0;
    const uint8_t scale[8] = {60, 62, 64, 65, 67, 69, 71, 72};

    for (;;) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(sock, &fds);
        struct timeval tv = {0, 5000};
        if (select(sock + 1, &fds, NULL, NULL, &tv) > 0) {
            uint8_t buf[256];
            struct sockaddr_in from;
            socklen_t fl = sizeof(from);
            int len = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr *)&from, &fl);
            if (len < (int)sizeof(PacketHeader)) continue;
            PacketHeader *h = (PacketHeader *)buf;
            if (h->magic != PROTO_MAGIC) continue;

            switch (h->type) {
                case PKT_DISCOVER: {
                    PktAnnounce p = {0};
                    header(&p.h, PKT_ANNOUNCE, 0);
                    strcpy(p.name, "PartituraIoT-TEST");
                    strcpy(p.firmware, "1.0.0");
                    uint8_t mac[6] = {0x24, 0x6F, 0x28, 0xAA, 0xBB, 0xCC};
                    memcpy(p.mac, mac, 6);
                    p.state = paired ? DEV_STATE_CONNECTED : DEV_STATE_IDLE;
                    p.rssi = -48;
                    p.sample_rate = 16000;
                    send_to(&from, &p, sizeof(p));
                    break;
                }
                case PKT_CONNECT: {
                    PktConnect *c = (PktConnect *)buf;
                    peer = from;
                    paired = 1;
                    printf("CONNECT de %.24s\n", c->app_name);
                    PktConnectAck p = {0};
                    header(&p.h, PKT_CONNECT_ACK, 0);
                    p.accepted = 1;
                    send_to(&from, &p, sizeof(p));
                    break;
                }
                case PKT_PING: {
                    PktPong p = {0};
                    header(&p.h, PKT_PONG, 0);
                    p.state = session ? DEV_STATE_SESSION : DEV_STATE_CONNECTED;
                    p.rssi = -48;
                    p.noise_floor_db = -71.5f;
                    send_to(&from, &p, sizeof(p));
                    break;
                }
                case PKT_SESSION_START: {
                    PktSessionStart *s = (PktSessionStart *)buf;
                    bpm = s->bpm;
                    beats = s->beats_per_bar;
                    count_in = s->count_in_bars;
                    printf("SESSION_START bpm=%u beats=%u count_in=%u flags=0x%02x\n", bpm, beats,
                           count_in, s->flags);
                    session = 1;
                    t0 = now_ms();
                    beat_index = note_index = note_on = 0;
                    break;
                }
                case PKT_SESSION_STOP:
                    session = 0;
                    break;
                case PKT_DISCONNECT:
                    printf("DISCONNECT\n");
                    paired = 0;
                    session = 0;
                    break;
                case PKT_SET_TEMPO:
                    bpm = ((PktSetTempo *)buf)->bpm;
                    break;
            }
            fflush(stdout);
        }

        if (!session || !paired) continue;
        uint32_t t = (uint32_t)(now_ms() - t0);
        uint32_t beat_ms = 60000 / bpm;

        // Batidas do metrônomo
        while ((uint32_t)beat_index * beat_ms <= t) {
            PktBeat p = {0};
            header(&p.h, PKT_BEAT, beat_index * beat_ms);
            p.beat_in_bar = beat_index % beats;
            p.bar_index = beat_index / beats;
            p.count_in = p.bar_index < count_in;
            p.bpm = bpm;
            send_to(&peer, &p, sizeof(p));
            beat_index++;
        }

        // Escala de Dó em semínimas logo após a contagem
        uint32_t music_start = count_in * beats * beat_ms;
        if (note_index < 8) {
            uint32_t onset = music_start + note_index * beat_ms;
            if (!note_on && t >= onset) {
                PktNoteOn p = {0};
                header(&p.h, PKT_NOTE_ON, onset);
                p.midi = scale[note_index];
                p.frequency_hz = 440.0f;
                p.level_db = -30.0f;
                p.confidence = 0.95f;
                send_to(&peer, &p, sizeof(p));
                note_on = 1;
            } else if (note_on && t >= onset + beat_ms - 40) {
                PktNoteOff p = {0};
                header(&p.h, PKT_NOTE_OFF, onset + beat_ms - 40);
                p.midi = scale[note_index];
                p.duration_ms = beat_ms - 40;
                p.frequency_hz = 440.0f;
                send_to(&peer, &p, sizeof(p));
                note_on = 0;
                note_index++;
            }
        }
    }
}
