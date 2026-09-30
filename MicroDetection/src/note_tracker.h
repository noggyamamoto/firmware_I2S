/*
 * ============================================================================
 * Segmentação de notas (onset / offset) a partir da altura e da energia
 *
 * Recebe, a cada janela de análise (hop), o nível do trecho mais recente e a
 * estimativa de altura do YIN, e produz eventos de início (NOTE_ON) e fim
 * (NOTE_OFF) de nota, com:
 *  - piso de ruído adaptativo + limiar mínimo (imunidade a ruído – RNFE03)
 *  - confirmação da altura por janelas consecutivas (evita notas espúrias)
 *  - detecção de reataque (mesma nota tocada repetidamente)
 *  - quantização leve dos tempos (tolerância temporal – RNFE04)
 *  - máscara temporal para ignorar o clique do metrônomo
 *
 * Módulo independente de hardware (testado no computador em test/host).
 * ============================================================================
 */
#ifndef NOTE_TRACKER_H
#define NOTE_TRACKER_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    float    gate_margin_db;        // Margem acima do piso de ruído
    float    gate_min_db;           // Limiar absoluto mínimo (dBFS)
    float    release_hysteresis_db; // Histerese para o fim de nota
    float    decay_release_db;      // Queda máxima em relação ao pico
    float    reattack_db;           // Salto de energia que indica novo ataque
    float    min_confidence;        // Confiança mínima do YIN
    int      stable_hops;           // Janelas consecutivas para confirmar a nota
    int      onset_timeout_hops;    // Janelas máximas aguardando altura estável
    int      release_hops;          // Janelas abaixo do limiar para soltar
    uint32_t quantize_ms;           // Grade de quantização temporal
} NoteTrackerConfig;

typedef enum {
    NOTE_EVENT_ON = 1,
    NOTE_EVENT_OFF = 2,
} NoteEventType;

typedef struct {
    NoteEventType type;
    uint8_t  midi;                  // Nota MIDI
    uint32_t time_ms;               // Instante do evento (quantizado)
    uint32_t duration_ms;           // Apenas NOTE_OFF
    float    frequency_hz;          // NOTE_ON: estimativa inicial / NOTE_OFF: média
    float    level_db;              // Nível no ataque
    float    confidence;            // Confiança média da altura
} NoteEvent;

typedef enum {
    TRACKER_IDLE,                   // Silêncio / ruído de fundo
    TRACKER_CANDIDATE,              // Ataque detectado, aguardando altura estável
    TRACKER_ACTIVE,                 // Nota confirmada soando
} TrackerState;

typedef struct {
    NoteTrackerConfig cfg;
    TrackerState state;
    float    noise_floor_db;        // Piso de ruído estimado
    float    prev_db;               // Nível da janela anterior
    uint32_t mask_until_ms;         // Fim da janela do clique do metrônomo
    int      mask_pitch_class;      // Classe de altura do clique (0..11) ou -1

    // Candidato a ataque
    uint32_t cand_time_ms;
    int      cand_hops;
    int      stable_midi;
    int      stable_count;
    float    cand_freq_sum;
    float    cand_conf_sum;
    float    cand_peak_db;
    bool     cand_over_active;      // Candidato surgiu sobre uma nota ativa (reataque)

    // Nota ativa
    int      active_midi;
    uint32_t active_on_ms;
    float    active_peak_db;
    float    active_freq_sum;
    int      active_freq_count;
    int      low_count;
    uint32_t low_start_ms;
    int      change_midi;
    int      change_count;
    uint32_t change_start_ms;
} NoteTracker;

/** @brief Preenche a configuração com os valores padrão. */
void note_tracker_default_config(NoteTrackerConfig *cfg);

/** @brief Inicializa o rastreador. */
void note_tracker_init(NoteTracker *t, const NoteTrackerConfig *cfg);

/** @brief Volta ao estado inicial mantendo o piso de ruído aprendido. */
void note_tracker_reset(NoteTracker *t);

/**
 * @brief Informa que o buzzer está tocando um clique até o instante indicado.
 *        Durante esse intervalo, alturas da mesma classe do clique são
 *        ignoradas, sem bloquear notas reais tocadas junto com a batida.
 * @param click_hz Frequência do clique (0 = desconhecida).
 */
void note_tracker_mask_until(NoteTracker *t, uint32_t until_ms, float click_hz);

/**
 * @brief Processa uma janela de análise.
 * @param hop_start_ms Instante de início do trecho mais recente.
 * @param hop_db       Nível RMS do trecho mais recente (dBFS).
 * @param freq_hz      Altura estimada (0 = sem altura).
 * @param confidence   Confiança da altura (0..1).
 * @param out          Vetor com espaço para 2 eventos.
 * @return Quantidade de eventos gerados (0, 1 ou 2).
 */
int note_tracker_process(NoteTracker *t, uint32_t hop_start_ms, float hop_db,
                         float freq_hz, float confidence, NoteEvent out[2]);

/**
 * @brief Encerra a nota ativa (fim da sessão).
 * @return 1 se um NOTE_OFF foi gerado.
 */
int note_tracker_flush(NoteTracker *t, uint32_t now_ms, NoteEvent *out);

/** @brief Limiar atual de detecção (dBFS). */
float note_tracker_gate_db(const NoteTracker *t);

#endif // NOTE_TRACKER_H
