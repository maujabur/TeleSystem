# tele_commands

Guia do grupo: [componentes_mqtt_config_status_commands.md](componentes_mqtt_config_status_commands.md).

`components/tele_commands` centraliza o manifesto de comandos remotos
disponiveis no firmware. Ele e o terceiro par do contrato generico:

- `tele_config`: campos editaveis;
- `tele_status`: campos observaveis;
- `tele_commands`: comandos executaveis.

## Modelo

Cada comando registrado tem:

- `name`: nome estavel usado no envelope de comando;
- `label`: nome curto para botao;
- `description`: texto de ajuda para ferramentas e hover;
- `group`: agrupamento livre definido pelo projeto, como `system`, `status`,
  `config` ou grupos de dominio;
- canais de exposicao, como `MQTT`, `WEB`, `SERIAL` e `LORA`;
- flags comportamentais, como `MUTATING`, `REBOOT_REQUIRED` e `INTERNAL`;
- lista opcional de argumentos, com `id`, tipo, obrigatoriedade e limites.
- handler opcional de execucao, independente do transporte.

O componente tambem oferece `tele_commands_execute()`, que recebe um envelope
normalizado (`cmd_id`, `name`, `args`, `required_channel_flags`) e devolve uma resposta
normalizada (`ok`, `error`, `result`). Transportes como MQTT, portal HTTP ou
serial apenas traduzem seu protocolo para esse envelope.

## Registro

Comandos sao registrados com `tele_commands_register()`. Assim como nos outros
registries, strings e arrays devem continuar vivos durante toda a execucao.

Exemplo reduzido:

```c
static const tele_command_arg_t s_reboot_args[] = {
    {
        .id = "delay_ms",
        .type = TELE_COMMAND_ARG_U32,
        .required = false,
        .min.u32 = 100,
        .max.u32 = 10000,
    },
};

static const tele_command_t s_commands[] = {
    {
        .name = "apply_and_reboot",
        .label = "Aplicar e reiniciar",
        .description = "Agenda reboot curto depois do ACK.",
        .group = "system",
        .channel_flags = TELE_CHANNEL_FLAG_MQTT,
        .flags = TELE_COMMAND_FLAG_MUTATING |
                 TELE_COMMAND_FLAG_REBOOT_REQUIRED,
        .args = s_reboot_args,
        .arg_count = 1,
        .handler = handle_apply_and_reboot,
    },
};
```

## Manifesto MQTT

`tele_mqtt` chama `tele_commands_add_manifest_to_json(root,
TELE_CHANNEL_FLAG_MQTT)` para publicar
`{base_topic}/{device_id}/meta/commands`. O manifesto e retained e descreve
comandos visiveis por MQTT.

Para cada comando, o JSON inclui:

- `name`, `label`, `description`, `group`;
- `channels`, como `mqtt`, `web`, `serial` e `lora`;
- `flags`, como `mutating`, `reboot_required` e `internal`;
- `args`, quando existem, com `id`, `type`, `required`, limites numericos e
  limites de string.

`channel_flags` controla quais transportes podem ver/executar o comando.
`flags` guarda apenas comportamento do dispatcher e da UI.

Comandos com flag `INTERNAL` podem aparecer no manifesto para ferramentas que
precisam entender o contrato completo, mas UIs comuns podem oculta-los da lista
principal. `config/set` e `config/reset`, por exemplo, sao usados internamente
pela aba Settings do Control Center.

## Execucao

Qualquer transporte pode executar um comando registrado:

```c
tele_command_response_t response = {0};
const tele_command_request_t request = {
    .cmd_id = "cmd-20260614T120000Z",
    .name = "get_state",
    .args = args_json,
    .required_channel_flags = TELE_CHANNEL_FLAG_MQTT,
};

esp_err_t err = tele_commands_execute(&request, &response);
if (err == ESP_OK) {
    /* serializar response.ok, response.error e response.result */
}
tele_commands_response_cleanup(&response);
```

Quando o comando tem `TELE_COMMAND_FLAG_MUTATING`, o dispatcher aplica
deduplicacao por `cmd_id`. Isso reduz o risco de repetir acoes causadas por
reentrega MQTT, retry HTTP ou retransmissao serial.

No MQTT, comandos chegam em `{base_topic}/{device_id}/cmd/in`:

```json
{
  "name": "get_state",
  "cmd_id": "cmd-20260614T120000Z",
  "args": {}
}
```

Respostas saem em `{base_topic}/{device_id}/cmd/out` e incluem o mesmo
`cmd_id`, sucesso ou erro, timestamp e resultado quando houver.

`tele_mqtt` apenas faz parse do payload, chama `tele_commands_execute()` com
`TELE_CHANNEL_FLAG_MQTT` e publica a resposta em `cmd/out`.

## Command ou setting?

Use `tele_config` quando a operacao altera estado configuravel persistivel,
como intervalo de heartbeat, politica Wi-Fi ou credenciais. Use
`tele_commands` quando a operacao e uma acao pontual, como `ping`,
`get_state`, diagnostico, limpeza, reboot ou start/stop de uma rotina.

Um setting pode ser alterado pelo comando generico `config/set`, mas nao deve
virar um comando proprio apenas para economizar UI. Isso mantem Settings
descobrivel por `meta/config` e evita duplicar semantica.

## Uso no TeleSystem

`components/tele_core_commands` registra os comandos base independentes de
transporte:

- `ping`;
- `get_state`;
- `get_technical_status`;
- `config/get`;
- `commands/get`;
- `config/set`;
- `config/reset`;
- `apply_and_reboot`.

O TeleSystem tambem registra comandos genericos de artefatos por manifest pelo
componente `components/tele_artifacts`:

- `artifacts/get`;
- `artifact/check`;
- `artifact/status`;
- `artifact/apply`.

Todos usam `args.artifact_type`, `args.manifest_url` e aceitam `args.channel`.
`artifact/apply` para `artifact_type = "firmware"` inicia uma task de OTA em
streaming e responde apenas que o processo comecou. `artifact/apply` para
`artifact_type = "ca_bundle"` baixa, valida e ativa o bundle CA pelo storage
runtime. `artifacts/get` lista os tipos registrados e `artifact/status` retorna
estado local e progresso quando o handler implementa status.

`components/tele_mqtt` apenas adapta esses comandos ao transporte MQTT:
recebe `cmd/in`, chama `tele_commands_execute()` com `TELE_CHANNEL_FLAG_MQTT`
e publica `cmd/out`. O firmware publica `{base_topic}/{device_id}/meta/commands`
como mensagem retida ao conectar ao broker. O Control Center consome esse
manifesto para mostrar comandos agrupados, argumentos e ajuda por hover.

## Uso Pelo Portal Ou Serial

O dispatcher ja e usado pelo adapter HTTP `components/tele_portal_commands`.
Ele expoe:

- `GET /api/commands`, usando `TELE_CHANNEL_FLAG_WEB`;
- `POST /api/commands/execute`, chamando `tele_commands_execute()` com
  `TELE_CHANNEL_FLAG_WEB`.

Serial pode seguir o mesmo modelo: chamar `tele_core_commands_register()`, ler
um JSON com `cmd_id`, `name` e `args`, chamar o dispatcher com a flag adequada
e imprimir a resposta normalizada. Upload OTA por arquivo continua fora desse
fluxo, porque e streaming binario.

## Fora De Escopo Por Enquanto

- validacao visual rica e widgets especificos por tipo em todos os comandos;
- paginacao de manifesto;
- autorizacao por comando.
