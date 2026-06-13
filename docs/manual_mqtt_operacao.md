# Manual de Operacao MQTT

## Objetivo

O firmware publica presenca, estado e telemetria via MQTT e aceita comandos
remotos simples. A implementacao reutilizavel vive em `components/tele_mqtt`; o
adaptador `components/tele_presence/mqtt_presence.c` injeta os dados atuais do
TeleCafezinho.

## Identificacao do dispositivo

O `device_id` segue o formato:

```text
{CONFIG_MQTT_DEVICE_ID_PREFIX}-{ultimos_3_bytes_do_mac}
```

Com os defaults atuais:

```text
TeleCafezinho-5112D0
```

Cada boot tambem gera um `session_id`, usado para diferenciar conexoes da mesma
placa ao longo do tempo.

## Namespace de topicos

Prefixo base:

```text
{CONFIG_MQTT_TOPIC_NAMESPACE}/{device_id}
```

Default atual:

```text
v1/telecafezinho/{device_id}
```

Topicos usados:

```text
v1/telecafezinho/{device_id}/availability
v1/telecafezinho/{device_id}/seen
v1/telecafezinho/{device_id}/state
v1/telecafezinho/{device_id}/heartbeat
v1/telecafezinho/{device_id}/event
v1/telecafezinho/{device_id}/meta/config
v1/telecafezinho/{device_id}/meta/status
v1/telecafezinho/{device_id}/meta/commands
v1/telecafezinho/{device_id}/cmd/in
v1/telecafezinho/{device_id}/cmd/out
```

## Publicacoes automaticas

### availability

Payload retido para online/offline. O mesmo topico e usado como LWT.

```json
{
  "device_id": "TeleCafezinho-5112D0",
  "fw": "0.3.13 TeleCafezinho manifest groups",
  "session_id": "20260609T120000Z-5112D0",
  "status": "online",
  "reason": "mqtt_connected",
  "ts": "2026-06-09T12:00:00Z"
}
```

### seen

Ultimo contato conhecido, retido no broker. Este topico permite que o Control
Center reinicie e ainda saiba quando o dispositivo foi visto pela ultima vez,
sem gravar nada em NVS/flash.

```json
{
  "device_id": "TeleCafezinho-5112D0",
  "fw": "0.3.13 TeleCafezinho manifest groups",
  "session_id": "20260609T120000Z-5112D0",
  "ts": "2026-06-09T12:01:00Z",
  "last_seen_ts": "2026-06-09T12:01:00Z",
  "reason": "heartbeat"
}
```

### state

Snapshot retido com conectividade, bateria e dados tecnicos curtos.

```json
{
  "device_id": "TeleCafezinho-5112D0",
  "fw": "0.3.13 TeleCafezinho manifest groups",
  "session_id": "20260609T120000Z-5112D0",
  "ts": "2026-06-09T12:00:00Z",
  "wifi_state": "sta_connected",
  "wifi_ready": true,
  "ssid": "Mau",
  "ip": "192.168.15.14",
  "rssi": -39,
  "vbat_mv": 0,
  "heap_free": 8195536,
  "uptime_s": 61,
  "heartbeat_interval_s": 60,
  "time_synchronized": true
}
```

### heartbeat

Telemetria periodica, sem retenção.

```json
{
  "device_id": "TeleCafezinho-5112D0",
  "fw": "0.3.13 TeleCafezinho manifest groups",
  "session_id": "20260609T120000Z-5112D0",
  "ts": "2026-06-09T12:01:00Z",
  "uptime_s": 60,
  "rssi": -40,
  "heap_free": 8195536,
  "vbat_mv": 0,
  "wifi_state": "sta_connected"
}
```

### meta/status

Manifesto retido dos campos de status conhecidos pelo firmware. O Control
Center usa `group`, `label` e `description` para organizar a exibicao e mostrar
ajuda por hover.

```json
{
  "device_id": "TeleCafezinho-5112D0",
  "fw": "0.3.13 TeleCafezinho manifest groups",
  "session_id": "20260609T120000Z-5112D0",
  "ts": "2026-06-09T12:00:00Z",
  "registry_revision": 1,
  "fields": [
    {
      "id": "rssi",
      "label": "RSSI",
      "description": "Intensidade do sinal Wi-Fi.",
      "group": "network",
      "type": "i32",
      "unit": "dBm",
      "flags": [
        {"flag": "state"},
        {"flag": "heartbeat"},
        {"flag": "mqtt"}
      ]
    }
  ]
}
```

### meta/config

Manifesto retido dos campos configuraveis conhecidos pelo firmware. Nesta
primeira fase, o payload fica em topico unico e descreve id, tipo, origem,
default, valor efetivo, limites e flags de cada campo exposto por MQTT.

```json
{
  "device_id": "TeleCafezinho-5112D0",
  "fw": "0.3.13 TeleCafezinho manifest groups",
  "session_id": "20260609T120000Z-5112D0",
  "ts": "2026-06-09T12:00:00Z",
  "registry_revision": 1,
  "fields": [
    {
      "id": "wifi.sta_max_retry",
      "type": "u32",
      "source": "default",
      "default": 3,
      "value": 3,
      "min": 1,
      "max": 20,
      "flags": [
        {"flag": "web"},
        {"flag": "mqtt"},
        {"flag": "runtime_apply"}
      ]
    }
  ]
}
```

### meta/commands

Manifesto retido dos comandos remotos conhecidos pelo firmware. O Control
Center usa este payload para descobrir comandos, argumentos e se o comando e
mutavel ou relacionado a reboot.

```json
{
  "device_id": "TeleCafezinho-5112D0",
  "fw": "0.3.13 TeleCafezinho manifest groups",
  "session_id": "20260609T120000Z-5112D0",
  "ts": "2026-06-09T12:00:00Z",
  "registry_revision": 1,
  "commands": [
    {
      "name": "config/set",
      "label": "Salvar configuracao",
      "description": "Atualiza um campo configuravel exposto por MQTT.",
      "group": "config",
      "mutating": true,
      "reboot_required": false,
      "internal": false,
      "args": [
        {"id": "id", "type": "string", "required": true, "min_len": 1, "max_len": 48},
        {"id": "value", "type": "any", "required": true}
      ]
    }
  ]
}
```

### event

Eventos discretos de firmware.

```json
{
  "device_id": "TeleCafezinho-5112D0",
  "fw": "0.3.13 TeleCafezinho manifest groups",
  "session_id": "20260609T120000Z-5112D0",
  "event": "boot",
  "message": "mqtt_online",
  "ts": "2026-06-09T12:00:00Z"
}
```

## Comandos

Comandos entram em:

```text
v1/telecafezinho/{device_id}/cmd/in
```

Respostas saem em:

```text
v1/telecafezinho/{device_id}/cmd/out
```

Formato base:

```json
{
  "cmd_id": "c1",
  "name": "ping"
}
```

Resposta base:

```json
{
  "device_id": "TeleCafezinho-5112D0",
  "fw": "0.3.13 TeleCafezinho manifest groups",
  "session_id": "20260609T120000Z-5112D0",
  "cmd_id": "c1",
  "ok": true,
  "ts": "2026-06-09T12:00:10Z",
  "result": {}
}
```

## Comandos disponiveis

### ping

```json
{"cmd_id":"c1","name":"ping"}
```

### get_state

```json
{"cmd_id":"c2","name":"get_state"}
```

### config/get

Retorna o manifesto de configuracao equivalente ao payload retido de
`meta/config`.

```json
{"cmd_id":"cfg-get-1","name":"config/get"}
```

### commands/get

Retorna o manifesto de comandos equivalente ao payload retido de
`meta/commands`.

```json
{"cmd_id":"cmds-get-1","name":"commands/get"}
```

### get_technical_status

Retorna uptime, sincronismo de tempo, heap, power-good e VBAT.

```json
{"cmd_id":"t1","name":"get_technical_status"}
```

### set_heartbeat_interval

Altera o intervalo de heartbeat em runtime.

```json
{"cmd_id":"h1","name":"set_heartbeat_interval","args":{"heartbeat_interval_s":30}}
```

O valor aceito fica entre 15 e 3600 segundos e nao e persistido em NVS.

### config/set

Atualiza um campo configuravel exposto por `meta/config`.

```json
{
  "cmd_id": "cfg1",
  "name": "config/set",
  "args": {
    "id": "wifi.sta_max_retry",
    "value": 5
  }
}
```

Resposta bem-sucedida:

```json
{
  "cmd_id": "cfg1",
  "ok": true,
  "result": {
    "id": "wifi.sta_max_retry",
    "stored": true,
    "applied": true,
    "requires_reboot": false
  }
}
```

Regras:

- `id` deve existir no registry e ter flag `mqtt`;
- `value` deve combinar com o tipo declarado em `meta/config`;
- cada comando atualiza um campo por vez;
- todos os valores passam por `tele_config_update_value()`;
- campos com `runtime_apply` sao aplicados por callback opcional antes de persistir;
- campos com `reboot_required` sao persistidos como override e entram em vigor no proximo boot ou apos comando de reboot;
- apos sucesso, o firmware republica `meta/config` retido;
- comandos mutaveis usam deduplicacao por `cmd_id`.

### config/reset

Remove o override NVS de um campo e volta ao default efetivo. Se o campo tiver
callback `runtime_apply`, o default tambem e aplicado em runtime.

```json
{
  "cmd_id": "cfg-reset-1",
  "name": "config/reset",
  "args": {
    "id": "wifi.sta_max_retry"
  }
}
```

Resposta bem-sucedida:

```json
{
  "cmd_id": "cfg-reset-1",
  "ok": true,
  "result": {
    "id": "wifi.sta_max_retry",
    "stored": false,
    "applied": true,
    "requires_reboot": false
  }
}
```

Depois de sucesso, o firmware republica `meta/config` retido.

### apply_and_reboot

Agenda reboot curto depois do ACK.

```json
{"cmd_id":"r1","name":"apply_and_reboot","args":{"delay_ms":800}}
```

## Extensao por produto

Novos comandos de dominio devem ser adicionados no adaptador
`components/tele_presence/mqtt_presence.c`, usando os callbacks:

- `is_mutating_command`, para declarar comandos que alteram estado antes da
  execucao;
- `handle_command`, para executar o comando e devolver `result`.

O componente `tele_mqtt` permanece responsavel por topicos, conexao, JSON,
ACK/NACK e deduplicacao.
