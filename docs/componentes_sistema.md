# Componentes De Sistema

Este grupo contem a infraestrutura local do firmware: versao, OTA de firmware,
energia, bateria e indicacao visual. A arquitetura atual separa tres papeis:

- componentes de dominio, como `tele_system`, `tele_signal` e
  `tele_indicator`;
- drivers/adapters fisicos, como `status_led`;
- adapters da aplicacao, como `main/app_indicators.c` e
  `main/connectivity`.

## Componentes

### `components/tele_system`

Inclui:

- `firmware_ota.c`: upload OTA, OTA por URL e OTA por manifest;
- `firmware_version.h`: versao normalizada do app;
- `vbat_monitor.c`: medicao e politica de bateria;
- `power_good.c`: controle de alimentacao de perifericos.

`firmware_ota.c` tambem registra o handler de artefato `firmware` em
`tele_artifacts`. Por isso, o componente depende de `tele_manifest` e
`tele_artifacts`, alem das APIs ESP-IDF de OTA.

### `components/tele_signal`

Define tipos compartilhados de sinal fisico: `tele_signal_color_t`,
`tele_signal_effect_t`, IDs de efeito (`off`, `solid`, `blink`, `alternate`,
`breath`, `heartbeat`, `pulse`) e targets.

### `components/status_led`

Driver/stub de LED WS28xx. Ele nao conhece estados semanticos de aplicacao.
Recebe apenas `tele_signal_effect_t`:

```c
tele_signal_effect_t effect = {
    .id = TELE_SIGNAL_EFFECT_BLINK,
    .color_a = {.red = 0x00, .green = 0x40, .blue = 0xFF},
    .time_a_ms = 250,
    .time_b_ms = 750,
    .brightness = 80,
    .target_mask = TELE_SIGNAL_TARGET_ALL,
};
ESP_ERROR_CHECK(status_led_apply_effect(&effect));
```

Quando `CONFIG_STATUS_LED_ENABLED` esta desligado, o CMake compila
`status_led_stub.c`; a API publica continua disponivel para o restante do
firmware.

### `components/tele_indicator`

Registry generico de indicadores logicos. Registra outputs, fontes e eventos.
Eventos ativos sao arbitrados por prioridade e recencia. Eventos com
`duration_ms > 0` sao limpos automaticamente.

Exemplo de uso pela aplicacao:

```c
tele_indicator_raise("wifi", "wifi.connecting");
tele_indicator_raise("battery", "battery.low");
tele_indicator_clear_source("wifi");
```

Manual completo: [tele_indicator.md](tele_indicator.md).

### `main/app_indicators.c`

Adapter da aplicacao entre o registry `tele_indicator` e o driver
`status_led`. Ele:

- inicia `status_led` e `tele_indicator`;
- registra o output fisico padrao;
- registra fontes logicas (`system`, `wifi`, `product`, `battery`, `output`);
- registra eventos semanticos como `wifi.connecting`,
  `wifi.provisioning`, `battery.low` e `system.error`;
- emite `system.boot` no startup.

O componente `tele_indicator` continua generico. A semantica visual do produto
fica neste adapter da aplicacao.

## Versao De Firmware

`firmware_version.h` define:

```c
#define APP_VERSION_SEMVER "0.5.03"
#define APP_VERSION_LABEL "WiFi system refactor"
#define APP_BUILD_ID APP_VERSION_SEMVER "-local"
#define APP_VERSION_STRING APP_VERSION_SEMVER " " APP_VERSION_LABEL
```

Use `APP_VERSION_SEMVER` para comparacao de update. Use
`APP_VERSION_STRING` para logs, MQTT e UI.

## OTA Local E Remoto

APIs principais:

```c
esp_err_t firmware_ota_init(void);
esp_err_t firmware_ota_register_artifact(void);
esp_err_t firmware_ota_start(const char *url);
esp_err_t firmware_ota_upload_begin(void);
esp_err_t firmware_ota_upload_write(const uint8_t *data, size_t data_len);
esp_err_t firmware_ota_upload_finalize(void);
void firmware_ota_upload_abort(void);
void firmware_ota_get_status(firmware_ota_status_t *status);
```

Uso:

- portal usa `firmware_ota_upload_*` por meio do binding
  `tele_firmware_portal_ota`;
- `firmware_ota_register_artifact()` registra `artifact_type = "firmware"` no
  registry generico `tele_artifacts`;
- comandos MQTT/portal usam `artifact/check` e `artifact/apply`;
- rotina principal futura pode chamar `tele_artifacts_check()` e
  `tele_artifacts_apply()` depois de Wi-Fi pronto.

O caminho de firmware por manifest roda em modo streaming, grava diretamente na
particao OTA e responde ACK rapidamente quando chamado por comando generico. O
estado detalhado continua em `firmware_ota_get_status()`.

## Status OTA

`firmware_ota_status_t` inclui:

- estado e flags: `state`, `in_progress`, `restart_pending`;
- versoes: `current_version`, `target_version`, `build_id`;
- origem: `url`, `manifest_url`, `artifact_url`;
- erro: `last_error`;
- particoes: `running_partition`, `next_update_partition`;
- progresso: `bytes_written`, `total_size`, `progress_pct`.

## Energia E Bateria

`vbat_monitor` mede VBAT e pode acionar shutdown por bateria baixa.

`power_good` controla uma GPIO para ligar/desligar perifericos externos. No
boot, `main/main.c`:

1. inicializa VBAT;
2. mede bateria se configurado;
3. inicializa power-good;
4. entra em deep sleep se a bateria estiver critica;
5. liga perifericos quando seguro.

O snapshot de configuracao de boot loga VBAT, shutdown por bateria baixa,
`power_good` e status LED. Campos tecnicos equivalentes podem ser expostos por
`tele_system_registry` quando MQTT/presenca estiver habilitado.

## Inicializacao Atual

Trecho de alto nivel de `main/main.c`:

```c
tele_portal_logs_init();
ESP_ERROR_CHECK(vbat_monitor_init());
vbat_monitor_maybe_measure(VBAT_MONITOR_MOMENT_BOOT);
ESP_ERROR_CHECK(power_good_init());
power_good_set(true);

ESP_ERROR_CHECK(nvs_flash_init());
ESP_ERROR_CHECK(tele_ca_store_init());
ESP_ERROR_CHECK(firmware_ota_init());
ESP_ERROR_CHECK(tele_ca_updater_register_artifact());
ESP_ERROR_CHECK(firmware_ota_register_artifact());
ESP_ERROR_CHECK(tele_artifacts_register_commands());
ESP_ERROR_CHECK(tele_portal_core_register_routes(tele_portal_commands_register_routes));
ESP_ERROR_CHECK(connectivity_controller_start());
maybe_start_ca_updater_boot_task();
ESP_ERROR_CHECK(tele_presence_start());
```

Detalhes importantes:

- se VBAT estiver abaixo do limite configurado, o app desliga `power_good` e
  entra em deep sleep antes de inicializar NVS;
- `tele_ca_store_init()` e `tele_ca_updater_register_artifact()` pertencem ao
  grupo de manifest/artefatos, mas aparecem aqui porque fazem parte do boot
  real do produto;
- `tele_artifacts_register_commands()` registra comandos genericos de artefato
  no dispatcher `tele_commands`;
- as rotas de upload OTA sao registradas pelo agregador `tele_portal`, usando
  `tele_firmware_portal_ota` para conectar o portal ao servico
  `firmware_ota`;
- `maybe_start_ca_updater_boot_task()` so cria task quando
  `CONFIG_TELE_CA_UPDATER_BOOT_ENABLED` esta ativo e ha manifest configurado;
- `tele_presence_start()` retorna imediatamente quando
  `CONFIG_MQTT_PRESENCE_ENABLED` esta desligado.

## Relacao Com Portal, MQTT E Status

- Portal local: `tele_portal_ota` e transporte HTTP; `tele_firmware_portal_ota`
  e o binding; `firmware_ota` e dono da escrita OTA e do status.
- Comandos: `artifact/check`, `artifact/apply`, `artifact/status` e
  `artifacts/get` vivem em `tele_artifacts` e podem ser chamados por portal,
  MQTT ou outro transporte que use `tele_commands`.
- Status tecnico: `tele_system_registry` registra campos de Wi-Fi, VBAT,
  uptime, heap, tempo, power-good e OTA quando `tele_presence_start()` ativa a
  presenca MQTT.
- Indicacao visual: `main/connectivity` traduz eventos de Wi-Fi em eventos do
  `tele_indicator`; `main/app_indicators.c` traduz esses eventos em efeitos de
  LED.

## Como Adicionar Rotina De Sistema

1. Mantenha regra de negocio em componente de dominio.
2. Inicialize o componente em `main/main.c`.
3. Se depender de rede, rode em task propria depois de
   `connectivity_controller_start()` e `wifi_manager_wait_until_ready()`.
4. Se expuser acao remota, prefira `tele_commands` em vez de rota HTTP ou topico
   MQTT exclusivo.
5. Exponha status via `tele_status` quando for util para portal/MQTT.
6. Se precisar de indicador fisico, registre evento em `main/app_indicators.c`
   e emita pelo `tele_indicator`; nao chame `status_led` diretamente a partir
   do dominio.
