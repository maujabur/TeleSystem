# Manual de Operacao MQTT

## Objetivo

Este documento descreve o uso do canal MQTT implementado no firmware para:

- identificar dispositivos online/offline;
- ler estado e configuracoes remotamente;
- alterar configuracoes equivalentes a pagina `/settings`;
- executar reboot remoto com confirmacao.

O broker atual usado no piloto e o HiveMQ Cloud.

## Identificacao do dispositivo

O `device_id` publicado no MQTT segue o formato:

- `{ACR_UPLOAD_PREFIX}-{ultimos_3_bytes_do_mac}`

Exemplo:

- `skips_999-5112D0`

Cada boot tambem gera um `session_id` estavel durante toda a execucao atual.

Formato preferencial atual:

- `{timestamp_utc_basico}-{mac_tail}`

Exemplo:

- `20260602T183000Z-5112D0`

## Namespace de topicos

Prefixo base:

- `v1/acr/{device_id}`

Topicos usados:

- `v1/acr/{device_id}/status`
- `v1/acr/{device_id}/state`
- `v1/acr/{device_id}/heartbeat`
- `v1/acr/{device_id}/event`
- `v1/acr/{device_id}/cmd/in`
- `v1/acr/{device_id}/cmd/out`

## Publicacoes automaticas

### status

Usado para online/offline.

Exemplo:

```json
{"device_id":"skips_999-5112D0","fw":"0.6.0 mqtt fw session_id","session_id":"20260602T015644Z-5112D0","status":"online","ts":"2026-06-02T01:56:44Z","reason":"mqtt_connected"}
```

O broker tambem pode publicar `offline` via LWT quando a conexao cair de forma abrupta.

### state

Snapshot rapido do estado da conectividade.

Exemplo:

```json
{"device_id":"skips_999-5112D0","fw":"0.6.0 mqtt fw session_id","session_id":"20260602T015644Z-5112D0","ts":"2026-06-02T01:56:44Z","wifi_state":"sta_connected","wifi_ready":true,"ssid":"Mau","ip":"192.168.15.14","rssi":-39}
```

### heartbeat

Publicacao periodica de saude operacional.

Exemplo:

```json
{"device_id":"skips_999-5112D0","fw":"0.6.0 mqtt fw session_id","session_id":"20260602T015644Z-5112D0","ts":"2026-06-02T02:12:13Z","uptime_s":61,"rssi":-40,"heap_free":8195536,"vbat_mv":0,"wifi_state":"sta_connected"}
```

### event

Eventos discretos do firmware.

Exemplo:

```json
{"device_id":"skips_999-5112D0","fw":"0.6.0 mqtt fw session_id","session_id":"20260602T015644Z-5112D0","event":"boot","message":"mqtt_online","ts":"2026-06-02T01:56:44Z"}
```

## Canal de comando

Comandos entram em:

- `v1/acr/{device_id}/cmd/in`

Respostas saem em:

- `v1/acr/{device_id}/cmd/out`

Todos os comandos devem incluir `cmd_id` unico.

Formato base:

```json
{
  "cmd_id": "c1",
  "name": "ping"
}
```

Formato base da resposta:

```json
{
  "device_id": "skips_999-5112D0",
  "fw": "0.6.0 mqtt fw session_id",
  "session_id": "20260602T015644Z-5112D0",
  "cmd_id": "c1",
  "ok": true,
  "ts": "2026-06-02T02:11:25Z",
  "result": {}
}
```

## Comandos disponiveis

### ping

Verifica se o dispositivo esta processando comandos.

Request:

```json
{"cmd_id":"c1","name":"ping"}
```

Response:

```json
{"device_id":"skips_999-5112D0","fw":"0.6.0 mqtt fw session_id","session_id":"20260602T015644Z-5112D0","cmd_id":"c1","ok":true,"ts":"2026-06-02T02:11:25Z","result":{"pong":true,"uptime_s":13,"time_synchronized":true}}
```

### get_state

Le estado operacional rapido.

Request:

```json
{"cmd_id":"c2","name":"get_state"}
```

### get_settings

Le as configuracoes equivalentes a pagina `/settings`.

Request:

```json
{"cmd_id":"s1","name":"get_settings"}
```

Blocos retornados em `result`:

- `acr_control`
- `trigger_output`
- `acr_cloud`
- `device_connectivity`
- `mqtt`

### set_heartbeat_interval

Altera o heartbeat em runtime.

Request:

```json
{"cmd_id":"c3","name":"set_heartbeat_interval","args":{"heartbeat_interval_s":30}}
```

Observacao:

- no estado atual, esse valor vale ate reboot;
- para persistencia, e necessario evoluir a implementacao para NVS.

### set_settings

Atualiza configuracoes do dispositivo equivalentes a pagina `/settings`.

Request de exemplo:

```json
{
  "cmd_id":"s2",
  "name":"set_settings",
  "args":{
    "acr_control":{
      "automatic_enabled":true,
      "automatic_interval_ms":60000,
      "capture_duration_seconds":8,
      "digital_gain":3.5,
      "silence_threshold_rms":170,
      "silence_hysteresis_rms":10,
      "min_active_ms":2000,
      "trigger_mode":2,
      "ai_probability_threshold":80
    },
    "device_connectivity":{
      "sta_max_retry":5,
      "apsta_policy":1,
      "apsta_grace_period_s":900
    },
    "trigger_output":{
      "enabled":true,
      "gpio":2,
      "active_level":1,
      "pulse_ms":2200
    },
    "acr_cloud":{
      "region":"us-west-2",
      "container_id":"30317",
      "upload_prefix":"skips_999"
    }
  }
}
```

Regras importantes:

- `apsta_policy` e `apsta_grace_period_s` devem ser enviados juntos;
- `bearer_token` pode ser enviado dentro de `acr_cloud`, mas e dado sensivel;
- validacoes de faixa seguem a mesma politica da UI web atual.

### apply_and_reboot

Agenda reboot curto apos ACK do comando.

Request:

```json
{"cmd_id":"rb1","name":"apply_and_reboot"}
```

Ou com atraso customizado:

```json
{"cmd_id":"rb2","name":"apply_and_reboot","args":{"delay_ms":1500}}
```

## Deduplicacao de comandos

Comandos mutaveis usam deduplicacao em RAM por `cmd_id`.

Cobertura atual:

- `set_heartbeat_interval`
- `set_settings`
- `apply_and_reboot`

Quando o mesmo `cmd_id` chega novamente durante a janela mantida em memoria, o
firmware nao reaplica o comando e responde:

```json
{
  "device_id": "skips_999-5112D0",
  "fw": "0.6.0 mqtt fw session_id",
  "session_id": "20260602T015644Z-5112D0",
  "cmd_id": "s2",
  "ok": true,
  "ts": "2026-06-02T02:20:00Z",
  "result": {
    "duplicate": true,
    "executed": false
  }
}
```

Limitacao atual:

- a deduplicacao nao persiste entre boots.

## Recomendacoes operacionais

1. Usar `cmd_id` sempre unico.
2. Tratar `cmd/out` como fonte de verdade do resultado da operacao.
3. Aplicar ACL por dispositivo no broker.
4. Evitar publicar o mesmo comando em paralelo por mais de um cliente.
5. Para mudancas sensiveis, usar `set_settings` seguido de `apply_and_reboot`.

## Fluxo recomendado para alteracao remota

1. `get_settings`
2. `set_settings`
3. Validar `cmd/out`
4. `apply_and_reboot`
5. Aguardar `status=online` e depois chamar `get_settings` novamente

## Painel Android

Apps recomendados:

- `IoT MQTT Panel` para painel operacional;
- `MQTT Analyzer` para diagnostico bruto.

Subscriptions uteis:

- `v1/acr/+/status`
- `v1/acr/+/state`
- `v1/acr/+/heartbeat`
- `v1/acr/+/event`
- `v1/acr/+/cmd/out`