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
v1/telecafezinho/{device_id}/cmd/in
v1/telecafezinho/{device_id}/cmd/out
```

## Publicacoes automaticas

### availability

Payload retido para online/offline. O mesmo topico e usado como LWT.

```json
{
  "device_id": "TeleCafezinho-5112D0",
  "fw": "0.3.4 TeleCafezinho retained seen",
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
  "fw": "0.3.4 TeleCafezinho retained seen",
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
  "fw": "0.3.4 TeleCafezinho retained seen",
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
  "fw": "0.3.4 TeleCafezinho retained seen",
  "session_id": "20260609T120000Z-5112D0",
  "ts": "2026-06-09T12:01:00Z",
  "uptime_s": 60,
  "rssi": -40,
  "heap_free": 8195536,
  "vbat_mv": 0,
  "wifi_state": "sta_connected"
}
```

### event

Eventos discretos de firmware.

```json
{
  "device_id": "TeleCafezinho-5112D0",
  "fw": "0.3.4 TeleCafezinho retained seen",
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
  "fw": "0.3.4 TeleCafezinho retained seen",
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

### get_settings

Retorna `device_connectivity` e `mqtt`.

```json
{"cmd_id":"s1","name":"get_settings"}
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

### set_settings

Atualiza configuracoes de conectividade do dispositivo.

```json
{
  "cmd_id": "s2",
  "name": "set_settings",
  "args": {
    "device_connectivity": {
      "provisioning_ssid": "TeleCafezinho",
      "sta_max_retry": 5,
      "apsta_policy": 1,
      "apsta_grace_period_s": 900
    }
  }
}
```

Regras:

- `apsta_policy` e `apsta_grace_period_s` devem ser enviados juntos;
- `sta_max_retry` aceita a mesma faixa validada pela API HTTP;
- comandos mutaveis usam deduplicacao por `cmd_id`.

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
