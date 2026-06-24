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
- deduplicar comandos mutaveis por `cmd_id`.

O nucleo deve continuar sem conhecer regras de dominio do TeleSystem. Produto
integra por `tele_mqtt_config_t`.

Guia de operacao: [manual_mqtt_operacao.md](manual_mqtt_operacao.md).

### `components/tele_presence`

Adaptador do TeleSystem para o nucleo MQTT. Responsavel por:

- registrar campos de config do produto;
- registrar campos de status comuns;
- registrar comandos de update por manifest;
- implementar `handle_command` e `is_mutating_command`;
- esperar Wi-Fi e horario sincronizado antes de iniciar MQTT.

`mqtt_presence_start()` e chamado por `main/main.c` depois de
`connectivity_controller_start()`.

## Fluxo De Inicializacao

```c
ESP_ERROR_CHECK(connectivity_controller_start());
ESP_ERROR_CHECK(mqtt_presence_start());
```

Dentro de `mqtt_presence_start()`:

1. campos de config Wi-Fi/MQTT sao registrados;
2. callbacks runtime de config sao conectados;
3. campos de status sao registrados;
4. comandos de update sao registrados;
5. eventos Wi-Fi passam a tentar iniciar o cliente MQTT;
6. `tele_mqtt_start()` registra comandos builtin e prepara o cliente.

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
2. Se for MQTT, implemente execucao em `handle_command`.
3. Se altera estado, marque `MUTATING` no registry e retorne `true` em
   `is_mutating_command`.
4. Retorne resultado JSON curto; nao bloqueie handlers por downloads grandes.

Para download grande, como OTA de firmware, inicie uma task e responda ACK.

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
```

Payloads e exemplos completos ficam em
[manual_mqtt_operacao.md](manual_mqtt_operacao.md).
