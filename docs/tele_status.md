# tele_status

`components/tele_status` centraliza campos observaveis de runtime. Ele e o
par read-only do `tele_config`: nao grava NVS, nao aplica valores e nao conhece
defaults de menuconfig. O componente apenas registra metadados e callbacks de
leitura.

## Objetivo

- evitar duplicacao de campos de status entre MQTT, web e futuras ferramentas;
- permitir que projetos diferentes registrem campos proprios sem alterar o
  nucleo MQTT;
- preparar a publicacao futura de `meta/status` para o Control Center.

## Modelo

Cada campo tem:

- `id`: nome estavel usado em JSON e, futuramente, em manifests;
- `type`: `bool`, `i32`, `u32` ou `string`;
- `unit`: unidade opcional, como `dBm`, `mV`, `bytes` ou `s`;
- `flags`: indica onde o campo deve aparecer, como `STATE`, `HEARTBEAT`,
  `TECHNICAL`, `MQTT`, `WEB` ou `SENSITIVE`;
- callback de leitura.

## Uso atual

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

`state` e `heartbeat` sao montados a partir do registry. O firmware tambem
publica `meta/status` retained com os metadados dos campos expostos por MQTT,
incluindo tipo, unidade opcional e flags.

## Fora de escopo por enquanto

- renderizacao dinamica no Control Center;
- campos aninhados complexos, como o status tecnico completo de VBAT;
- persistencia ou configuracao remota, que pertencem ao `tele_config`.
