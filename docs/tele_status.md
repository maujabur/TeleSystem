# tele_status

Guia do grupo: [componentes_mqtt_config_status_commands.md](componentes_mqtt_config_status_commands.md).

`components/tele_status` centraliza campos observaveis de runtime. Ele e o
par read-only do `tele_config`: nao grava NVS, nao aplica valores e nao conhece
defaults de menuconfig. O componente apenas registra metadados e callbacks de
leitura.

## Objetivo

- evitar duplicacao de campos de status entre MQTT, web e futuras ferramentas;
- permitir que projetos diferentes registrem campos proprios sem alterar o
  nucleo MQTT;
- publicar `meta/status` para ferramentas dinamicas como o Control Center.

## Modelo de campo

Cada campo tem:

- `id`: nome estavel usado em JSON e, futuramente, em manifests;
- `label`: nome curto para exibicao;
- `description`: texto de ajuda para ferramentas e hover;
- `group`: agrupamento livre definido pelo projeto, como `network`, `runtime`,
  `power`, `memory` ou grupos de dominio;
- `type`: `bool`, `i32`, `u32` ou `string`;
- `unit`: unidade opcional, como `dBm`, `mV`, `bytes` ou `s`;
- `flags`: indica onde o campo deve aparecer, como `STATE`, `HEARTBEAT`,
  `TECHNICAL`, `MQTT`, `WEB` ou `SENSITIVE`;
- callback de leitura.

O callback e chamado no momento em que o payload e montado. Por isso ele deve
ser rapido, nao bloquear e devolver valor seguro mesmo quando o subsistema
observado ainda nao estiver inicializado.

## Registro

Projetos registram campos chamando `tele_status_register_fields()` com uma
tabela estatica de `tele_status_field_t`. O componente nao copia strings, entao
`id`, `label`, `description`, `group` e `unit` devem permanecer validos durante
a execucao.

Exemplo reduzido:

```c
static const tele_status_field_t s_status_fields[] = {
    {
        .id = "rssi",
        .label = "RSSI",
        .description = "Intensidade do sinal Wi-Fi.",
        .group = "network",
        .type = TELE_STATUS_TYPE_I32,
        .unit = "dBm",
        .flags = TELE_STATUS_FLAG_STATE |
                 TELE_STATUS_FLAG_HEARTBEAT |
                 TELE_STATUS_FLAG_MQTT,
        .read.i32 = read_rssi,
    },
};
```

## Payloads MQTT

`tele_mqtt` usa o registry como fonte default para:

- `{base_topic}/{device_id}/state`, usando campos com flag `STATE`;
- `{base_topic}/{device_id}/heartbeat`, usando campos com flag `HEARTBEAT`;
- `{base_topic}/{device_id}/meta/status`, usando campos com flag `MQTT`.

`state` e retained, pois representa o snapshot operacional mais recente.
`heartbeat` nao e retained, pois e telemetria periodica. `meta/status` e
retained, pois descreve a estrutura que ferramentas externas usam para
renderizar os dados.

## Manifesto MQTT

`tele_status_add_manifest_to_json(root, TELE_STATUS_FLAG_MQTT)` gera o
manifesto de status. Para cada campo, o JSON inclui:

- `id`, `label`, `description`, `group`, `type`;
- `unit`, quando declarada;
- `flags`, como `state`, `heartbeat`, `technical`, `mqtt`, `web` e
  `sensitive`.

O manifesto nao carrega valores. Valores chegam por `state`, `heartbeat`,
`cmd/out` de `get_state` e, quando fizer sentido, `get_technical_status`.
Ferramentas devem cruzar `id` do manifesto com as chaves dos payloads de valor.
Campos com `SENSITIVE` aparecem no manifesto, mas sao omitidos dos payloads de
valor gerados por `tele_status_add_fields_to_json()`.

## Uso no TeleSystem

`components/tele_presence` registra os campos comuns:

- `wifi_state`
- `wifi_ready`
- `ssid`
- `ip`
- `rssi`
- `vbat_mv`
- `heap_free`
- `uptime_s`
- `heartbeat_interval_s`
- `time_synchronized`
- `ota_state`
- `ota_in_progress`
- `ota_progress_pct`
- `ota_target_version`
- `ota_last_error`

`state` e `heartbeat` sao montados a partir do registry. O firmware tambem
publica `meta/status` retained com os metadados dos campos expostos por MQTT,
incluindo label, description, group, tipo, unidade opcional e flags.

O Control Center ja consome esse manifesto para renderizar campos read-only
agrupados e seus valores atuais, usando os payloads `heartbeat`,
`state`, `get_state` e `get_technical_status` como fontes de valor.

## Fora de escopo por enquanto

- edicao de valores, que pertence ao `tele_config`;
- paginacao do manifesto, que so deve entrar quando o payload crescer;
- modelagem completa de campos aninhados complexos, como diagnosticos VBAT;
- persistencia ou configuracao remota, que pertencem ao `tele_config`.
