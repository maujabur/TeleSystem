# Componentes De Sistema

Este grupo contem infraestrutura local do firmware: versao, OTA, energia,
bateria e LED.

## Componentes

### `components/tele_system`

Inclui:

- `firmware_ota.c`: upload OTA, OTA por URL e OTA por manifest;
- `firmware_version.h`: versao normalizada do app;
- `vbat_monitor.c`: medicao e politica de bateria;
- `power_good.c`: controle de alimentacao de perifericos.

### `components/status_led`

Driver/stub de LED de status. O `connectivity_controller` atualiza o LED de
acordo com o estado Wi-Fi.

## Versao De Firmware

`firmware_version.h` define:

```c
#define APP_VERSION_SEMVER "0.3.31"
#define APP_VERSION_LABEL "TeleSystem portal OTA adapter"
#define APP_BUILD_ID "0.3.31-local"
#define APP_VERSION_STRING APP_VERSION_SEMVER " " APP_VERSION_LABEL
```

Use `APP_VERSION_SEMVER` para comparacao de update. Use
`APP_VERSION_STRING` para logs, MQTT e UI.

## OTA Local E Remoto

APIs principais:

```c
esp_err_t firmware_ota_init(void);
esp_err_t firmware_ota_start(const char *url);
esp_err_t firmware_ota_start_manifest(const firmware_ota_manifest_config_t *config);
esp_err_t firmware_ota_upload_begin(void);
esp_err_t firmware_ota_upload_write(const uint8_t *data, size_t data_len);
esp_err_t firmware_ota_upload_finalize(void);
void firmware_ota_upload_abort(void);
void firmware_ota_get_status(firmware_ota_status_t *status);
```

Uso:

- portal usa `firmware_ota_upload_*`;
- comandos MQTT usam `firmware_ota_check_manifest()` e
  `firmware_ota_start_manifest()`;
- rotina principal futura pode chamar as mesmas APIs depois de Wi-Fi pronto.

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

## Inicializacao Atual

Trecho de alto nivel:

```c
tele_portal_logs_init();
ESP_ERROR_CHECK(vbat_monitor_init());
ESP_ERROR_CHECK(power_good_init());
ESP_ERROR_CHECK(tele_ca_store_init());
ESP_ERROR_CHECK(firmware_ota_init());
ESP_ERROR_CHECK(register_portal_ota_routes());
ESP_ERROR_CHECK(connectivity_controller_start());
ESP_ERROR_CHECK(mqtt_presence_start());
```

## Como Adicionar Rotina De Sistema

1. Mantenha regra de negocio em componente de dominio.
2. Inicialize o componente em `main/main.c`.
3. Se depender de rede, rode em task propria depois de
   `connectivity_controller_start()` e `wifi_manager_wait_until_ready()`.
4. Exponha status via `tele_status` quando for util para portal/MQTT.
