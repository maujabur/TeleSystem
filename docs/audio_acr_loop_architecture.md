# Arquitetura atual: loop ACR de captura de audio e acionamento de trigger

## Objetivo

Este documento descreve a arquitetura atual do loop autonomo de analise ciclica
de audio no firmware, com notas historicas de evolucao.

Fluxo atual:

```text
espera condicao de rede pronta para operacoes cloud
captura N segundos de audio mono em PCM (memoria)
descarta trechos/sessoes sem audio util
monta WAV em memoria e envia para ACRCloud
aguarda resultado de deteccao IA/humano
se politica de trigger for satisfeita, aciona GPIO de trigger por tempo configuravel
volta ao inicio
```

Valores iniciais recomendados:

- audio mono;
- `16000 Hz`;
- PCM 16-bit;
- WAV como formato de upload;
- captura padrao de `8 s`;
- polling ACR mais rapido que o atual;
- pulso de trigger (BT_NEXT) padrao de `2200 ms`;
- GPIO, nivel ativo e tempo de pulso configuraveis.

## Premissas validadas

- A integracao ACRCloud ja foi validada em outro sistema.
- O audio vem de fonte `line out`, portanto e limpo e com nivel mais previsivel
  que microfone ambiente.
- E toleravel ouvir alguns segundos adicionais antes do `next`.
- O produto nao precisa reagir em tempo real absoluto; precisa reagir de forma
  confiavel apos a classificacao ACR.

Essas premissas simplificam o desenho:

- nao e necessario um VAD sofisticado no primeiro ciclo;
- o descarte de silencio pode ser baseado em energia/RMS simples;
- nao e necessario filtragem pesada ou reducao de ruido;
- o WAV PCM 16-bit e uma escolha adequada para robustez e simplicidade.

## Estado atual do projeto

O firmware ja possui o loop principal implementado:

- `main/main.c`
  - inicializa NVS/rotas/controladores;
  - transfere execucao continua para `acr_orchestrator_run()`.
- `main/app/acr_orchestrator.c`
  - executa loop autonomo;
  - aguarda/sincroniza condicoes de rede;
  - captura audio com `audio_capture_record_pcm_to_buffer`;
  - ativa/desativa overlay de captura no LED;
  - envia para ACRCloud e aplica politica de trigger;
  - agenda retry e tenta recuperacao de Wi-Fi quando necessario.
- `main/app/acr_client.c`
  - envia multipart `audio/wav` (WAV montado em memoria);
  - cria nome unico de upload;
  - consulta resultado com polling e aplica politica de trigger;
  - publica eventos de upload, espera, IA, humano e falha.
- `main/app/acr_trigger_output.c`
  - controla a GPIO de trigger (BT_NEXT no caso de uso atual).
- `main/app/acr_runtime_status.c`
  - guarda estado/telemetria visiveis pela UI.
- `main/connectivity/status_led.c`
  - apresenta estados de Wi-Fi/ACR e suporta overlay visual de captura.

Portanto, as fases de WAV fixo, loop manual e trigger apenas proposto ja foram
superadas pelo estado atual do codigo.

## Viabilidade tecnica

Para `16 kHz`, mono, PCM 16-bit:

```text
16000 amostras/s * 2 bytes * 8 s = 256000 bytes
header WAV = 44 bytes
total aproximado = 250 KiB
```

Mesmo com `15 s`, o arquivo fica perto de `480 KiB`.

A arvore `firmware_assets` continua viavel para artefatos embutidos, mas o fluxo
principal implementado hoje prioriza captura PCM em memoria (PSRAM), montagem
de WAV em memoria e envio direto para ACRCloud, sem depender de arquivo
temporario em disco no caminho principal.

## Arquitetura em camadas

```text
app_main / acr_orchestrator
  |
  +-- audio_capture
  |     |
  |     +-- esp_driver_i2s
  |
  +-- wav_writer
  |     +-- montagem WAV em memoria
  |
  +-- acr_client
  |     |
  |     +-- ACRCloud File Scanning API
  |
  +-- acr_trigger_output
  |     |
  |     +-- esp_driver_gpio
  |
  +-- runtime_status / status_led / web UI
```

## Modulos e responsabilidades atuais

### `audio_capture`

Responsavel por configurar e ler audio via I2S RX.

Responsabilidades:

- configurar sample rate, bits por amostra e canal;
- configurar pinos I2S;
- criar canal RX;
- ler blocos de PCM;
- entregar amostras para o orquestrador;
- esconder detalhes do driver I2S.

API inicial sugerida:

```c
typedef struct {
    int sample_rate_hz;
    int bits_per_sample;
    int bclk_gpio;
    int ws_gpio;
    int data_in_gpio;
} audio_capture_config_t;

esp_err_t audio_capture_start(const audio_capture_config_t *config);
esp_err_t audio_capture_read(void *buffer,
                             size_t buffer_size,
                             size_t *bytes_read,
                             TickType_t timeout_ticks);
esp_err_t audio_capture_stop(void);
```

### `audio_activity_detector`

Responsavel por detectar se ha audio util.

Como a fonte e `line out`, o algoritmo inicial pode ser simples:

- calcular energia/RMS por janela;
- comparar com threshold configuravel;
- contar janelas ativas;
- descartar uma captura se nao houve atividade minima.

Configuracoes iniciais:

- `silence_threshold`;
- `min_active_ms`;
- `max_initial_silence_ms`;
- `trailing_silence_ms`.

No primeiro MVP, a regra pode ser ainda mais simples:

```text
capturar ate N segundos
se menos de X ms ativos, descartar e reiniciar loop
caso contrario, enviar WAV
```

### `wav_writer`

Responsavel por gerar WAV PCM valido.

Responsabilidades:

- montar cabecalho WAV para payload em memoria;
- serializar blocos PCM para upload;
- opcionalmente apoiar gravacao em arquivo apenas para diagnostico;
- manter consistencia de `RIFF size` e `data size` quando houver escrita em arquivo.

API inicial sugerida:

```c
typedef struct {
    int sample_rate_hz;
    int channels;
    int bits_per_sample;
} wav_writer_config_t;

esp_err_t wav_writer_open(const char *path, const wav_writer_config_t *config);
esp_err_t wav_writer_write_pcm(const void *data, size_t size);
esp_err_t wav_writer_close(void);
```

### `acr_client`

Hoje o cliente retorna apenas `esp_err_t` e publica eventos. Para o loop de
produto, ele deve tambem devolver uma decisao estruturada com os campos brutos
da ACRCloud e a decisao calculada pela politica de disparo.

API recomendada:

```c
typedef enum {
    ACR_DECISION_UNKNOWN = 0,
    ACR_DECISION_HUMAN,
    ACR_DECISION_AI,
} acr_decision_t;

typedef struct {
    acr_decision_t decision;
    bool trigger;
    double ai_probability;
    char prediction[32];
    char uploaded_name[128];
} acr_client_result_t;

esp_err_t acr_client_submit_and_wait_result(const acr_config_t *config,
                                            const char *path,
                                            acr_client_result_t *out);
```

Os eventos existentes devem continuar existindo para status LED e UI.

### Politica de disparo IA

O sistema Python atual usa uma politica configuravel que deve ser replicada no
firmware.

Configuracao equivalente:

```json
{
  "trigger_mode": "prediction_only",
  "ai_prediction_values": ["ai", "ai-generated", "ai_generated", "aigc", "generated"],
  "ai_probability_threshold": 80.0
}
```

Modos suportados:

- `prediction_only`: dispara se `prediction` estiver na lista de valores IA;
- `probability_only`: dispara se `ai_probability` for maior ou igual ao
  threshold;
- `prediction_or_probability`: dispara se qualquer um dos criterios for
  verdadeiro;
- `prediction_and_probability`: dispara apenas se os dois criterios forem
  verdadeiros.

Regra recomendada para o MVP:

```text
trigger_mode = prediction_only
ai_prediction_values = ai, ai-generated, ai_generated, aigc, generated
ai_probability_threshold = 80.0
```

No MVP atual, `prediction_only` e a politica padrao ativa. O threshold de
probabilidade continua configuravel e armazenado para uso quando o modo for
alterado para `probability_only`, `prediction_or_probability` ou
`prediction_and_probability`.

Normalizacao recomendada para `prediction`:

```text
trim
lowercase ASCII
comparacao exata contra a lista configurada
```

Com isso, `ACR_DECISION_AI` significa que a politica de disparo foi satisfeita,
nao apenas que a string bruta era exatamente `ai_generated`.

Resultado recomendado:

```text
politica satisfeita -> trigger = true, decision = ACR_DECISION_AI
politica nao satisfeita com resultado valido -> trigger = false, decision = ACR_DECISION_HUMAN
erro, timeout ou resposta incompleta -> erro/retry
```

O campo `trigger` e propositalmente redundante com `decision == ACR_DECISION_AI`.
Ele deixa explicito que a saida fisica deve obedecer a politica de disparo, e
nao apenas ao parse bruto do resultado ACR.

### `acr_trigger_output` (BT_NEXT)

Responsavel por acionar a GPIO que simula o comando `BT_NEXT`.

Responsabilidades:

- configurar GPIO como saida;
- manter nivel inativo no boot;
- aplicar pulso com nivel ativo configuravel;
- retornar ao nivel inativo apos o tempo configurado;
- aplicar o pulso de forma bloqueante dentro do ciclo do orquestrador.

Decisao de produto: se a musica atual for classificada como IA, o sistema deve
acionar `BT_NEXT` e so depois iniciar o proximo ciclo. Isso evita sobreposicao
entre o comando fisico e uma nova captura, e torna explicito que a acao desejada
e avancar para a proxima musica antes de continuar analisando.

API inicial sugerida:

```c
typedef struct {
    bool enabled;
    int gpio;
    int active_level;
    int pulse_ms;
} acr_trigger_output_config_t;

esp_err_t acr_trigger_output_init(void);
esp_err_t acr_trigger_output_pulse(void);
```

Configuracao padrao:

```text
enabled = true
active_level = 1
pulse_ms = 2200
```

O GPIO padrao deve ser decidido com base no hardware final.

### `acr_orchestrator`

O loop autonomo esta separado em `main/app/acr_orchestrator.c`. `main.c` fica
responsavel pela inicializacao e entrega o fluxo continuo ao orquestrador.

Responsabilidades:

- esperar Wi-Fi pronto;
- carregar configuracoes;
- capturar audio;
- decidir se a captura tem audio util;
- enviar para ACR;
- interpretar decisao;
- acionar `acr_trigger_output` quando necessario;
- atualizar `acr_runtime_status`;
- respeitar retries e solicitacoes manuais da UI.
- solicitar recuperacao de Wi-Fi apos erros consecutivos de ACR/rede.

Loop conceitual:

```text
while true:
    wait_until_wifi_ready()
    load_config()

    set_status("Capturando audio")
  capture_pcm_to_buffer(capture_seconds)

    if capture_is_silent:
        set_status("Silencio descartado")
        short_delay()
        continue

    set_status("Enviando para ACRCloud")
    result = acr_client_submit_and_wait_result(...)

    if result == AI:
        set_status("Resultado ACR: IA; acionando next")
        acr_trigger_output_pulse()
    else if result == HUMAN:
        set_status("Resultado ACR: humano")
    else:
        schedule_retry()
        if consecutive_network_errors >= 3:
            reconnect_sta()

    loop_delay_or_continue()
```

## Polling ACR

O polling atual e:

```text
CONFIG_ACR_POLL_MAX_ATTEMPTS = 10
CONFIG_ACR_POLL_DELAY_MS = 2000
```

Isso permite ate aproximadamente `20 s` de espera apos upload.

Como e toleravel ouvir alguns segundos adicionais, mas o produto fica melhor
com resposta mais rapida, recomendacao inicial:

```text
CONFIG_ACR_POLL_MAX_ATTEMPTS = 20
CONFIG_ACR_POLL_DELAY_MS = 1000
```

Alternativa mais agressiva:

```text
CONFIG_ACR_POLL_MAX_ATTEMPTS = 30
CONFIG_ACR_POLL_DELAY_MS = 750
```

Recomendacao para MVP:

- usar `1000 ms` por simplicidade;
- manter timeout total perto de `20 s`;
- evitar polling muito rapido antes de medir comportamento real da API.

## Configuracoes

### Menuconfig

Parametros de fabrica e fallback devem entrar em `main/Kconfig.projbuild`.

Sugestoes:

```text
Audio Capture Config
  AUDIO_CAPTURE_ENABLED
  AUDIO_CAPTURE_SAMPLE_RATE_HZ = 16000
  AUDIO_CAPTURE_SECONDS = 8
  AUDIO_CAPTURE_BITS_PER_SAMPLE = 16
  AUDIO_CAPTURE_I2S_BCLK_GPIO
  AUDIO_CAPTURE_I2S_WS_GPIO
  AUDIO_CAPTURE_I2S_DATA_IN_GPIO
  AUDIO_CAPTURE_SILENCE_THRESHOLD
  AUDIO_CAPTURE_MIN_ACTIVE_MS

BT Next Output Config
  BT_NEXT_OUTPUT_ENABLED
  BT_NEXT_OUTPUT_GPIO
  BT_NEXT_OUTPUT_ACTIVE_LEVEL
  BT_NEXT_OUTPUT_PULSE_MS = 2200

ACR Trigger Config
  ACR_TRIGGER_MODE = prediction_only
  ACR_TRIGGER_AI_PREDICTION_VALUES = "ai,ai-generated,ai_generated,aigc,generated"
  ACR_TRIGGER_AI_PROBABILITY_THRESHOLD = 80.0
```

### NVS

Configuracoes que podem mudar em campo devem ser persistidas em NVS.

Sugestoes:

- duracao da captura;
- threshold de silencio;
- GPIO BT_NEXT;
- nivel ativo;
- tempo de pulso;
- habilitar/desabilitar acionamento automatico.
- modo de disparo ACR;
- lista de valores IA para `prediction`;
- threshold de `ai_probability`.

### Interface web

A UI deve evoluir de ferramenta de teste para painel de operacao.

Durante a fase de validacao, a tela principal concentra Wi-Fi, status, restart
e o botao manual `Analisar Audio`. Para o produto final, o objetivo nao e o
usuario disparar analises manualmente. O loop autonomo deve ser o dono do
fluxo de captura, upload, decisao e acionamento.

Por isso, a tela principal deve mostrar o estado do sistema e nao parecer uma
bancada de testes.

Conteudo recomendado para a tela principal:

- estado Wi-Fi e rede atual;
- estado do loop ACR/audio;
- ultima captura: tamanho, atividade/RMS e se houve descarte de silencio;
- ultimo resultado ACR: `prediction`, `ai_probability`, `trigger` e arquivo
  enviado;
- estado BT_NEXT: GPIO, nivel ativo e ultimo pulso;
- versao do firmware.

Acoes recomendadas na tela principal:

- conectar Wi-Fi apenas quando o dispositivo estiver em modo provisionamento;
- pausar/retomar deteccao somente se isso for necessario em campo;
- evitar comandos tecnicos destrutivos ou de bancada na primeira tela.

Acoes que devem sair da tela principal no produto final:

- `Analisar Audio`;
- `Restart`;
- scan/lista de redes quando o dispositivo ja esta conectado e operando.

Essas acoes podem continuar existindo em paginas tecnicas ou de diagnostico,
mas nao devem ser o caminho normal de uso.

### Pagina tecnica de audio

A configuracao de audio pode evoluir em uma pagina tecnica separada, para nao
sobrecarregar o portal principal.

Pagina sugerida:

```text
/audio-settings
```

Campos:

- captura habilitada;
- segundos de captura;
- threshold de silencio;
- GPIO I2S BCLK;
- GPIO I2S WS/LRCLK;
- GPIO I2S DATA IN;
- GPIO BT_NEXT;
- nivel ativo;
- tempo de pulso;
- modo de disparo;
- valores IA de `prediction`;
- threshold de probabilidade IA;
- teste manual de pulso BT_NEXT;
- teste manual de captura.

### Paginas tecnicas existentes

As paginas atuais devem ser classificadas como manutencao/diagnostico:

- `/api/acr`: configuracao ACRCloud, token, container e prefixo de upload;
- `/api/acr/control`: modos e controle tecnico do ciclo ACR;
- `/device-config`: configuracoes de fabrica, como SSID de provisionamento;
- `/logs`: logs e diagnostico;
- futura `/audio-settings`: audio, trigger ACR e BT_NEXT.

Essas paginas podem continuar acessiveis por URL conhecida ou modo tecnico, mas
nao precisam estar em destaque na tela principal.

### Rotas e comandos de teste

A rota atual `POST /api/acr/run` nasceu para facilitar testes com upload ACR.
No produto final, ela nao deve iniciar um fluxo paralelo de captura/upload. O
dono do ciclo deve ser uma unica task de orquestracao.

Comportamentos aceitaveis para essa rota no produto final:

- remover da tela principal e manter apenas como endpoint tecnico;
- renomear conceitualmente para `force_cycle`;
- aceitar o pedido apenas quando o orquestrador estiver ocioso;
- acordar retry ou antecipar o proximo ciclo sem interromper uma captura/upload
  em andamento.

Comportamentos que devem ser evitados:

- iniciar upload concorrente;
- reenviar arquivo antigo sem deixar isso claro;
- interromper captura atual sem politica explicita;
- disputar estado com o loop automatico.

Comandos como `POST /api/restart`, `POST /api/acr`, `POST /api/device/config`
e testes de BT_NEXT tambem devem ser tratados como comandos tecnicos, nao como
acoes de operacao normal.

## Estados de runtime

O status atual deve ser expandido para ajudar debug e operacao.

Campos uteis:

- `audio_status`;
- `audio_last_capture_size`;
- `audio_last_active_ms`;
- `audio_last_rms`;
- `acr_status`;
- `acr_uploaded_name`;
- `acr_last_prediction`;
- `acr_last_ai_probability`;
- `bt_next_last_pulse_ms`;
- `bt_next_last_error`;
- `acr_consecutive_errors`;
- historico recente em RAM, se util para diagnostico durante validacao.

O historico persistente em flash nao e necessario para o MVP. O historico em
RAM pode continuar existindo como ferramenta de diagnostico, mas deve ser
tratado como opcional e revisado futuramente se nao trouxer valor operacional.

Estados textuais recomendados:

```text
Aguardando Wi-Fi
Capturando audio
Silencio descartado
Enviando arquivo para ACRCloud
Aguardando resultado da ACRCloud
Resultado ACR: humano
Resultado ACR: IA
Acionando BT_NEXT
Falha no envio ou consulta ACR
Aguardando nova tentativa
```

## Plano historico de implementacao (referencia)

Status: as fases abaixo nasceram como plano evolutivo e varias delas ja estao
implementadas no firmware atual (orquestrador, captura em memoria, trigger
GPIO, polling, telemetria e integracao com portal).

### Fase 1: resultado ACR estruturado

Objetivo: permitir que o firmware tome decisao programatica a partir do
resultado ACR.

Tarefas:

- adicionar `acr_decision_t` e `acr_client_result_t`;
- adicionar `acr_trigger_mode_t` e configuracao de politica de disparo;
- criar `acr_client_submit_and_wait_result(...)`;
- manter `acr_client_submit_and_wait(...)` como wrapper, se conveniente;
- preencher `trigger`, `decision`, `prediction`, `ai_probability` e
  `uploaded_name`;
- normalizar `prediction` antes de comparar;
- suportar os modos `prediction_only`, `probability_only`,
  `prediction_or_probability` e `prediction_and_probability`;
- usar defaults equivalentes ao sistema Python atual;
- preservar eventos existentes para LED/UI;
- validar usando fixture de audio de referencia em `firmware_assets/teste.wav`.

Criterio de pronto:

- o firmware consegue distinguir disparo/nada por retorno de funcao;
- a decisao segue `prediction_only` por default, usando a lista de valores IA;
- logs e LED continuam funcionando como antes.

### Fase 2: GPIO BT_NEXT (implementada como `acr_trigger_output`)

Objetivo: acionar uma saida fisica quando o resultado for IA.

Tarefas:

- criar `main/app/acr_trigger_output.c`;
- criar `main/app/acr_trigger_output.h`;
- adicionar opcoes em `Kconfig.projbuild`;
- inicializar saida no boot;
- chamar pulso quando `result.trigger == true`;
- adicionar rota tecnica opcional para teste manual de pulso.

Criterio de pronto:

- com WAV fixo que satisfaz prediction e probabilidade, a GPIO pulsa por
  `2200 ms`;
- com WAV que nao satisfaz a politica de disparo, a GPIO nao pulsa;
- nivel inativo fica correto apos boot e apos pulso.

### Fase 3: WAV writer

Objetivo: gerar arquivos WAV validos no proprio ESP32.

Tarefas:

- criar `wav_writer`;
- escrever header PCM 16-bit mono;
- corrigir tamanhos no fechamento;
- adicionar teste simples gerando silencio ou tom sintetico, se util;
- validar tamanho e header do WAV gerado no buffer (ou em arquivo de debug, quando habilitado).

Criterio de pronto:

- WAV gerado e reconhecido como WAV PCM valido;
- ACR aceita upload do arquivo gerado.

### Fase 4: captura I2S

Objetivo: capturar audio real do ADC/I2S.

Tarefas:

- adicionar dependencia de driver I2S no `main/CMakeLists.txt`, se necessario;
- criar `audio_capture`;
- configurar I2S RX para `16 kHz`, mono, 16-bit;
- gravar blocos recebidos com `wav_writer`;
- limitar captura por tempo configuravel;
- medir tamanho final do arquivo.

Criterio de pronto:

- arquivo WAV capturado da fonte line out toca corretamente fora do ESP32;
- tamanho bate com a duracao configurada;
- ACR processa o arquivo capturado.

### Fase 5: descarte de silencio

Objetivo: evitar upload quando nao ha audio util.

Tarefas:

- criar detector RMS simples;
- medir atividade durante a captura;
- descartar arquivo quando atividade total for menor que `min_active_ms`;
- registrar `active_ms` e RMS em runtime status;
- ajustar threshold com amostras reais da fonte line out.

Criterio de pronto:

- silencio ou ausencia de sinal nao gera upload;
- musica em nivel normal gera upload;
- falsos descartes sao raros com threshold default.

### Fase 6: loop autonomo

Objetivo: substituir o envio manual do arquivo fixo pelo ciclo continuo.

Tarefas:

- alterar `main.c` ou criar `acr_orchestrator`;
- loopar captura -> upload -> decisao -> acao;
- transformar `/api/acr/run` em comando tecnico de `force_cycle` ou remover da
  tela principal;
- garantir que qualquer pedido manual apenas acorde o orquestrador quando for
  seguro;
- respeitar retry em falhas de rede/ACR;
- evitar reentrancia durante captura/upload.

Criterio de pronto:

- apos Wi-Fi pronto, o sistema opera continuamente;
- resultado IA aciona BT_NEXT;
- resultado humano apenas volta ao loop;
- falhas entram em retry sem travar o produto.

### Fase 7: configuracao web

Objetivo: permitir ajuste em campo sem recompilar.

Tarefas:

- criar store NVS para configuracoes de audio e BT_NEXT;
- criar rotas REST;
- criar pagina `/audio-settings`;
- transformar a home em painel de operacao do loop autonomo;
- esconder formulario Wi-Fi quando o dispositivo estiver conectado e operando;
- mover `Analisar Audio`, `Restart` e testes manuais para paginas tecnicas;
- adicionar teste manual de pulso;
- mostrar ultimos valores de captura e resultado.

Criterio de pronto:

- usuario tecnico ajusta captura e saida pela UI;
- usuario final ve estado do loop, nao ferramentas de teste;
- configuracoes persistem apos reboot;
- defaults continuam vindo de menuconfig.

## Riscos e mitigacoes

### Latencia de classificacao

Risco:

- ACR pode demorar alguns segundos para processar.

Mitigacao:

- polling default de `1000 ms`;
- timeout total toleravel;
- status claro na UI/LED;
- evitar capturas longas demais.

### Nivel de audio variavel

Risco:

- threshold de silencio pode ficar alto ou baixo demais.

Mitigacao:

- como a fonte e line out, usar threshold simples inicialmente;
- expor threshold pela UI;
- registrar RMS/atividade recentes para calibracao.

### GPIO incorreta ou nivel invertido

Risco:

- BT_NEXT pode acionar no boot ou nao acionar.

Mitigacao:

- configurar nivel inativo imediatamente no boot;
- nivel ativo configuravel;
- teste manual pela UI;
- evitar GPIOs de boot strap ou pinos usados por flash/PSRAM/LED.

### Arquivo WAV corrompido

Risco:

- queda ou erro antes de corrigir header.

Mitigacao:

- usar arquivo temporario fixo;
- sempre fechar/corrigir header antes de upload;
- validar tamanho minimo;
- logar erro e descartar captura invalida.

### Concorrencia com portal/status

Risco:

- chamadas manuais e loop automatico competirem.

Mitigacao:

- manter uma unica task/orquestrador dona do ciclo;
- `/api/acr/run` ou `force_cycle` apenas sinaliza pedido;
- aceitar pedido manual somente em estados seguros;
- nao iniciar upload concorrente.

### Exposicao de endpoints tecnicos

Risco:

- em modo STA normal, rotas de escrita como restart, configuracao ACR,
  configuracao de dispositivo, force cycle e testes de GPIO ficarem acessiveis
  na rede local sem protecao.

Mitigacao:

- manter portal completo apenas em modo provisionamento/manutencao;
- em modo normal, expor no maximo status e logs, se necessario;
- bloquear comandos de escrita fora do modo tecnico;
- considerar modo tecnico ativado por botao fisico no boot;
- avaliar autenticacao simples se o portal precisar ficar disponivel em campo.

## Decisoes de referencia para o MVP

- usar WAV PCM 16-bit mono (arquivo ou montagem em memoria);
- capturar `8 s` por ciclo;
- permitir captura em memoria (PSRAM) com montagem de WAV para upload;
- descartar silencio por energia simples;
- mudar polling para `1000 ms`, `20` tentativas;
- acionar BT_NEXT quando a politica de disparo ACR for satisfeita;
- usar `prediction_only` por default no MVP atual;
- aceitar como valores IA: `ai`, `ai-generated`, `ai_generated`, `aigc` e
  `generated`;
- manter `ai_probability >= 80.0` como threshold configuravel para os modos que
  usam probabilidade;
- tratar a tela principal como painel de operacao, nao ferramenta de teste;
- remover `Analisar Audio` e `Restart` da home final;
- manter comandos de teste e configuracao em paginas tecnicas ou modo
  manutencao;
- manter trigger GPIO integrado ao orquestrador conforme politica de disparo.

## Sequencia pratica historica

```text
1. ACR retorna decisao estruturada
2. GPIO BT_NEXT funcionando com WAV fixo
3. WAV writer local validado
4. captura I2S gravando WAV
5. descarte simples de silencio
6. loop autonomo
7. pagina tecnica de configuracao
```

Essa ordem permite validar o comportamento de produto antes de introduzir a
complexidade da captura de audio.
