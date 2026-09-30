# firmware_I2S

Firmware do dispositivo embarcado do TCC **Sistema de Processamento de Áudio com Feedback em Tempo Real Aplicado ao Ensino de Partitura**.

O dispositivo capta o som do teclado com o **microfone digital INMP441 (I2S)**, identifica as notas tocadas (altura e duração) e envia os eventos via **Wi-Fi (UDP)** para o [aplicativo Flutter](https://github.com/noggyamamoto/Flutter_App), que compara a execução com a partitura MusicXML. Também executa o metrônomo sonoro (buzzer) e visual (LED RGB).

## Stack

- **C**
- **ESP-IDF 5.x** (FreeRTOS, driver I2S `i2s_std`, LEDC, esp_timer, NVS, lwIP)
- **PlatformIO**

## Funcionalidades (requisitos do TCC)

| Requisito | Implementação |
|---|---|
| RFE01 – Verificação de rede | Varredura das redes, conexão com novas tentativas, rede própria de reserva (modo AP) e LED de status |
| RFE02 – Metrônomo sonoro | Buzzer passivo (LEDC), clique acentuado no tempo forte |
| RFE03 – Metrônomo visual | LED RGB: azul (binário), verde (ternário), roxo (quaternário), amarelo na contagem |
| RFE04 – Captação digital e filtragem | INMP441 via I2S (24 bits em slots de 32 bits, 16 kHz) + passa-banda 70 Hz–2,5 kHz |
| RFE05 – Processamento digital de sinais | Altura pelo algoritmo **YIN** + segmentação em notas (ataque, soltura, duração) |
| RFE06 – Transmissão | Eventos `NOTE_ON`/`NOTE_OFF` em pacotes UDP binários com sequência e timestamp |
| RNFE01 – Baixa latência | Janelas de 16 ms; tempo de processamento exibido no menu serial (opção 3) |
| RNFE03 – Imunidade a ruído | Filtro passa-banda + piso de ruído adaptativo + limiar mínimo |
| RNFE04 – Tolerância temporal | Quantização dos tempos em 10 ms |

## Hardware

| INMP441 | ESP32 DevKit V1 | ESP32-S3 N16R8 |
|---|---|---|
| VDD | 3V3 | 3V3 |
| GND | GND | GND |
| L/R | GND | GND |
| SCK | GPIO26 | GPIO5 |
| WS | GPIO25 | GPIO6 |
| SD | GPIO33 | GPIO4 |

| Indicador | ESP32 DevKit V1 | ESP32-S3 N16R8 |
|---|---|---|
| LED de status (+ resistor 220 Ω) | GPIO2 (LED on-board) | GPIO2 |
| LED RGB catodo comum (R/G/B, resistores 220 Ω) | GPIO27 / 14 / 13 | GPIO15 / 16 / 17 |
| Buzzer passivo (via transistor) | GPIO18 | GPIO18 |

Os pinos ficam em `MicroDetection/src/config.h` (para LED RGB de anodo comum, use `RGB_LED_COMMON_ANODE 1`).

**LED de status:** piscando rápido = conectando ao Wi-Fi · duas piscadas = rede própria ativa · piscando lento = Wi-Fi conectado, aguardando o app · aceso = app pareado.

## Compilar e gravar

```bash
cd MicroDetection
pio run -e esp32doit-devkit-v1 -t upload      # ESP32 DevKit V1
pio run -e esp32-s3-devkitc-1 -t upload       # ESP32-S3 N16R8
pio device monitor
```

### Wi-Fi

Na primeira execução não há rede configurada: o dispositivo cria a rede **PartituraIoT-XXXX** (senha `partitura123`). Para usar a rede da escola/casa, abra o monitor serial e escolha a opção **5 – Configurar rede Wi-Fi**; as credenciais ficam salvas na NVS. O app precisa estar na mesma rede que o dispositivo.

### Menu serial (115200 bps)

```
1 - Iniciar captura local (teste sem o app)
2 - Parar captura
3 - Status
4 - Escanear redes Wi-Fi
5 - Configurar rede Wi-Fi
6 - Liga/desliga envio de áudio bruto (diagnóstico)
7 - Testar metrônomo (2 compassos)
8 - Afinador (5 s de leitura contínua)
9 - Reiniciar dispositivo
```

## Arquitetura do firmware

```
I2S (INMP441) ─DMA─► audio_prod (núcleo 1, prio 5)
                      │ passa-banda → janela 1024 → YIN → segmentação
                      ▼
                 fila de eventos ──► net_tx (núcleo 0) ──UDP──► app
                                           ▲
          metrônomo (esp_timer) ─ batidas ─┘
app ──UDP──► net_rx (núcleo 0): DISCOVER, CONNECT, PING, SESSION_START/STOP, SET_TEMPO
```

| Arquivo | Função |
|---|---|
| `main.c` | Inicialização e criação das tarefas |
| `config.h` | Pinos, parâmetros de áudio, DSP e rede |
| `protocol.h` | Formato binário dos pacotes UDP (espelhado no app) |
| `audio_capture.c` | Driver I2S do INMP441 |
| `dsp.c` | Passa-banda (biquads RBJ) e detector de altura YIN |
| `note_tracker.c` | Ataque/soltura, piso de ruído, reataque, legato, quantização |
| `audio_pipeline.c` | Tarefa de áudio: captura → DSP → eventos |
| `udp_link.c` | Recepção de comandos e envio de eventos |
| `metronome.c` | Metrônomo sonoro/visual com contagem de entrada |
| `wifi_manager.c` | Varredura, conexão, novas tentativas e modo AP |
| `indicators.c` | LED de status, LED RGB e buzzer |
| `settings.c` | Credenciais na NVS |
| `serial_ui.c` | Menu serial e diagnóstico |
| `circular_buffer.c` | Buffer circular de quadros brutos (modo diagnóstico) |

## Protocolo UDP

Todos os pacotes começam com um cabeçalho de 12 bytes (little-endian): `magic 0x5443 | versão | tipo | sequência | timestamp_ms`. O dispositivo escuta na porta **54322** e responde ao endereço de origem do app. Detalhes em [`src/protocol.h`](MicroDetection/src/protocol.h).

| App → dispositivo | Dispositivo → app |
|---|---|
| `DISCOVER` (broadcast) | `ANNOUNCE` (nome, firmware, MAC, RSSI) |
| `CONNECT` | `CONNECT_ACK` |
| `PING` (1 s) | `PONG` (estado, RSSI, ruído) |
| `SESSION_START` (BPM, compasso, contagem) | `BEAT` (a cada tempo) |
| `SET_TEMPO`, `CONFIG`, `SESSION_STOP`, `DISCONNECT` | `NOTE_ON` / `NOTE_OFF` (MIDI, Hz, duração) |

O tempo 0 da sessão é o início da contagem de entrada; a música começa após 2 compassos.

## Testes no computador

O DSP (filtro, YIN e segmentação) não depende do ESP-IDF e é testado com sinais sintéticos de teclado, com ruído de sala e cliques do metrônomo:

```bash
make -C MicroDetection/test/host          # testes do pipeline de DSP
make -C MicroDetection/test/host fake_device
./MicroDetection/test/host/fake_device    # dispositivo falso para testar o app sem hardware
```

---

## Documento de concepção (versão inicial do projeto)

> A seção abaixo registra a concepção inicial. Na implementação atual, a detecção de notas é feita no próprio ESP32 (requisito RFE05 do TCC) e o app recebe apenas os eventos de nota.


## 1. Visão Geral e Objetivos

O sistema captura uma performance em teclado elétrico e gera automaticamente a partitura correspondente.

Para atender ao rigor científico, o projeto adota uma arquitetura híbrida:

- **Entrada MIDI (Musical Instrument Digital Interface)** – referência perfeitamente conhecida – serve como **ground truth** (referência de validação) para comparação objetiva.
- **Entrada de áudio** (capturada por microfone ou saída de linha) – processada pelo algoritmo de transcrição automática.

Assim é possível medir objetivamente a qualidade da transcrição por áudio, comparando-a com os eventos MIDI registrados simultaneamente. O sistema compõe-se de um subsistema embarcado (ESP32) para captura e envio do sinal de áudio (opcionalmente também do MIDI) e de um aplicativo Flutter que concentra todo o processamento inteligente, a geração da partitura e a validação.

## 2. Arquitetura Geral

```text
┌──────────────────────────┐       Wi-Fi (UDP)        ┌──────────────────────────────────────────┐
│  Teclado Elétrico        │                          │  Aplicativo Flutter (Mobile / Desktop)   │
│                          │                          │                                          │
│  ┌─────────┐  Line Out   │   ┌──────────────────┐  │  ┌──────────────────────────────────┐    │
│  │ Saída   │─────────────┼──▶│ ESP32            │  │  │ Módulo de Recepção               │    │
│  │ Áudio   │             │   │ (I2S INMP441 ou  │  │  │ - Buffer de pacotes UDP          │    │
│  └─────────┘             │   │ entrada direta)  │  │  │ - Reconstrói stream de áudio     │    │
│                          │   │ - Filtro passa-banda│  │  └────────────┬─────────────────────┘    │
│  ┌─────────┐             │   │                  │  │               │                          │
│  │ Saída   │─────────────┼──▶│ - Normalização   │  │               ▼                          │
│  │ MIDI    │             │   │ - Buffer circular│  │  ┌──────────────────────────────────┐    │
│  └─────────┘             │   │ - Encapsulamento │──┼─▶│ Pipeline de Processamento Áudio  │    │
│                          │   │   binário +      │  │  │ (Dart FFI → C/C++)               │    │
│                          │   │   cabeçalho      │  │  │ - FFT (1024 amostras)            │    │
│                          │   └──────────────────┘  │  │ - Detecção de Pitch (f → MIDI)   │    │
│                          │                         │  │ - Chroma Vector                  │    │
│                          │                         │  │ - Detecção de Acordes (templates)│    │
│                          │                         │  │ - Detecção de Onset (energia/fluxo)│  │
│                          │                         │  └────────────┬─────────────────────┘    │
│                          │                         │               │                          │
│                          │                         │               │ Notas/Acordes detectados  │
│                          │                         │               ▼                          │
│                          │  ┌──────────────────┐   │  ┌──────────────────────────────────┐    │
│                          │  │ Cabo MIDI → USB  │───┼─▶│ Módulo MIDI (Ground Truth)       │    │
│                          │  │ (direto ao app)  │   │  │ - Parser de eventos MIDI        │    │
│                          │  └──────────────────┘   │  │ - Agrupamento polifônico         │    │
│                          │                         │  └────────────┬─────────────────────┘    │
│                          │                         │               │ Notas “reais”             │
│                          │                         │               ▼                          │
│                          │                         │  ┌──────────────────────────────────┐    │
│                          │                         │  │ Módulo de Validação              │    │
│                          │                         │  │ - Compara notas (altura, onset,  │    │
│                          │                         │  │   duração) entre áudio e MIDI    │    │
│                          │                         │  │ - Calcula métricas: Precisão,    │    │
│                          │                         │  │   Revocação, Erro de frequência, │    │
│                          │                         │  │   Atraso de onset                │    │
│                          │                         │  └────────────┬─────────────────────┘    │
│                          │                         │               │                          │
│                          │                         │               ▼                          │
│                          │                         │  ┌──────────────────────────────────┐    │
│                          │                         │  │ Construtor de Partitura          │    │
│                          │                         │  │ - Quantização rítmica            │    │
│                          │                         │  │ - Modelo de dados (Note, Chord)  │    │
│                          │                         │  │ - Renderização (VexFlow/WebView) │    │
│                          │                         │  └──────────────────────────────────┘    │
│                          │                         │                                          │
└──────────────────────────┘                         └──────────────────────────────────────────┘
```

> **Nota:** O caminho principal deste projeto usa microfone I2S INMP441. O áudio também pode vir da saída de fone/line do teclado para o ESP32 via entrada ADC (Analog-to-Digital Converter) com acoplamento AC para remover offset DC. Isso ajuda a manter um sinal limpo e controlado.
> O MIDI pode chegar por USB/DIN ao sistema, e na arquitetura proposta segue diretamente para o Flutter via USB, evitando a necessidade do ESP32 nessa rota.

## 3. Decisões de Projeto e Fundamentação

### 3.1 Por que concentrar a inteligência no Flutter, e não no ESP32?

- **Capacidade computacional:** O ESP32 não possui poder de processamento suficiente para executar, com folga em tempo real, uma FFT (Fast Fourier Transform – Transformada Rápida de Fourier) em alta resolução espectral, detecção simultânea de múltiplos pitches (alturas) e comparação com templates complexos de acordes.
- **Manutenibilidade e depuração:** Todo o código de análise musical reside em um ambiente de desenvolvimento moderno (Dart / C++ via FFI – Foreign Function Interface), com ferramentas robustas de teste e profiling.
- **Modularidade:** Isolando o hardware embarcado apenas para captura e transmissão, o sistema fica mais simples de validar e pode evoluir independentemente (ex.: substituir o ESP32 por outro módulo de áudio sem afetar o núcleo do projeto).

### 3.2 Por que usar MIDI como ground truth?

- **Precisão absoluta:** O MIDI fornece exatamente quais notas foram tocadas, com onset (instante de início), duração e velocity (velocidade/força de toque), sem ambiguidade de altura ou mistura harmônica.
- **Validação científica robusta:** A transcrição por áudio é um problema complexo (polifonia, ruído, sobreposição espectral). Comparar o resultado do algoritmo com uma referência confiável permite calcular métricas objetivas (erro de nota, atraso de onset etc.), conferindo rigor à pesquisa.
- **Facilidade para experimentos controlados:** O teclado elétrico garante execuções repetíveis, essenciais para avaliação quantitativa.

### 3.3 Por que usar Wi-Fi com UDP para áudio, e não BLE ou TCP?

- **BLE:** Largura de banda frequentemente insuficiente para streaming de áudio contínuo de baixa latência (especialmente em cenários BLE 4.x e com overhead prático de enlace/aplicação), e alto overhead para pacotes pequenos. Mesmo para 16 kHz / 16 bits em mono (taxa bruta de 256 kbps), a estabilidade ponta a ponta pode ficar comprometida.
- **TCP:** Embora confiável, o mecanismo de retransmissão introduz latência acumulada e variações de jitter (variação no atraso) que podem distorcer a linha de tempo, prejudicando a detecção rítmica.
- **UDP:** Oferece baixa latência e jitter previsível. Perdas ocasionais de pacotes são toleráveis, pois o sistema pode realizar interpolação ou simplesmente lidar com um pequeno gap que não compromete a análise espectral. Para compensar pacotes perdidos, utiliza-se numeração sequencial no cabeçalho do quadro (frame).

### 3.4 Formato de transmissão do áudio

Optou-se por binário com cabeçalho estruturado em vez de JSON ou texto puro:

- **Eficiência:** JSON multiplica o tamanho dos dados por 5–10×, desperdiçando banda e aumentando a latência.

Estrutura proposta (16 bytes de cabeçalho + N amostras):

```text
[uint32 timestamp_ms]        // tempo em milissegundos
[uint32 sequence_number]     // número sequencial do pacote
[uint32 num_samples]         // ex.: 512
[float  energy]              // energia média do quadro
[int16 samples[num_samples]] // amostras de áudio com sinal de 16 bits
```

O campo `energy`, já pré-calculado, auxilia na detecção de onset dentro do Flutter, sem custo adicional.

### 3.5 Processamento de áudio no Flutter: por que C/C++ via FFI?

Flutter/Dart é adequado para lógica de alto nível e UI, mas o processamento intensivo de sinais (DSP – Digital Signal Processing) como FFT, auto-correlação e operações com vetores seria ineficiente em Dart puro.

Usando `dart:ffi`, compila-se uma biblioteca nativa (em C/C++) com algoritmos otimizados (ex.: FFTW, KissFFT ou implementação própria). A vantagem é desempenho próximo ao nativo e acesso direto a buffers de áudio.

O pipeline de áudio fica encapsulado em uma classe que recebe buffers de amostras e retorna eventos musicais (listas de notas ou acordes), mantendo a separação de responsabilidades.

### 3.6 Detecção de acordes: Chroma Vector + Template Matching

O chroma vector (vetor cromático) reduz o espectro a 12 classes de altura (uma por semitom), tornando o reconhecimento de acordes independente da oitava e mais robusto.

Para um teclado com, no máximo, 3–4 notas simultâneas, a abordagem por templates (comparação da similaridade do cosseno com modelos armazenados de acordes maiores, menores, com 7ª etc.) funciona com excelente acurácia.

O uso exclusivo de áudio para acordes mais complexos (ex.: tétrades, disposições abertas) pode ter limitações; por isso o MIDI é essencial para avaliar os limites do algoritmo.

### 3.7 Quantização rítmica

Transformar tempos absolutos (em segundos) em figuras musicais (semínima, colcheia etc.) requer:

- Estimação do BPM (Batidas por Minuto), obtida pelo espaçamento entre ataques sucessivos (onset detection – detecção de início de nota).
- Construção de uma grade rítmica adaptativa (ex.: usando algoritmo de clustering dos onsets associado a um BPM predominante).
- Aproximação de cada onset para a posição rítmica mais próxima, respeitando um limiar de erro (ex.: ±30 ms). Notas muito distantes da grade podem indicar erro de detecção, métrica que também se avalia comparando com o MIDI.

## 4. Descrição dos Componentes Principais

### 4.1 Subsistema Embarcado (ESP32)

**Responsabilidades:**

- Aquisição de áudio via I2S (microfone INMP441 ou entrada analógica condicionada via ADC), com taxa de amostragem de 16 kHz / 16 bits (equilíbrio ideal entre qualidade espectral e taxa de dados).
- Pré-processamento leve:
  - Filtro passa-banda digital (80 Hz – 2 kHz) para remover ruídos fora da faixa útil do teclado.
  - Normalização de amplitude para evitar saturação.
- Empacotamento em frames binários com timestamp, sequência e energia.
- Envio via UDP a cada quadro (ex.: a cada 512 amostras ≈ 32 ms), com buffer circular para absorver bursts (rajadas) de transmissão.

**Nota sobre o MIDI:** O ESP32 pode opcionalmente atuar como ponte MIDI-Wi-Fi, mas na arquitetura proposta o MIDI vai direto ao Flutter via USB para simplicidade e menor latência.

### 4.2 Aplicativo Flutter

**Módulos:**

- **Receptor UDP:** isolate separado que recebe pacotes, trata perdas e monta o buffer contínuo de áudio.
- **Parser MIDI** (via biblioteca como `flutter_midi_command` ou similar): abstrai eventos MIDI para uma lista de notas com `(midiNote, velocity, startTime, endTime)`, tratando corretamente polifonia e pedal de sustain.
- **Motor de análise de áudio (C++/FFI):**
  - FFT com janela de Hann (1024 amostras, sobreposição de 50%), gerando espectro de magnitude.
  - Detecção de pitch por pico espectral refinado: para cada quadro estima a frequência fundamental (`f`, em Hz); converte para número MIDI com a fórmula `midiNote = 69 + 12 * log2(f / 440.0)`.
  - Chroma: reduz o espectro a um vetor de 12 posições (uma por semitom).
  - Detecção de acordes: compara o chroma acumulado em janelas temporais com templates ideais (usando distância do cosseno).
  - Onset: função de detecção baseada na variação da energia espectral entre quadros, com pico seguido de limiar adaptativo.
  - Tracking de notas: associa onsets e offsets (fins de nota) para construir notas contínuas, lidando com sobreposição polifônica limitada.
- **Validador:**
  - Casa os eventos detectados por áudio com os eventos MIDI (referência), estabelecendo correspondência ótima por proximidade temporal (< 100 ms, limiar inicial para tolerar jitter de captura/transporte sem perder alinhamento musical) e similaridade de altura.
  - Calcula métricas: precisão, revocação, F1-score por nota; erro absoluto de onset (média e desvio padrão); acurácia de acordes.
- **Construtor da partitura:**
  - Aplica quantização sobre os eventos MIDI (tempo absoluto) ou, no modo apenas áudio, sobre as notas detectadas.
  - Preenche uma representação simbólica da peça (lista de compassos, cada um com figuras e notas).
  - Renderizador: usa VexFlow em WebView ou gera SVG customizado, exibindo partitura final e permitindo exportação (MusicXML, PDF).

## 5. Fluxo de Dados e Interação entre Caminhos

### Cenário de validação (híbrido)

1. O músico toca no teclado; simultaneamente:
   - O áudio é captado pelo ESP32 e transmitido.
   - O MIDI é enviado via USB para o Flutter.
2. O Flutter recebe as duas streams com timestamps sincronizados (sincronia por evento inicial comum, como clique audível ou pressionamento de tecla no início da gravação).
3. O pipeline de áudio gera uma lista de eventos musicais estimados.
4. O módulo MIDI produz a lista de eventos de referência.
5. O validador computa as métricas de desempenho.
6. A partitura é gerada a partir da referência MIDI (partitura “perfeita”) e, opcionalmente, exibe-se sobreposição visual dos erros (notas omitidas, adições incorretas, diferenças de ritmo).
7. Os resultados são apresentados no app: partitura, gráficos de erro e indicadores numéricos.

### Cenário sem MIDI (uso real)

Apenas o ramo de áudio opera, e a partitura é gerada com base nas notas detectadas. A confiança é respaldada pelas métricas previamente obtidas nos testes.

## 6. Considerações de Teste, Desempenho e Escalabilidade

- **Polifonia máxima:** o sistema de áudio tem desempenho garantido para até 4 notas simultâneas. Para peças mais densas, pode-se adicionar um modelo baseado em NMF (Non-negative Matrix Factorization), mas isso aumentaria significativamente a carga computacional. O MIDI naturalmente não sofre essa limitação.
- **Latência de análise:** todo o processamento por quadro deve manter-se abaixo do tempo real (~32 ms por quadro), viável com FFI e código C++ otimizado.
- **Validação robusta:** o protocolo experimental inclui peças monofônicas e polifônicas de dificuldade progressiva, medindo a degradação do algoritmo com o aumento da complexidade, sempre usando o MIDI como referência absoluta.

## 7. Conclusão

Esta arquitetura separa claramente as responsabilidades, maximiza o valor científico ao incorporar um padrão ouro de validação e fornece os artefatos esperados de uma aplicação profissional de transcrição musical. O design é modular, testável e apto a evoluir — desde um protótipo de TCC até um produto funcional. Cada decisão foi fundamentada em requisitos objetivos de desempenho, viabilidade de implementação e rigor acadêmico.
