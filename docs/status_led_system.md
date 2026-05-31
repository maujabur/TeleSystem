# Sistema de LED de status

Este documento descreve o subsistema de LED de status do firmware.

O LED usado e um WS28xx conectado por padrao ao IO48. O objetivo do modulo e
mostrar o estado do produto com cores e padroes de piscada, sem misturar essa
apresentacao com a logica de Wi-Fi, portal web ou ACRCloud.

## Arquivos principais

- `main/connectivity/status_led.c`
- `main/connectivity/status_led.h`
- `main/connectivity/status_led_stub.c`
- `main/connectivity/connectivity_controller.c`
- `main/app/acr_orchestrator.c`
- `main/app/acr_trigger_output.c`
- `main/app/acr_client.c`
- `main/app/acr_client.h`
- `main/main.c`
- `main/Kconfig.projbuild`

## Responsabilidade

`status_led` controla apenas o LED fisico:

- inicializa o canal RMT;
- codifica o protocolo WS28xx;
- aplica brilho global;
- converte cores logicas RGB para a ordem fisica do chip;
- executa cores fixas e padroes de piscada;
- expoe uma API simples baseada em estados.

Ele nao deve:

- conhecer credenciais Wi-Fi;
- chamar APIs `esp_wifi_*`;
- conhecer HTTP, ACRCloud ou JSON;
- decidir politica de conectividade;
- decidir politica de aplicacao.

Os outros modulos publicam ou interpretam estados, e o LED apenas apresenta o
estado que recebe.

## API publica

```c
typedef enum {
    STATUS_LED_STATE_BOOT = 0,
    STATUS_LED_STATE_WIFI_CONNECTING,
    STATUS_LED_STATE_WIFI_PROVISIONING,
    STATUS_LED_STATE_WIFI_CONNECTED,
    STATUS_LED_STATE_ACR_UPLOADING,
    STATUS_LED_STATE_ACR_WAITING_RESULT,
    STATUS_LED_STATE_ACR_RESULT_HUMAN,
    STATUS_LED_STATE_ACR_RESULT_AI,
    STATUS_LED_STATE_BT_NEXT_ACTIVE,
    STATUS_LED_STATE_ERROR,
} status_led_state_t;

esp_err_t status_led_start(void);
status_led_state_t status_led_get_state(void);
esp_err_t status_led_set_state(status_led_state_t state);
esp_err_t status_led_set_capture_overlay(bool enabled);
```

`status_led_start()` e chamado uma vez durante a inicializacao do
`connectivity_controller`.

`status_led_set_state()` pode ser chamado por modulos de orquestracao para
alterar a apresentacao do LED.

`status_led_set_capture_overlay(true)` habilita um pulso curto adicional
(por padrao branco) sobre o estado atual do LED sem trocar o estado base.

`status_led_set_capture_overlay(false)` desabilita esse pulso adicional.

## Estados padrao

| Estado | Cor padrao | Padrao |
|---|---:|---|
| Boot / inicializacao | `0x202020` branco fraco | pisca curto e lento |
| Wi-Fi conectando | `0x0040FF` azul | pisca rapido |
| Wi-Fi em configuracao / provisioning AP | `0xFF9000` ambar | pisca medio |
| Wi-Fi conectado | `0x00B050` verde | fixo |
| ACR enviando arquivo | `0x8000FF` roxo | pisca rapido |
| ACR aguardando resultado | `0x00C8FF` ciano | pulso lento |
| ACR resultado humano | `0x00FF00` verde puro | fixo |
| ACR resultado IA | `0xFF00A0` magenta | fixo |
| Erro | `0xFF0000` vermelho | pisca rapido |

As cores configuradas no menuconfig sempre usam formato RGB logico:

```text
0xRRGGBB
```

A ordem fisica enviada ao LED e configurada separadamente.

## Ordem de cor do chip

Alguns LEDs WS28xx recebem os canais em ordens diferentes. Por isso, a cor
logica continua sendo RGB, mas a ordem no fio pode ser configurada.

Opcao em menuconfig:

```text
Status LED Config -> Status LED color order
```

Opcoes:

- `RGB`
- `RBG`
- `GRB`
- `GBR`
- `BRG`
- `BGR`

O padrao e `GRB`, que e comum em WS2812/WS28xx e preserva as cores atualmente
esperadas no hardware deste projeto.

RGBW nao e suportado por enquanto, porque exige 4 bytes por pixel e outra
politica de conversao.

## Configuracao por menuconfig

As opcoes ficam em:

```text
Status LED Config
```

Configuracoes principais:

- habilitar/desabilitar LED;
- GPIO do LED, padrao `48`;
- quantidade de LEDs, padrao `1`;
- brilho global, padrao `48` em escala `0..255`;
- ordem fisica de cor;
- resolucao RMT;
- stack e prioridade da task do LED;
- cores por estado;
- tempos de piscada por estado piscante.

Exemplos de opcoes:

```text
STATUS_LED_GPIO
STATUS_LED_COUNT
STATUS_LED_BRIGHTNESS
STATUS_LED_COLOR_ORDER_GRB
STATUS_LED_WIFI_CONNECTED_COLOR
STATUS_LED_ACR_UPLOADING_COLOR
STATUS_LED_ACR_WAITING_RESULT_ON_MS
STATUS_LED_CAPTURE_OVERLAY_COLOR
STATUS_LED_CAPTURE_OVERLAY_ON_MS
STATUS_LED_CAPTURE_OVERLAY_OFF_MS
```

## Fluxo de conectividade

O `wifi_manager` nao conhece o LED.

O fluxo e:

```text
wifi_manager -> publica WIFI_MANAGER_EVENT
connectivity_controller -> consulta status Wi-Fi
connectivity_controller -> chama status_led_set_state(...)
status_led -> atualiza LED
```

Mapeamento atual:

| Estado Wi-Fi | Estado do LED |
|---|---|
| `WIFI_MANAGER_STATE_INIT` | `STATUS_LED_STATE_BOOT` |
| `WIFI_MANAGER_STATE_STA_CONNECTING` | `STATUS_LED_STATE_WIFI_CONNECTING` |
| `WIFI_MANAGER_STATE_PROVISIONING_AP` | `STATUS_LED_STATE_WIFI_PROVISIONING` |
| `WIFI_MANAGER_STATE_STA_CONNECTED` | `STATUS_LED_STATE_WIFI_CONNECTED` |

Isso mantem a politica de apresentacao no `connectivity_controller`, nao no
`wifi_manager`.

## Fluxo ACRCloud

O `acr_client` tambem nao conhece o LED.

Ele publica eventos de dominio:

```c
ACR_CLIENT_EVENT_UPLOAD_STARTED
ACR_CLIENT_EVENT_WAITING_RESULT
ACR_CLIENT_EVENT_RESULT_HUMAN
ACR_CLIENT_EVENT_RESULT_AI
ACR_CLIENT_EVENT_FAILED
```

`acr_orchestrator` registra um handler para `ACR_CLIENT_EVENT` e traduz esses
eventos para estados do LED.

Mapeamento atual:

| Evento ACR | Estado do LED |
|---|---|
| `ACR_CLIENT_EVENT_UPLOAD_STARTED` | `STATUS_LED_STATE_ACR_UPLOADING` |
| `ACR_CLIENT_EVENT_WAITING_RESULT` | `STATUS_LED_STATE_ACR_WAITING_RESULT` |
| `ACR_CLIENT_EVENT_RESULT_HUMAN` | `STATUS_LED_STATE_ACR_RESULT_HUMAN` |
| `ACR_CLIENT_EVENT_RESULT_AI` | `STATUS_LED_STATE_ACR_RESULT_AI` |
| `ACR_CLIENT_EVENT_FAILED` | `STATUS_LED_STATE_ERROR` |

Durante `ACR_RUNTIME_STATE_CAPTURING`, o orquestrador habilita o overlay de
captura no LED. Assim, o LED continua mostrando o estado base anterior
(resultado humano/IA ou qualquer outro estado ativo) com pequenas piscadas
de captura sobrepostas.

## Prioridade visual

Hoje o LED mostra o ultimo estado base recebido.

Excecao controlada: durante captura de audio, o overlay de captura injeta
pulsos curtos sem perder o estado base.

Na pratica:

1. Durante boot e conexao, os eventos Wi-Fi comandam o LED.
2. Depois que a rede esta pronta, o fluxo ACR assume o LED.
3. Resultado humano/IA permanece fixo ate outro estado ser definido ou o
   dispositivo reiniciar.

Se no futuro houver multiplas tarefas simultaneas, pode ser necessario criar
uma politica explicita de prioridade, por exemplo:

- erro critico sempre vence;
- ACR em andamento vence Wi-Fi conectado;
- modo de configuracao Wi-Fi vence aplicacao;
- resultado final permanece por tempo configuravel.

## Overlay de captura

Objetivo: sinalizar captura de audio sem apagar o contexto visual anterior.

Comportamento padrao:

- estado base continua ativo;
- overlay aplica pulso curto de cor dedicada;
- duty cycle baixo para nao ofuscar resultado humano/IA.

Opcoes em menuconfig:

- `STATUS_LED_CAPTURE_OVERLAY_COLOR` (padrao `0xFFFFFF`);
- `STATUS_LED_CAPTURE_OVERLAY_ON_MS` (padrao `40`);
- `STATUS_LED_CAPTURE_OVERLAY_OFF_MS` (padrao `760`).

## Observacoes de hardware

- O protocolo WS28xx e gerado por RMT.
- A resolucao RMT padrao e `10 MHz`.
- Os tempos de bit sao calculados a partir da resolucao configurada.
- O buffer usa 3 bytes por LED.
- A ordem padrao no fio e `GRB`.

## Como testar

1. Flashar o firmware.
2. Observar o boot: branco fraco piscando.
3. Observar Wi-Fi conectando: azul piscando rapido.
4. Forcar modo de configuracao com o botao no boot: ambar piscando.
5. Aguardar conexao Wi-Fi: verde fixo.
6. Deixar o fluxo ACR rodar:
   - durante captura, validar pequenas piscadas de overlay (branco por padrao) sem perder a cor base anterior;
   - roxo piscando durante upload;
   - ciano pulsando enquanto aguarda resultado;
   - verde fixo para resultado humano;
   - magenta fixo para resultado IA;
   - vermelho piscando em falha.
7. Acionar teste de BT_NEXT e confirmar estado temporario dedicado sem perder o estado anterior ao final do pulso.

Se as cores parecerem trocadas, ajustar `Status LED color order` no menuconfig.
