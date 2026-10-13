# firmware_I2S — Dispositivo de captação e detecção de notas

Firmware do dispositivo embarcado do TCC **Sistema de Processamento de Áudio com Feedback em Tempo Real Aplicado ao Ensino de Partitura**.

O dispositivo capta o som do teclado com o microfone digital **INMP441 (I2S)**, identifica as notas tocadas (altura e duração), envia os eventos ao [aplicativo Flutter](https://github.com/noggyamamoto/Flutter_App) por **Wi-Fi** e executa o metrônomo sonoro (buzzer) e visual (LED RGB). O app compara as notas recebidas com a partitura MusicXML e devolve o feedback ao aluno.

### Novidades da versão 1.3.3

| Feature (commit) | O que mudou |
|---|---|
| **Notas sem atraso** (`notas-sem-atraso`) | Na 1.3.2 todo NOTE_ON esperava 3 janelas (48 ms) para confirmar a oitava, e o feedback no app atrasava. Agora a nota sai assim que a altura se confirma; a espera fica só para os sons em que o detector tem dúvida de oitava (energia real uma oitava abaixo, ou som quase senoidal, como o 4º harmônico isolado do alto-falante pequeno). Atraso de detecção (ataque → evento pronto) nos testes: média de 97 para 62 ms, pior caso de 152 para 112 ms. A correção de oitava ficou ~3 vezes mais barata no pior trecho (mesmas respostas, conferidas trecho a trecho): o trecho com nota custa no máximo 1,6x o trecho sem nota (antes 2,3x), com folga nos 16 ms do ESP32 mesmo enquanto se toca. O status (Menu 4) mostra o pior tempo de processamento e o atraso de detecção da sessão |

### Novidades da versão 1.3.2

| Feature (commit) | O que mudou |
|---|---|
| **Oitava correta com harmônico isolado** (`oitava-correta-harmonico-isolado`) | Na 1.3.1 o C4–G4 ainda saíam como C6–G6 quando o microfone recebia praticamente só o 4º harmônico (as componentes da nota ficavam 30–40 dB abaixo). O detector agora aceita a oitava de baixo também com evidência fraca, mas coerente (picos centrados nos harmônicos da nota, acima do ruído entre eles), e o rastreador de notas espera até 3 janelas (~48 ms) antes de anunciar a nota para confirmar a oitava: se as leituras seguintes descem para um submúltiplo estável, a nota sai com a oitava certa. O dispositivo também mede a taxa real do microfone e corrige a frequência se ela estiver mais de 10% fora dos 16 000 amostras/s configurados. Diagnóstico: o monitor da execução mostra a taxa medida no cabeçalho e, em cada nota CAPTADA, a energia uma e duas oitavas abaixo (`[f/2 … dB, f/4 … dB]`) |

### Novidades da versão 1.3.1

| Feature (commit) | O que mudou |
|---|---|
| **Oitava correta com alto-falante pequeno** (`oitava-correta-alto-falante-pequeno`) | Na 1.3.0 o E4 ainda saía como E6 quando o som vinha de um alto-falante pequeno (teclado ou celular), que quase não reproduz abaixo de ~1,2 kHz: só o 4º harmônico chegava forte ao microfone. A confirmação do período verdadeiro passa a usar também as componentes acima do harmônico (5º, 7º...), que esse alto-falante reproduz. O afinador (Menu 3 › 3) mostra a energia em f/4, f/3, f/2, f e 2f para diagnóstico |

### Novidades da versão 1.3.0

| Feature (commit) | O que mudou |
|---|---|
| **Piano inteiro: A0 a C8** (`deteccao-piano-inteiro-a0-c8`) | Detecção das 88 notas do piano. Novo detector de altura com duas resoluções (16 kHz/32 ms para os agudos, 4 kHz/80 ms para os graves) e **correção de oitava**: um harmônico forte não é mais confundido com a nota (antes, um E4 com o 4º harmônico dominante era lido como E6). Passa-banda de 20 Hz a 5 kHz. Custo de processamento ~40% do YIN da versão 1.1.0 |

### Novidades da versão 1.2.0

| Feature (commit) | O que mudou |
|---|---|
| **Detecção a partir do C4** (`deteccao-a-partir-do-c4`) | Notas detectadas de C4 a C7; abaixo do C4 são ignoradas. Janela do YIN de 32 ms, CPU a 240 MHz e `-O2`: o processamento cabe com folga nos 16 ms de cada janela, sem perda de amostras nem `task_wdt`. Relógio das amostras reancorado se amostras forem perdidas |
| **Captação pronta na execução** (`captacao-pronta-na-execucao`) | A contagem de entrada (buzzer sempre ligado) não eleva mais o limiar de detecção; cliques sobre uma nota longa não viram notas repetidas falsas; WebSocket sem atraso de Nagle (`TCP_NODELAY`) |
| **Monitor da execução no serial** (`monitor-da-execucao-serial`) | Durante a execução o terminal mostra cada nota **ESPERADA** (ex.: `C4 seminima`) na hora dela e cada nota **CAPTADA** pelo microfone (nome, Hz, instante, figura pela duração e comparação) |
| **Menu sem acentos e log técnico** (`menu-sem-acentos-e-log-tecnico`) | Menu em ASCII puro (sem acentos, cedilha ou símbolos). Mensagens do watchdog (`task_wdt`) e `Backtrace` ficam somente em *Log tecnico* |
| **Wi-Fi escolhido na inicialização** (`wifi-escolhido-na-inicializacao`) | Ao ligar, o terminal pede SSID e senha; sem resposta (15 s) ou se a rede falhar, usa a rede padrão `JOAO_2.4G`. Anúncio mDNS `partitura.local`. Corrigido: a rede padrão do `config.h` nunca era usada |

---

## Sumário

1. [Visão geral](#1-visão-geral)
2. [Requisitos atendidos](#2-requisitos-atendidos)
3. [Hardware](#3-hardware)
4. [Arquitetura de software](#4-arquitetura-de-software)
   - 4.1 [Camadas](#41-camadas)
   - 4.2 [Estrutura de diretórios](#42-estrutura-de-diretórios)
   - 4.3 [Módulos por camada](#43-módulos-por-camada)
   - 4.4 [Diagrama de classes (módulos)](#44-diagrama-de-classes-módulos)
   - 4.5 [Tarefas e concorrência](#45-tarefas-e-concorrência)
5. [Fluxos de funcionamento](#5-fluxos-de-funcionamento)
6. [Protocolo de comunicação](#6-protocolo-de-comunicação)
7. [Configuração](#7-configuração)
8. [Compilação e gravação](#8-compilação-e-gravação)
9. [Operação do dispositivo](#9-operação-do-dispositivo)
10. [Testes](#10-testes)
11. [Solução de problemas](#11-solução-de-problemas)
12. [Licença](#12-licença)

---

## 1. Visão geral

```mermaid
flowchart LR
    T["🎹 Teclado digital"] -- som --> M["Microfone INMP441"]
    M -- I2S + DMA --> E["ESP32 / ESP32-S3<br/>filtro · YIN · segmentação"]
    E -- "NOTE_ON / NOTE_OFF / BEAT<br/>(UDP ou WebSocket)" --> A["📱 App Flutter<br/>celular · computador · web"]
    A -- "CONNECT / SESSION_START / SET_TEMPO" --> E
    E -- PWM --> B["🔊 Buzzer<br/>(metrônomo sonoro)"]
    E -- PWM --> R["🌈 LED RGB<br/>(metrônomo visual)"]
    E -- GPIO --> L["💡 LED embutido<br/>(status da rede)"]
```

| Item | Descrição |
|---|---|
| Linguagem | C (C17) |
| Framework | ESP-IDF 5.x (FreeRTOS, `i2s_std`, LEDC, `esp_timer`, NVS, lwIP, `esp_http_server`) |
| Ferramentas | PlatformIO ou `idf.py` (ESP-IDF) |
| Placas | ESP32 DevKit V1 (MVP) e ESP32-S3 DevKitC-1 N16R8 (lista de materiais do TCC) |
| Áudio | 16 kHz, 24 bits, trechos de 16 ms; análise em 32 ms (agudos) e 80 ms (graves); notas de A0 a C8 (piano inteiro) |
| Comunicação | UDP (porta 54322) para celular/computador e WebSocket (`ws://IP/ws`) para o app web |

---

## 2. Requisitos atendidos

| Requisito (TCC) | Implementação | Módulo |
|---|---|---|
| **RFE01** – Verificação de rede | Varredura das redes, conexão com novas tentativas, rede própria de reserva (modo AP) e LED de status | `hal_wifi`, `hal_indicators`, `app_state` |
| **RFE02** – Metrônomo sonoro | Buzzer passivo por PWM, clique acentuado no tempo forte | `metronome`, `hal_indicators` |
| **RFE03** – Metrônomo visual | LED RGB: azul (binário), verde (ternário), roxo (quaternário), amarelo na contagem | `metronome`, `hal_indicators` |
| **RFE04** – Captação digital e filtragem | INMP441 via I2S (24 bits em slots de 32 bits) + passa-banda 20 Hz – 5 kHz | `drv_i2s_mic`, `hal_audio`, `dsp` |
| **RFE05** – Processamento digital de sinais | Altura pelo algoritmo **YIN** + segmentação em notas (ataque, soltura, duração) | `dsp`, `note_tracker`, `audio_pipeline` |
| **RFE06** – Transmissão de dados | Pacotes binários com sequência e timestamp, por UDP ou WebSocket | `link_service`, `hal_net` |
| **RNFE01** – Baixa latência | Janelas de 16 ms; tempo de processamento medido (menu serial, Status do sistema); notas com prioridade sobre os fluxos de diagnóstico | `audio_pipeline`, `app_state` |
| **RNFE02** – Calibração de timbre | Faixa de altura e limiares ajustados para teclado digital (`config.h`) | `config.h`, `note_tracker` |
| **RNFE03** – Imunidade a ruído | Passa-banda + piso de ruído adaptativo + limiar mínimo + máscara do clique | `dsp`, `note_tracker` |
| **RNFE04** – Tolerância temporal | Quantização dos tempos em 10 ms | `note_tracker` |
| **RNFA02** – Comunicação assíncrona | WebSocket para o app web e UDP para os apps nativos | `hal_net` |

---

## 3. Hardware

### 3.1 Lista de materiais

| Item | Qtd. | Função |
|---|---|---|
| ESP32-S3 N16R8 (ou ESP32 DevKit V1) | 1 | Aquisição, processamento e comunicação sem fio |
| Microfone INMP441 | 1 | Captação digital do som (I2S) |
| LED RGB catodo comum + 3 resistores 220 Ω | 1 | Metrônomo visual |
| Buzzer passivo + transistor NPN | 1 | Metrônomo sonoro |
| Regulador 7805 + fonte 9 V | 1 | Alimentação estável |
| Protoboard e jumpers | — | Montagem do protótipo |

### 3.2 Ligações

| INMP441 | ESP32 DevKit V1 | ESP32-S3 N16R8 |
|---|---|---|
| VDD | 3V3 | 3V3 |
| GND | GND | GND |
| L/R | GND (canal esquerdo) | GND |
| SCK | GPIO26 | GPIO5 |
| WS | GPIO25 | GPIO6 |
| SD | GPIO33 | GPIO4 |

| Indicador | ESP32 DevKit V1 | ESP32-S3 N16R8 |
|---|---|---|
| LED de status | GPIO2 (LED embutido) | GPIO2 |
| LED RGB (R / G / B) | GPIO27 / 14 / 13 | GPIO15 / 16 / 17 |
| Buzzer passivo (via transistor) | GPIO18 | GPIO18 |

Os pinos ficam em [`include/config.h`](MicroDetection/include/config.h) e são escolhidos automaticamente pelo alvo de compilação. Para LED RGB de anodo comum, use `RGB_LED_COMMON_ANODE 1`.

---

## 4. Arquitetura de software

### 4.1 Camadas

O firmware é organizado em três camadas com dependência em um único sentido: **app → hal → drivers**. A aplicação não acessa periféricos nem APIs de hardware do ESP-IDF diretamente, o que facilita testes, trocas de placa e manutenção.

```mermaid
flowchart TB
    subgraph APP["app/ — lógica da aplicação"]
        direction LR
        main["main"] --- state["app_state"] --- pipeline["audio_pipeline"] --- link["link_service"]
        metro["metronome"] --- ui["serial_ui"] --- dsp["dsp · note_tracker"] --- cbuf["circular_buffer"]
    end
    subgraph HAL["hal/ — abstração de hardware"]
        direction LR
        haudio["hal_audio"] --- hind["hal_indicators"] --- hwifi["hal_wifi"] --- hnet["hal_net"]
        hsto["hal_storage"] --- hcon["hal_console"] --- htime["hal_time"] --- hsys["hal_system"]
    end
    subgraph DRV["drivers/ — controle direto do hardware"]
        direction LR
        dgpio["drv_gpio"] --- dpwm["drv_pwm"] --- di2s["drv_i2s_mic"]
        duart["drv_uart"] --- dnvs["drv_nvs"] --- dwifi["drv_wifi"]
    end
    subgraph IDF["ESP-IDF / FreeRTOS"]
        direction LR
        periph["GPIO · LEDC · I2S · UART · NVS · Wi-Fi"]
        stack["lwIP · esp_http_server · esp_timer"]
    end
    APP --> HAL --> DRV --> periph
    HAL --> stack
```

| Camada | Responsabilidade | Pode usar |
|---|---|---|
| `drivers/` | Controle direto dos periféricos (configurar pinos, PWM, I2S, UART, flash, rádio) | ESP-IDF (drivers de periféricos) |
| `hal/` | Funções simples e reutilizáveis que escondem o hardware (ler áudio normalizado, piscar o LED, conectar ao Wi-Fi, enviar um pacote) | `drivers/`, serviços do ESP-IDF (timers, sockets, HTTP) |
| `app/` | Fluxo de controle, regras do produto, protocolo, DSP, metrônomo e interface com o usuário | `hal/`, FreeRTOS, log |
| `include/` | Cabeçalhos públicos de cada camada e configurações compartilhadas | — |

### 4.2 Estrutura de diretórios

```text
MicroDetection/
├── app/                      # lógica da aplicação (componente principal do ESP-IDF)
│   ├── CMakeLists.txt        # registra app/, hal/ e drivers/ em um único componente
│   ├── main.c                # app_main: inicialização em camadas e criação das tarefas
│   ├── app_state.c           # sessão, pareamento, fila de saída e LED de status
│   ├── audio_pipeline.c      # tarefa de áudio: microfone → DSP → eventos de nota
│   ├── dsp.c                 # passa-banda (biquads RBJ), nível RMS e detector YIN
│   ├── note_tracker.c        # segmentação em notas (ataque, soltura, reataque, quantização)
│   ├── metronome.c           # metrônomo sonoro/visual com contagem de entrada
│   ├── link_service.c        # protocolo com o app (comandos, respostas e eventos)
│   ├── serial_ui.c           # menu serial navegável (vermelho/branco), único conteúdo do terminal
│   ├── diagnostics.c         # problemas atuais explicados ao usuário (o que houve / o que fazer)
│   ├── ui_events.c           # avisos exibidos dentro do menu (app conectado, Wi-Fi caiu...)
│   ├── execution_monitor.c   # monitor da execução no serial (esperado x captado)
│   ├── wifi_setup.c          # pergunta SSID e senha ao ligar (padrão: JOAO_2.4G)
│   ├── idf_component.yml     # componente mDNS (partitura.local)
│   └── circular_buffer.c     # buffer circular de quadros brutos (modo diagnóstico)
├── hal/                      # camada de abstração de hardware
│   ├── hal_audio.c           # amostras float normalizadas + instante de cada bloco
│   ├── hal_indicators.c      # LED de status, LED RGB e buzzer
│   ├── hal_wifi.c            # conexão com backoff, rede própria de reserva e causa das falhas
│   ├── hal_net.c             # transporte de pacotes: UDP e WebSocket
│   ├── hal_storage.c         # NVS e rede Wi-Fi padrão
│   ├── hal_console.c         # terminal do menu + histórico do ESP_LOG (log técnico)
│   ├── hal_time.c            # relógio monotônico e temporizadores
│   └── hal_system.c          # reinício e memória livre
├── drivers/                  # controle direto de hardware
│   ├── drv_gpio.c            # saídas digitais (LED embutido)
│   ├── drv_pwm.c             # PWM por hardware (LEDC)
│   ├── drv_i2s_mic.c         # microfone INMP441 (I2S + DMA)
│   ├── drv_uart.c            # porta serial
│   ├── drv_nvs.c             # partição NVS da flash
│   └── drv_wifi.c            # rádio Wi-Fi (estação, ponto de acesso, varredura)
├── include/                  # cabeçalhos
│   ├── config.h              # pinos, parâmetros de áudio, DSP e rede
│   ├── protocol.h            # formato binário dos pacotes (espelhado no app)
│   ├── app/                  # cabeçalhos da camada de aplicação
│   ├── hal/                  # cabeçalhos da HAL
│   └── drivers/              # cabeçalhos dos drivers
├── test/host/                # testes no computador (gcc) e ferramentas
├── CMakeLists.txt            # projeto ESP-IDF
├── platformio.ini            # projeto PlatformIO (src_dir = app)
├── sdkconfig.defaults        # opções do ESP-IDF (WebSocket)
└── sdkconfig.esp32doit-devkit-v1
```

> O ESP-IDF já possui um componente chamado `hal`; por isso as três camadas são compiladas em um único componente (registrado em `app/CMakeLists.txt`), e a separação é garantida pelos diretórios de cabeçalhos e pela regra de dependência acima.

### 4.3 Módulos por camada

#### drivers/

| Módulo | Função | API principal |
|---|---|---|
| `drv_gpio` | Saída digital do LED embutido | `drv_gpio_output_init`, `drv_gpio_write` |
| `drv_pwm` | Temporizadores e canais LEDC | `drv_pwm_timer_init`, `drv_pwm_channel_init`, `drv_pwm_set_duty`, `drv_pwm_set_freq` |
| `drv_i2s_mic` | Canal I2S mestre RX (Philips, 32 bits, mono esquerdo) com DMA | `drv_i2s_mic_init`, `drv_i2s_mic_start`, `drv_i2s_mic_read` |
| `drv_uart` | UART0 do monitor serial | `drv_uart_init`, `drv_uart_write`, `drv_uart_read_byte` |
| `drv_nvs` | Leitura e escrita de strings na NVS | `drv_nvs_init`, `drv_nvs_get_str`, `drv_nvs_set_str` |
| `drv_wifi` | Rádio Wi-Fi e eventos (STA, AP, IP, varredura com segurança da rede); país BR, WPA2/WPA3 (SAE H2E), PMF opcional, 20 MHz | `drv_wifi_init`, `drv_wifi_connect`, `drv_wifi_enable_ap`, `drv_wifi_scan` |

#### hal/

| Módulo | Função | API principal |
|---|---|---|
| `hal_audio` | Áudio em `float` [-1, 1) com ganho e carimbo de tempo | `hal_audio_read`, `hal_audio_next_sample_time_us` |
| `hal_indicators` | Padrões do LED de status, flash do LED RGB, bipe | `hal_status_led_set`, `hal_rgb_flash`, `hal_buzzer_beep` |
| `hal_wifi` | Política de conexão (tentativas com backoff, AP de reserva, estado) e classificação das falhas (senha, rede inexistente, sinal, segurança, DHCP) | `hal_wifi_start`, `hal_wifi_reconnect`, `hal_wifi_get_diag`, `hal_wifi_scan` |
| `hal_net` | Datagramas por UDP e WebSocket com endereço único (`NetPeer`) | `hal_net_init`, `hal_net_send`, `hal_net_peer_equal` |
| `hal_storage` | NVS e rede Wi-Fi padrão do `config.h` | `hal_storage_init`, `hal_storage_default_wifi` |
| `hal_console` | Terminal do menu; desvia o ESP_LOG para um histórico em memória | `hal_console_write`, `hal_console_read_byte`, `hal_console_capture_logs`, `hal_console_log_lines` |
| `hal_time` | Relógio e temporizadores | `hal_time_us`, `hal_timer_create`, `hal_timer_start_once` |
| `hal_system` | Sistema | `hal_system_restart`, `hal_system_free_heap` |

#### app/

| Módulo | Função |
|---|---|
| `main` | Inicializa as camadas em ordem, trata falhas críticas e cria as tarefas |
| `app_state` | Estado compartilhado (spinlock): app pareado, sessão (com `session_id`), flags, métricas, filas de saída com prioridade e padrão do LED de status |
| `audio_pipeline` | Tarefa produtora: lê o microfone, filtra, estima altura, segmenta notas, publica eventos e verifica o microfone (sem sinal, erro de I2S, saturação) |
| `dsp` | Passa-banda (cascata de biquads RBJ), nível RMS em dBFS, detector de altura YIN |
| `note_tracker` | Máquina de estados de notas (ver 5.3), piso de ruído adaptativo, reataque, quantização |
| `metronome` | Batidas em instantes absolutos (sem deriva), contagem de entrada, som e luz, troca de BPM em uma batida exata |
| `link_service` | Interpreta os comandos do app e monta/envia os pacotes de resposta e de eventos |
| `serial_ui` | Menu serial navegável: status, Wi-Fi (lista de redes, senha, detalhes), captura local, monitor de notas, afinador, metrônomo de teste, log técnico |
| `diagnostics` | Converte o estado do Wi-Fi, do microfone e do enlace em problemas com "o que fazer" |
| `execution_monitor` | Registra sem bloquear as notas esperadas (app) e captadas (microfone) e entrega as linhas do monitor da execução em ordem de tempo |
| `wifi_setup` | Pergunta SSID e senha no terminal ao ligar; ENTER vazio/ESC/15 s usam a rede padrão |
| `ui_events` | Avisos recentes exibidos no menu (não bloqueante, chamado de qualquer tarefa) |
| `circular_buffer` | Buffer circular SPSC sem cópia (spinlock só no contador) para quadros de áudio bruto (diagnóstico) |

### 4.4 Diagrama de classes (módulos)

Em C, cada módulo funciona como uma classe com estado privado (`static`) e funções públicas declaradas em `include/`. O diagrama mostra os principais tipos, operações e dependências.

```mermaid
classDiagram
    direction TB

    class main {
        <<app>>
        +app_main()
        -fatal(what, action)
        -on_wifi_state(state)
    }
    class app_state {
        <<app>>
        -NetPeer peer
        -SessionInfo session
        -QueueHandle_t queue_ctrl
        -QueueHandle_t queue_stream
        -portMUX_TYPE lock
        +app_state_set_peer(peer, name)
        +app_state_session_start(bpm, beats, countIn, flags, local, sessionId) bool
        +app_state_session_stop()
        +app_state_post(OutEvent) bool
        +app_state_next_event(out, wait) bool
        +app_state_refresh_status_led()
    }
    class audio_pipeline {
        <<app>>
        -BandpassFilter filter
        -YinDetector yin
        -NoteTracker tracker
        +audio_pipeline_init() bool
        +audio_pipeline_start_task()
        +audio_pipeline_mask_click(untilMs, hz)
        +audio_pipeline_mic_status() MicStatus
    }
    class dsp {
        <<app>>
        +bandpass_process(filter, x, n)
        +yin_detect(yin, window, confidence) float
        +dsp_rms_db(x, n) float
        +dsp_hz_to_midi(hz) float
    }
    class note_tracker {
        <<app>>
        -TrackerState state
        -float noise_floor_db
        +note_tracker_process(t, ms, db, hz, conf, events) int
        +note_tracker_mask_until(t, ms, hz)
        +note_tracker_flush(t, ms, event) bool
    }
    class metronome {
        <<app>>
        -HalTimer timer
        +metronome_start(bpm, beats, countIn, flags, t0, sessionId)
        +metronome_set_tempo(bpm, atBeat)
        +metronome_stop()
    }
    class link_service {
        <<app>>
        +link_service_init() bool
        +link_service_start()
        -handle_packet(from, data, len)
        -tx_task()
    }
    class serial_ui {
        <<app>>
        +serial_ui_splash()
        +serial_ui_fatal(what, action)
        +serial_ui_start_task()
    }
    class diagnostics {
        <<app>>
        +diagnostics_collect(out, max) int
        +diagnostics_wifi_error_text(diag, buf, len)
    }
    class ui_events {
        <<app>>
        +ui_event(level, fmt)
        +ui_events_recent(out, max) int
    }
    class OutEvent {
        <<struct>>
        OutEventType type
        bool has_dest
        NetPeer dest
        uint32 timestamp_ms
    }

    class hal_audio {
        <<hal>>
        +hal_audio_init(n) bool
        +hal_audio_read(out, n, timeout) size_t
        +hal_audio_next_sample_time_us() int64
    }
    class hal_indicators {
        <<hal>>
        +hal_status_led_set(StatusPattern)
        +hal_rgb_flash(r, g, b, ms)
        +hal_buzzer_beep(hz, ms)
    }
    class hal_wifi {
        <<hal>>
        +hal_wifi_start(cred, onState) bool
        +hal_wifi_reconnect(cred)
        +hal_wifi_state() HalWifiState
        +hal_wifi_get_diag(out)
        +hal_wifi_scan(out, max) int
    }
    class hal_net {
        <<hal>>
        +hal_net_init(onReceive) bool
        +hal_net_send(NetPeer, data, len) bool
        +hal_net_peer_equal(a, b) bool
    }
    class NetPeer {
        <<struct>>
        NetPeerKind kind
        uint32 ip
        uint16 port
        int ws_fd
    }
    class hal_storage {
        <<hal>>
        +hal_storage_load_wifi(out)
        +hal_storage_save_wifi(cred) bool
    }
    class hal_console {
        <<hal>>
        +hal_console_write(text)
        +hal_console_read_byte(out, timeout) bool
        +hal_console_capture_logs()
        +hal_console_log_lines(out, max) int
    }
    class hal_time {
        <<hal>>
        +hal_time_us() int64
        +hal_timer_create(cb, arg, name) HalTimer
        +hal_timer_start_once(t, us)
    }

    class drv_i2s_mic {
        <<driver>>
        +drv_i2s_mic_init(cfg, n)
        +drv_i2s_mic_read(out, n, timeout)
    }
    class drv_gpio {
        <<driver>>
        +drv_gpio_output_init(pin)
        +drv_gpio_write(pin, level)
    }
    class drv_pwm {
        <<driver>>
        +drv_pwm_set_duty(ch, duty)
        +drv_pwm_set_freq(timer, hz)
    }
    class drv_wifi {
        <<driver>>
        +drv_wifi_init(cb)
        +drv_wifi_connect()
        +drv_wifi_enable_ap(ssid, pass, ch)
    }
    class drv_nvs {
        <<driver>>
        +drv_nvs_get_str(ns, key, out, n)
        +drv_nvs_set_str(ns, key, v)
    }
    class drv_uart {
        <<driver>>
        +drv_uart_write(port, data, len)
        +drv_uart_read_byte(port, out, timeout)
    }

    main ..> app_state
    main ..> audio_pipeline
    main ..> link_service
    main ..> serial_ui
    main ..> hal_wifi
    audio_pipeline ..> dsp
    audio_pipeline ..> note_tracker
    audio_pipeline ..> hal_audio
    audio_pipeline ..> app_state : publica OutEvent
    metronome ..> hal_indicators
    metronome ..> app_state : publica BEAT
    metronome ..> audio_pipeline : máscara do clique
    link_service ..> app_state : consome as filas
    link_service ..> hal_net
    link_service ..> metronome
    app_state ..> hal_indicators
    app_state ..> hal_wifi
    app_state "1" o-- "0..1" NetPeer : app pareado
    app_state "1" o-- "*" OutEvent : filas de saída
    serial_ui ..> hal_console
    serial_ui ..> hal_storage
    serial_ui ..> hal_wifi
    serial_ui ..> diagnostics
    serial_ui ..> ui_events
    diagnostics ..> hal_wifi
    diagnostics ..> audio_pipeline
    app_state ..> ui_events
    link_service ..> ui_events
    hal_audio ..> drv_i2s_mic
    hal_indicators ..> drv_gpio
    hal_indicators ..> drv_pwm
    hal_indicators ..> hal_time
    hal_wifi ..> drv_wifi
    hal_storage ..> drv_nvs
    hal_console ..> drv_uart
    hal_net ..> NetPeer
```

### 4.5 Tarefas e concorrência

| Tarefa | Núcleo | Prioridade | Função |
|---|---|---|---|
| `audio_prod` | 1 | 5 | Microfone → DSP → eventos (núcleo exclusivo para não perder amostras) |
| `net_tx` | 0 | 4 | Consome as filas de saída e envia pacotes (UDP ou WebSocket) |
| `net_rx` | 0 | 3 | Recebe comandos UDP |
| `httpd` | 0 | 5 (padrão) | Servidor WebSocket do app web |
| `esp_timer` | 0 | 22 | Batidas do metrônomo, desligamento do LED/buzzer, monitor de PING, tentativas de Wi-Fi |
| `ui_task` | 0 | 1 | Menu serial |
| `status_led` | 0 | 1 | Animação do LED de status |

#### Sincronização (análise e escolhas)

| Recurso compartilhado | Quem acessa | Mecanismo | Por quê |
|---|---|---|---|
| `app_state` (sessão, app pareado, flags, contadores) | áudio (a cada 16 ms), rede, menu e **callbacks de timer** (metrônomo, watchdog do PING) | **spinlock** (`portMUX`) | As seções críticas só copiam poucos bytes. Com o **mutex** do MVP, a tarefa de áudio (prioridade 5, núcleo 1) podia ficar bloqueada esperando uma tarefa de prioridade menor no núcleo 0 – que por sua vez pode ser preemptada pelo Wi-Fi (prioridade 23) – e os callbacks do `esp_timer` não devem bloquear. O spinlock não bloqueia, não troca de contexto e não sofre inversão de prioridade. |
| Saída para a rede | áudio, metrônomo, recepção UDP/WebSocket → `net_tx` | **duas filas FreeRTOS** + **notificação de tarefa** | Fila *prioritária* (32: respostas, `NOTE_ON/OFF`, `BEAT`) e fila de *fluxo* (16: `PITCH`, áudio bruto). Produtores usam timeout 0 (nunca bloqueiam); o consumidor dorme em `ulTaskNotifyTake` e sempre esvazia a fila prioritária primeiro. Assim o fluxo de diagnóstico nunca atrasa nem descarta uma nota. Perdas são contadas separadamente. |
| Buffer circular de áudio bruto (4 quadros) | áudio (escreve) → `net_tx` (lê) | **SPSC sem cópia** + spinlock só no contador | O produtor escreve no slot da cauda e o consumidor envia direto do slot da cabeça (o cabeçalho do pacote fica no próprio slot): sem `memcpy` de 2 KB dentro de uma seção crítica e sem bloqueio. Buffer cheio descarta o quadro novo (o slot em transmissão nunca é sobrescrito). Quadros de uma sessão anterior são descartados pelo número de geração. |
| Máscara do clique, últimas notas, avisos do menu, log técnico | várias tarefas | spinlock | Dados pequenos, escritos e lidos em poucas instruções. |
| Envio por WebSocket | `net_tx` | `send_wait_timeout` = 1 s | O padrão (5 s) podia prender a transmissão com um cliente lento e encher as filas. |

O único ponto em que uma tarefa bloqueia esperando outra é o **consumidor** (`net_tx`) aguardando eventos – o comportamento desejado. A tarefa de áudio só bloqueia na leitura do I2S (DMA), que é o seu relógio.

```mermaid
flowchart LR
    subgraph C1["Núcleo 1"]
        AP["audio_prod"]
    end
    subgraph C0["Núcleo 0"]
        TM["esp_timer<br/>(metrônomo)"]
        TX["net_tx"]
        RX["net_rx (UDP)"]
        WS["httpd (WebSocket)"]
        UI["ui_task"]
    end
    AP -- "NOTE_ON/OFF" --> QC[("Fila prioritária")]
    AP -- "PITCH, áudio bruto" --> QS[("Fila de fluxo")]
    AP -- "quadros (sem cópia)" --> CB[("Buffer circular")]
    TM -- BEAT --> QC
    RX -- "ANNOUNCE, ACK, PONG" --> QC
    WS -- "ANNOUNCE, ACK, PONG" --> QC
    QC -- "1º" --> TX
    QS -- "2º" --> TX
    CB --> TX
    TX -- pacotes --> NET(("UDP / WebSocket"))
    RX -. "comandos" .-> S[["app_state (spinlock)"]]
    WS -. "comandos" .-> S
    UI -. "sessão local" .-> S
    S -. "sessão ativa, t0" .-> AP
```

---

## 5. Fluxos de funcionamento

### 5.1 Inicialização

```mermaid
flowchart TD
    A([Energização]) --> W["hal_console_init<br/>wifi_setup_ask: SSID e senha<br/>(15 s sem tecla = rede padrão)"]
    W --> B["hal_indicators_init"]
    B --> C{"hal_storage_init<br/>(NVS)"}
    C -- falha --> F["fatal(): LED de erro<br/>e mensagem no serial"]
    C -- ok --> D["app_state_init<br/>metronome_init"]
    D --> E{"audio_pipeline_init<br/>(I2S + DSP)"}
    E -- falha --> F
    E -- ok --> H{"hal_wifi_start<br/>(rede escolhida, reserva: padrão)"}
    H -- falha --> F
    H -- ok --> I{"link_service_init<br/>(UDP + WebSocket)"}
    I -- falha --> F
    I -- ok --> M["hal_net_advertise<br/>(mDNS partitura.local)"]
    M --> J["Cria tarefas:<br/>audio_prod · net_tx · net_rx · ui_task"]
    J --> K([Sistema pronto])
```

### 5.2 Processamento de áudio (a cada 16 ms)

```mermaid
flowchart TD
    A["hal_audio_read<br/>256 amostras (16 ms)"] --> B["Passa-banda<br/>20 Hz – 5 kHz"]
    B --> C["Janelas deslizantes<br/>512 amostras a 16 kHz (32 ms)<br/>320 amostras a 4 kHz (80 ms)"]
    C --> D["Nível RMS (dBFS)"]
    C --> E["Altura: YIN nas duas faixas<br/>+ correção de oitava"]
    D --> F{"Sessão mudou?"}
    E --> F
    F -- sim --> G["Fecha nota aberta<br/>e reinicia o rastreador"]
    F -- não --> H
    G --> H["Aplica máscara do clique<br/>do metrônomo"]
    H --> I["note_tracker_process"]
    I --> J{"Sessão ativa e<br/>após o instante zero?"}
    J -- não --> L
    J -- sim --> K["Publica NOTE_ON / NOTE_OFF<br/>(+ PITCH e quadros brutos, se ativados)"]
    K --> L["Mede o tempo de processamento<br/>(RNFE01)"]
    L --> A
```

### 5.3 Segmentação de notas

```mermaid
stateDiagram-v2
    [*] --> IDLE
    IDLE --> CANDIDATE: nível > piso de ruído + margem
    CANDIDATE --> ACTIVE: mesma nota em N janelas<br/>(confiança ≥ mínima) / NOTE_ON
    CANDIDATE --> IDLE: tempo esgotado ou nível caiu
    ACTIVE --> ACTIVE: reataque (salto de energia) / NOTE_OFF + NOTE_ON
    ACTIVE --> ACTIVE: nova altura estável (legato) / NOTE_OFF + NOTE_ON
    ACTIVE --> IDLE: nível abaixo do limiar ou queda grande<br/>em relação ao pico / NOTE_OFF
    note right of IDLE
        O piso de ruído é atualizado
        continuamente em silêncio (RNFE03)
    end note
```

### 5.4 Conexão Wi-Fi e LED de status

```mermaid
stateDiagram-v2
    [*] --> Varredura
    Varredura --> Conectando: rede digitada ao ligar
    Varredura --> Padrao: nada digitado (15 s)
    Conectando --> Conectado: IP recebido
    Conectando --> Conectando: falha (backoff 1 s, 2 s... até 5 s)
    Conectando --> Padrao: 3 tentativas (senha incorreta, rede inexistente)<br/>ou 8 tentativas (demais causas)
    Padrao --> Conectado: IP recebido
    Padrao --> RedePropria: tentativas esgotadas na rede padrão
    RedePropria --> Conectado: nova tentativa a cada 15 s
    Conectado --> Conectando: conexão perdida
    Conectado --> Pareado: CONNECT do app
    RedePropria --> Pareado: CONNECT do app
    Pareado --> Conectado: DISCONNECT ou 5 s sem PING

    note right of Conectando: LED pisca rápido (5 Hz)
    note right of RedePropria: LED com 2 piscadas curtas
    note right of Conectado: LED pisca lento (1 Hz)
    note right of Pareado: LED aceso
```

### 5.5 Pareamento e execução com o app

```mermaid
sequenceDiagram
    autonumber
    participant App as App Flutter
    participant Servico as link_service
    participant State as app_state
    participant Metro as metronome
    participant Audio as audio_pipeline

    App->>Servico: DISCOVER (broadcast UDP ou ws://IP/ws)
    Servico-->>App: ANNOUNCE (nome, firmware, MAC, RSSI)
    App->>Servico: CONNECT (nome do app)
    Servico->>State: app_state_set_peer()
    Servico-->>App: CONNECT_ACK (aceito)
    loop a cada 1 s
        App->>Servico: PING
        Servico-->>App: PONG (estado, RSSI, piso de ruído)
    end
    App->>Servico: SESSION_START (BPM, compasso, 2 compassos de contagem, session_id)
    Servico->>State: session_start() — t0 = agora
    Note over App,State: um SESSION_START repetido (mesmo session_id) é ignorado:<br/>o relógio não é zerado de novo
    State->>Metro: metronome_start(t0)
    loop cada batida
        Metro-->>App: BEAT (contagem / compasso, BPM, session_id)
    end
    loop cada nota tocada
        Audio-->>App: NOTE_ON (MIDI, Hz, confiança)
        Audio-->>App: NOTE_OFF (duração)
    end
    App->>Servico: SET_TEMPO (novo BPM, at_beat = batida do início da frase – RFA09)
    Servico->>Metro: metronome_set_tempo(bpm, at_beat)
    App->>Servico: SESSION_STOP / DISCONNECT
    Servico->>State: session_stop() / clear_peer()
```

### 5.6 Metrônomo

As batidas são agendadas em **instantes absolutos** a partir de `t0` (início da sessão), evitando deriva acumulada. O app calcula a sua linha do tempo com **o mesmo BPM inteiro** enviado ao metrônomo e alinha o relógio pelas batidas recebidas; a troca de andamento acontece na batida `at_beat` informada pelo app (início exato da frase), e não "na próxima batida após o pacote chegar". Durante os compassos de contagem, som e luz são sempre acionados (aviso de preparação – RFA05/RU07); depois, seguem as opções do app. A cada clique, o pipeline de áudio recebe uma máscara para não confundir o som do buzzer com uma nota.

---

## 6. Protocolo de comunicação

Definido em [`include/protocol.h`](MicroDetection/include/protocol.h) e espelhado no app em `lib/features/connection/data/protocol/device_protocol.dart`.

### 6.1 Transportes

| Transporte | Endereço | Usado por |
|---|---|---|
| UDP | porta **54322** (descoberta por broadcast) | App no Android, iOS, Windows, macOS e Linux |
| WebSocket | `ws://IP_DO_DISPOSITIVO/ws` (porta 80), uma mensagem binária por pacote | App na web (o navegador não tem UDP) |

### 6.2 Cabeçalho (12 bytes, little-endian)

| Campo | Tipo | Descrição |
|---|---|---|
| `magic` | `uint16` | `0x5443` ("TC") |
| `version` | `uint8` | Versão do protocolo (1) |
| `type` | `uint8` | Tipo do pacote |
| `sequence` | `uint32` | Número sequencial (detecção de perdas) |
| `timestamp_ms` | `uint32` | Tempo desde o início da sessão (o instante 0 é o início da contagem) |

### 6.3 Pacotes

| Código | App → dispositivo | Conteúdo |
|---|---|---|
| `0x01` | `DISCOVER` | — |
| `0x02` | `CONNECT` | nome do app |
| `0x03` | `DISCONNECT` | — |
| `0x04` | `PING` | — (a cada 1 s) |
| `0x05` | `CONFIG` | BPM, tempos por compasso, flags |
| `0x06` | `SESSION_START` | BPM, tempos por compasso, compassos de contagem, flags, `session_id` (reenvio com o mesmo id é ignorado), `beat_type` (denominador do compasso) |
| `0x07` | `SESSION_STOP` | — |
| `0x08` | `SET_TEMPO` | novo BPM e `at_beat`: índice da batida (contagem incluída) em que o novo BPM começa; 0 = próxima batida |
| `0x09` | `EXPECTED_NOTE` | índice, nota MIDI, início e duração (ms), duração em milésimos de semínima – somente para o monitor da execução no serial; enviado antes de cada trecho |

| Código | Dispositivo → app | Conteúdo |
|---|---|---|
| `0x81` | `ANNOUNCE` | nome, versão do firmware, MAC, estado, RSSI, taxa de amostragem |
| `0x82` | `CONNECT_ACK` | aceito (1) ou em uso por outro app (0) |
| `0x83` | `PONG` | estado, RSSI, piso de ruído, eventos descartados |
| `0x84` | `NOTE_ON` | nota MIDI, frequência, nível, confiança |
| `0x85` | `NOTE_OFF` | nota MIDI, duração, frequência média |
| `0x86` | `PITCH` | frequência contínua (afinador, opcional) |
| `0x87` | `BEAT` | tempo no compasso, contagem, índice do compasso, BPM, `session_id` |
| `0x88` | `AUDIO_FRAME` | 1024 amostras PCM 16 bits + energia (diagnóstico) |

Flags (`CONFIG`/`SESSION_START`): `0x01` metrônomo sonoro · `0x02` metrônomo visual · `0x04` áudio bruto · `0x08` altura contínua.

Os campos `session_id`, `at_beat` e `beat_type` ocupam bytes que eram reservados (zero), então apps e firmwares anteriores continuam compatíveis.

Regras: somente o app pareado controla a sessão; outro app só assume o dispositivo se o anterior ficar 5 s sem enviar `PING` (RU02); `DISCONNECT` libera o dispositivo imediatamente (RU17).

---

## 7. Configuração

Principais parâmetros de [`include/config.h`](MicroDetection/include/config.h):

| Grupo | Parâmetro | Padrão | Descrição |
|---|---|---|---|
| Áudio | `SAMPLE_RATE` | 16000 | Taxa de amostragem (Hz) |
| | `HOP_SAMPLES` / `WINDOW_SAMPLES` | 256 / 512 | Passo (16 ms) e janela de análise (32 ms) |
| | `INPUT_GAIN` | 4.0 | Ganho digital do INMP441 |
| DSP | `BANDPASS_LOW_HZ` / `BANDPASS_HIGH_HZ` | 200 / 2500 | Faixa do filtro |
| | `PITCH_MIN_HZ` / `PITCH_MAX_HZ` | 26,7 / 4400 | Faixa de altura (A0 – C8, com margem) |
| | `PITCH_SPLIT_HZ` | 125 | Abaixo: faixa grave (sinal decimado para 4 kHz, janela de 80 ms); acima: faixa aguda (16 kHz, janela de 32 ms) |
| | `MIN_MIDI_NOTE` / `MAX_MIDI_NOTE` | 21 / 108 | Notas aceitas (A0 – C8) |
| | `YIN_THRESHOLD` / `MIN_CONFIDENCE` | 0.15 / 0.70 | Limiar do YIN e confiança mínima |
| Notas | `GATE_MARGIN_DB` / `GATE_MIN_DB` | 12 / −62 | Margem sobre o piso de ruído e limiar absoluto |
| | `STABLE_HOPS` / `MIN_NOTE_MS` | 2 / 60 | Confirmação da altura e duração mínima |
| | `QUANTIZE_MS` | 10 | Quantização temporal (RNFE04) |
| | `CLICK_MASK_MS` | 45 | Máscara do clique do metrônomo |
| Rede | `WIFI_DEFAULT_SSID` / `WIFI_DEFAULT_PASS` | `JOAO_2.4G` | Rede padrão: usada quando nada é digitado ao ligar ou quando a rede digitada não conecta |
| | `WIFI_SETUP_TIMEOUT_MS` | 15000 | Tempo para começar a digitar a rede ao ligar |
| | `MDNS_HOSTNAME` | `partitura` | Nome na rede local (`partitura.local`) |
| | `WIFI_MAX_RETRIES` / `WIFI_FATAL_RETRIES` | 8 / 3 | Tentativas antes da rede própria (3 quando a causa não se resolve sozinha: senha incorreta, rede inexistente) |
| | `WIFI_COUNTRY_CODE` | `BR` | Canais 1–13 (modems nos canais 12 e 13) |
| | `WIFI_DHCP_TIMEOUT_MS` | 12000 | Associado, mas sem IP: falha "DHCP" |
| | `WIFI_AP_PASS` / `WIFI_AP_CHANNEL` | `partitura123` / 6 | Rede própria |
| | `WS_PORT` | 80 | Servidor WebSocket |
| | `SESSION_TIMEOUT_MS` | 5000 | Tempo sem `PING` para liberar o dispositivo |
| | `EVENT_QUEUE_LENGTH` / `STREAM_QUEUE_LENGTH` | 32 / 16 | Fila prioritária (notas, batidas) e de fluxo (altura, áudio bruto) |
| | `WS_SEND_TIMEOUT_S` | 1 | Tempo máximo de um envio por WebSocket |
| Menu | `SERIAL_UI_ANSI` | 1 | Cores e cursor ANSI; 0 para terminais sem ANSI (ex.: IDE Arduino) |
| | `SERIAL_UI_IDLE_RESET_MS` | 20000 | Sem tecla por 20 s após uma interação, o menu volta ao início |

As opções do ESP-IDF necessárias (suporte a WebSocket no `esp_http_server`) estão em `sdkconfig.defaults`.

---

## 8. Compilação e gravação

### 8.1 PlatformIO (recomendado)

```bash
cd MicroDetection
pio run -e esp32doit-devkit-v1 -t upload      # ESP32 DevKit V1
pio run -e esp32-s3-devkitc-1 -t upload       # ESP32-S3 N16R8
pio device monitor                            # 115200 bps
```

### 8.2 ESP-IDF (`idf.py`)

```bash
cd MicroDetection
idf.py set-target esp32        # ou esp32s3
idf.py build flash monitor
```

Sem o ESP-IDF instalado, a imagem oficial em Docker compila o projeto:

```bash
docker run --rm -v "$PWD":/project -w /project espressif/idf:v5.5.3 \
    idf.py -B build-esp32 -DSDKCONFIG=/project/sdkconfig.esp32doit-devkit-v1 build
```

> Validação da versão 1.1.0: compilação sem avisos com ESP-IDF 5.5.3 para **ESP32** (`MicroDetection.bin` ≈ 882 KB, 14% livre na partição) e **ESP32-S3** (≈ 874 KB, 15% livre).
>
> Versão 1.2.0: validada no computador (testes `make -C MicroDetection/test/host`, com `-Wall -Wextra -Werror`). Na primeira compilação o PlatformIO/`idf.py` baixa o componente `espressif/mdns` (internet necessária); sem ele o firmware compila sem o anúncio `partitura.local`.

---

## 9. Operação do dispositivo

### 9.1 Wi-Fi

Ao ligar, o monitor serial pergunta a rede:

```
========================================================================
  PARTITURA IoT - firmware 1.2.0 - REDE WI-FI
========================================================================
  Rede padrao: "JOAO_2.4G"
  Digite o nome da rede (SSID) a que o dispositivo deve se conectar
  e tecle ENTER. Depois, digite a senha.
  ENTER sem digitar, ESC ou 15 s sem tecla: usa a rede padrao.
  Se a rede digitada nao conectar, a rede padrao e usada.

  Nome da rede: (15 s)
```

1. Digite o SSID e tecle ENTER; depois a senha (aparece como `*`; ENTER vazio = rede aberta).
2. O dispositivo tenta a rede digitada. Se ela não conectar (senha incorreta, rede inexistente, sinal fraco...), passa automaticamente para a **rede padrão** (`JOAO_2.4G`, `WIFI_DEFAULT_SSID` no `config.h`) e avisa no menu.
3. Sem resposta em 15 s (ex.: dispositivo ligado sem computador), usa direto a rede padrão.
4. Se nenhuma rede conectar, cria a rede própria **PartituraIoT-XXXX** (senha `partitura123`, IP `192.168.4.1`) e continua tentando a cada 15 s.

Com o dispositivo funcionando também é possível trocar de rede pelo menu (**2 – Rede Wi-Fi**); a rede padrão continua sendo a reserva. A rede não é gravada na NVS: a pergunta é feita a cada inicialização. O app precisa estar na mesma rede do dispositivo.

Compatibilidade com modems domésticos (corrigida nesta versão):

- **WPA3 / WPA2-WPA3**: o SAE agora aceita os dois métodos (hunt-and-peck e hash-to-element). Antes a configuração zerada usava só o primeiro, recusado por modems Wi-Fi 6 em WPA3 – enquanto o roteador do celular (WPA2) funcionava;
- **canais 12 e 13**: país `BR` configurado (antes, modo mundial com canais 1–11 ativos);
- **PMF (802.11w)** opcional e canal de **20 MHz**;
- SSID de 32 caracteres e chave hexadecimal de 64 dígitos aceitos.

Se ainda assim não conectar, o menu mostra a causa (senha incorreta, rede não encontrada, rede de 5 GHz, sinal fraco, segurança incompatível, modem recusou – filtro de MAC –, sem IP do DHCP) e o que fazer.

O dispositivo se anuncia por **mDNS** como `partitura.local`. O app procura sozinho: broadcast (celular/computador), endereços já usados, `partitura.local`, `192.168.4.1` e, se nada responder, a sub-rede inteira (no **app web**, sem broadcast, as sub-redes mais comuns de modems domésticos). Se a rede usar uma faixa incomum, informe o IP (topo do menu serial) ou `partitura.local` em "Conectar pelo IP". Abra o app web por `http://` (páginas `https://` não podem abrir conexões `ws://` sem criptografia).

### 9.2 LED de status

| Padrão | Significado |
|---|---|
| Piscando rápido | Conectando ao Wi-Fi |
| Duas piscadas curtas | Rede própria (modo AP) ativa |
| Piscando lento | Wi-Fi conectado, aguardando o app |
| Aceso | App pareado |
| Piscando (erro) | Falha crítica na inicialização (ver monitor serial) |

### 9.3 Menu serial (115200 bps)

O terminal mostra **somente o menu**, em **ASCII puro** (sem acentos, cedilha ou símbolos especiais – legível em qualquer monitor serial). As mensagens técnicas do ESP_LOG e as mensagens precoces do sistema (`esp_rom_printf`), como o aviso do watchdog de tarefas (`task_wdt`) e o `Backtrace`, vão para o *Log tecnico* e só aparecem quando ele é aberto. A tela é redesenhada no lugar, em vermelho e branco: no topo, o estado do Wi-Fi, do app, do microfone e da sessão; abaixo, os **problemas atuais com o que fazer** e os últimos avisos; por fim, as opções.

```
  PARTITURA IoT  -  firmware 1.2.0  -  PartituraIoT-B2C3
 Wi-Fi   conectando a "CasaDoAluno"...
 App     aguardando conexao
 Mic     OK    Sessao  parada
----------------------------------------------------------------------------
 ! Senha incorreta para a rede "CasaDoAluno"
   > Menu 2 > 1: digite a senha de novo (maiusculas contam; TAB mostra).
   > Se a senha estiver certa, reinicie o modem e use o Menu 2 > 3.
   > Modem em "somente WPA3"? Troque para "WPA2/WPA3" ou "WPA2".
----------------------------------------------------------------------------
 MENU PRINCIPAL
  > 1  Status do sistema
    2  Rede Wi-Fi  >>
    3  Captura e audio  >>
    4  Log tecnico
    5  Reiniciar dispositivo

 Setas mover - ENTER escolhe - 1-5 atalho
 Sem tecla por 20 s, o menu volta ao inicio.
```

| Menu | Opções |
|---|---|
| 1 – Status do sistema | IP, sessão, metrônomo, notas detectadas, pacotes e erros, eventos perdidos, buffer, nível de entrada, piso de ruído, tempo de processamento por janela, memória livre (ao vivo) |
| 2 – Rede Wi-Fi | 1 Escolher rede da lista · 2 Digitar o nome (rede oculta) · 3 Tentar conectar de novo · 4 Detalhes da conexão (causa da falha, tentativa, sinal, canal, segurança, MAC) |
| 3 – Captura e áudio | 1 Iniciar/parar captura local · 2 Monitor de notas · 3 Afinador e nível do microfone · 4 Testar metrônomo · 5 Envio de áudio bruto · 6 Monitor da execução do app |
| 4 – Log técnico | Últimas linhas do ESP_LOG e das mensagens do sistema (`task_wdt`, `Backtrace`) |
| 5 – Reiniciar dispositivo | Pede confirmação |

Navegação: **↑/↓** e **ENTER**, ou o número da opção; **ESC**/**←** volta. Depois de qualquer interação, **20 s sem tecla** fazem o menu voltar ao início (o que estava sendo digitado é descartado). Funciona no monitor do PlatformIO (`monitor_filters = direct`), PuTTY, screen e minicom; no Monitor Serial da IDE Arduino (sem ANSI) use `SERIAL_UI_ANSI 0`.

### 9.4 Monitor da execução (esperado × captado)

Quando o app inicia uma execução, o terminal deixa de redesenhar o menu e passa a mostrar, linha a linha e no relógio da sessão (0 ms = primeira batida da contagem), as notas **ESPERADAS** da partitura – enviadas pelo app antes de cada trecho e impressas na hora de cada uma – e as notas **CAPTADAS** pelo microfone, com a comparação com a esperada mais próxima e, ao soltar, a figura correspondente à duração medida:

```
=== EXECUCAO INICIADA: 100 BPM, compasso 4/4, contagem de 2 compasso(s) (4800 ms) ===
    Contagem de entrada: notas tocadas agora sao ignoradas pelo app.
--- Musica iniciada (t=4800 ms) ---
ESPERADO #0   C4   seminima               t=  4800 ms  dur  600 ms
CAPTADO       C4   261.7 Hz               t=  4810 ms  -> esperado #0 C4: altura OK, tempo +10 ms
  solta       C4   seminima               t=  5380 ms  dur  570 ms
ESPERADO #1   D4   colcheia               t=  5400 ms  dur  300 ms
CAPTADO       E4   329.6 Hz               t=  5420 ms  -> esperado #1 D4: altura ERRADA, tempo +20 ms
ESPERADO #2   B6   fusa                   t=  5700 ms  dur   75 ms
=== EXECUCAO ENCERRADA: 3 nota(s) esperada(s), 3 captada(s), 1 com a altura certa ===
```

ESC volta ao menu (o monitor continua disponível em *Captura e audio › 6*). O mesmo log aparece no console do navegador, gerado pelo app.

Problemas diagnosticados e explicados no menu:

| Área | Situações |
|---|---|
| Wi-Fi | nenhuma rede cadastrada · senha com tamanho inválido · **senha incorreta** · **rede inexistente** (com sugestão de nome parecido e aviso de 5 GHz) · segurança incompatível · sinal fraco · modem recusou (filtro de MAC/limite) · sem IP (DHCP) · conexão caiu · rede própria ativa |
| Microfone | sem sinal (fios do INMP441, com os pinos da placa) · I2S sem amostras · saturação · falha ao iniciar |
| App | aguardando conexão (com o IP para "Conectar pelo IP") · app parou de responder · falhas de envio · eventos perdidos por rede lenta |

---

## 10. Testes

| Teste | Comando | O que valida |
|---|---|---|
| DSP no computador | `make -C MicroDetection/test/host` | Filtro, detector de altura e segmentação com sinais sintéticos de teclado, ruído de sala e cliques do metrônomo; A0 a C8, 4º harmônico dominante (E4 não vira E6), Hino à Alegria só com o 4º harmônico (C4–G4 não viram C6–G6), fusas em B6, cliques sobre nota longa e nota repetida; NOTE_ON retido só com dúvida de oitava; atraso de detecção (média ≤ 70 ms, pior ≤ 130 ms) |
| Altura (88 notas) | `make -C MicroDetection/test/host pitch` | Todas as notas do piano com inarmonicidade de corda: perfil de teclado e senoide (A0–C8), fundamental fraca (A0–A6), 4º harmônico dominante (A0–F#5), alto-falante pequeno com corte em 1,2 kHz (A3–C7) e harmônico quase isolado (D2–C5); erro médio de afinação ≤ 25 cents; custo por trecho (iterações de YIN e Goertzel, independente da máquina): trecho com nota ≤ 2,1x o trecho sem nota |
| Monitor da execução | `make -C MicroDetection/test/host monitor` | Linhas ESPERADO/CAPTADO, figuras, contagem de entrada e resumo, somente ASCII |
| Dispositivo falso (UDP) | `make -C MicroDetection/test/host fake_device && ./MicroDetection/test/host/fake_device` | Protocolo completo para testar o app sem hardware (descoberta, pareamento, batidas e escala de Dó) |
| Ponte WebSocket | `python3 MicroDetection/test/host/ws_bridge.py 80` (com o `fake_device` rodando) | App web conversando com o dispositivo falso por `ws://127.0.0.1/ws` |
| Compilação do firmware | ver seção 8 | ESP32 e ESP32-S3 |

Testes de integração no repositório do app:

```bash
# App nativo (UDP)
FAKE_DEVICE=/caminho/fake_device flutter test test/connection/udp_device_test.dart

# App web (WebSocket, no Chrome)
flutter test --platform chrome --dart-define=WS_DEVICE=true \
    test/connection/websocket_device_test.dart
```

---

## 11. Solução de problemas

| Sintoma | Causa provável | Ação |
|---|---|---|
| LED pisca rápido sem parar | Rede ou senha incorretas | O topo do menu serial mostra a causa e o que fazer; Menu 2 › 1 para escolher a rede e digitar a senha |
| Conecta no roteador do celular, mas não no modem | Modem em WPA3 (H2E), canal 12/13, 5 GHz ou filtro de MAC | Versão 1.1.0 corrige WPA3 e canais; para 5 GHz/filtro, siga o diagnóstico do menu (Menu 2 › 4) |
| App não encontra o dispositivo | Dispositivo na rede própria (a rede digitada e a padrão falharam), redes diferentes ou isolamento de clientes no modem | Veja o topo do menu serial (rede e IP); use "Conectar pelo IP" com o IP ou `partitura.local` |
| `task_wdt` / `Backtrace` | Carga de processamento alta (versões anteriores: `-Og` e 160 MHz) | Desde a 1.2.0 a CPU roda a 240 MHz com `-O2` (`sdkconfig.<placa>`); se ocorrer, a mensagem fica no *Log tecnico* (Menu 4). Em compilações com `idf.py`, apague o `sdkconfig` antigo para aplicar o `sdkconfig.defaults` |
| App web não conecta | Página aberta por `https://` ou IP errado | Abra o app web por `http://` e informe o IP do dispositivo |
| Menu mostra "Microfone sem sinal" | Ligações do INMP441 | Confira SCK/WS/SD, VDD em 3V3 e L/R em GND (o menu mostra os pinos da placa) |
| Notas falsas com o metrônomo | Buzzer muito próximo do microfone | Afaste o buzzer ou reduza o volume; ajuste `CLICK_MASK_MS` |
| Nota lida uma ou duas oitavas acima | Harmônico mais forte que a fundamental no som captado (ex.: alto-falante pequeno) ou taxa do microfone errada | Corrigido na 1.3.0–1.3.2 (correção de oitava e confirmação antes de anunciar a nota). Se ainda ocorrer, veja no monitor da execução: (1) o cabeçalho "Microfone: X amostras/s medidas" — deve ficar perto de 16000; ~4000 indica relógio do I2S errado; (2) o final de cada linha CAPTADO, `[f/2 … dB, f/4 … dB]` (energia relativa ao total da janela): numa nota lida alta demais, valores acima de ~−45 dB mostram que a nota certa estava no som (envie o log); valores abaixo de ~−60 dB indicam que ela quase não chega ao microfone. Aproxime o microfone do alto-falante ou use fones/saída de linha do teclado |
| Feedback no app atrasado em relação ao toque | Processamento no limite ou rede lenta | Veja no status (Menu 4) "Processamento" (pior janela, deve ficar bem abaixo de 16 000 us) e "Atraso de deteccao" (do ataque ao envio, ~40–110 ms). Se o processamento passar de 12 000 us, confira a CPU a 240 MHz e `-O2`; se a detecção estiver normal, o atraso está na rede (Wi-Fi do modem/celular) |
| Leituras de 60 Hz (A#1/B1) sem tocar | Zumbido da rede elétrica captado pelo microfone ou pelos fios | Afaste fonte/carregador dos fios do INMP441, use cabos curtos e alimente pela USB do computador |
| Notas não detectadas | Nível baixo | Aproxime o microfone ou aumente `INPUT_GAIN`; verifique nível e piso de ruído no afinador (Menu 3 › 3) |

---

## 12. Licença

Distribuído sob a licença do repositório (ver [`LICENSE`](LICENSE)). O histórico da concepção inicial do projeto está em [`docs/historico-concepcao.md`](docs/historico-concepcao.md).
