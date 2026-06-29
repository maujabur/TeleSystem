# Componentes MQTT, Config, Status E Commands

Este grupo fornece o contrato remoto generico do firmware:

- configuracoes persistiveis;
- status observaveis;
- comandos executaveis;
- transporte MQTT;
- adaptador TeleSystem.

## Componentes

### `components/tele_config`

Registry de configuracoes. Ele valida valores, persiste overrides em NVS e
chama callbacks de aplicacao runtime quando registrados.

Use quando a operacao representa estado configuravel que pode sobreviver a
reboot, como politica Wi-Fi, SSID de provisionamento ou intervalo de heartbeat.

Guia detalhado: [tele_config.md](tele_config.md).

### `components/tele_status`

Registry read-only de valores observaveis. Ele nao grava NVS e nao aplica
estado; apenas descreve campos e chama callbacks de leitura quando payloads sao
montados.

Use para campos como Wi-Fi pronto, RSSI, heap livre, uptime, VBAT e progresso
OTA.

Guia detalhado: [tele_status.md](tele_status.md).

### `components/tele_commands`

Registry de metadados de comandos. Ele nao executa comandos; descreve nome,
grupo, argumentos e flags para ferramentas remotas.

Use para acoes pontuais, como reboot, check/apply update, diagnostico ou
operacoes sem modelo persistivel.

Guia detalhado: [tele_commands.md](tele_commands.md).

### `components/tele_mqtt`

Nucleo MQTT reutilizavel. Responsavel por:

- montar topicos;
- publicar `availability`, `seen`, `state`, `heartbeat`;
- publicar manifests `meta/config`, `meta/status`, `meta/commands`;
- receber `cmd/in` e publicar `cmd/out`;
- chamar o dispatcher generico de `tele_commands`.

O nucleo deve continuar sem conhecer regras de dominio do TeleSystem. Produto
integra por registries e pelos callbacks de payload de `tele_mqtt_config_t`.

Guia de operacao: [manual_mqtt_operacao.md](manual_mqtt_operacao.md).

### `components/tele_presence`

Bootstrap MQTT do TeleSystem. Responsavel por:

- esperar Wi-Fi e horario sincronizado antes de iniciar MQTT.
- preencher `tele_mqtt_config_t` com Kconfig e callbacks do produto;
- chamar `tele_system_registry_register()`;
- iniciar `tele_mqtt`.

### `components/tele_system_registry`

Bindings de produto para os registries base. Responsavel por:

- registrar campos de config do produto;
- registrar campos de status comuns;
- conectar callbacks runtime de config;
- montar o diagnostico tecnico usado por `get_technical_status`.

`tele_presence_start()` e chamado por `main/main.c` depois de
`connectivity_controller_start()`.

## Fluxo De Inicializacao

```c
ESP_ERROR_CHECK(connectivity_controller_start());
ESP_ERROR_CHECK(tele_presence_start());
```

Dentro de `tele_presence_start()`:

1. `tele_system_registry_register()` registra config/status/callbacks;
2. eventos Wi-Fi passam a tentar iniciar o cliente MQTT;
3. `tele_mqtt_start()` prepara o cliente e registra comandos core via
   `tele_core_commands`.

## Como Adicionar Configuracao

1. Defina um `tele_config_field_t` estatico.
2. Registre com `tele_config_register_fields()`.
3. Se o valor tiver efeito em runtime, registre callback com
   `tele_config_set_apply_handler()`.
4. Exponha por MQTT e/ou web usando flags.

## Como Adicionar Status

1. Crie uma funcao de leitura rapida e nao bloqueante.
2. Registre um `tele_status_field_t`.
3. Escolha flags:
   - `STATE`: aparece no snapshot retido;
   - `HEARTBEAT`: aparece em telemetria periodica;
   - `TECHNICAL`: aparece em diagnostico;
   - `MQTT` e `WEB`: indicam canais de exposicao.

## Como Adicionar Comando

1. Registre metadados com `tele_commands_register()`.
2. Informe `.handler` no `tele_command_t`.
3. Se altera estado, marque `MUTATING` no registry.
4. Retorne resultado JSON curto; nao bloqueie handlers por downloads grandes.

Para download grande, como OTA de firmware, inicie uma task e responda ACK.
MQTT, portal HTTP ou serial podem chamar o mesmo dispatcher
`tele_commands_execute()`; cada transporte decide quais flags aceita.

## Topicos MQTT

Topicos principais:

```text
{base_topic}/{device_id}/availability
{base_topic}/{device_id}/seen
{base_topic}/{device_id}/state
{base_topic}/{device_id}/heartbeat
{base_topic}/{device_id}/event
{base_topic}/{device_id}/meta/config
{base_topic}/{device_id}/meta/status
{base_topic}/{device_id}/meta/commands
{base_topic}/{device_id}/cmd/in
{base_topic}/{device_id}/cmd/out
{base_topic}/_shared/{topic_suffix}
```

O namespace compartilhado e reservado para componentes de dominio que precisam
publicar ou receber topicos fora da arvore `{base_topic}/{device_id}`. O
`topic_suffix` e sempre relativo; quem chama nao deve hardcodar `base_topic` nem
`_shared`.

Shared topics podem ser registrados durante o bootstrap ou em runtime. O
`tele_mqtt` protege a registry interna contra acesso concorrente e chama o
handler registrado fora da secao critica.

Fronteiras:

- `tele_mqtt` cuida de conexao, montagem de topico, publish, subscribe,
  resubscribe e dispatch;
- componentes de dominio cuidam de schema de payload, estado, filtros e regras
  de negocio;
- shared topics nao substituem `tele_config`, `tele_status` nem
  `tele_commands`;
- o contrato de topicos por dispositivo permanece inalterado.

Payloads e exemplos completos ficam em
[manual_mqtt_operacao.md](manual_mqtt_operacao.md).
