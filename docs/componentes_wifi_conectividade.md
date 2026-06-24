# Componentes Wi-Fi E Conectividade

Este grupo cuida de rede local, provisionamento e eventos de conectividade.

## Componentes

### `components/tele_wifi`

Responsavel por:

- inicializar stack Wi-Fi;
- operar STA, AP de provisionamento ou AP+STA;
- salvar e listar redes conhecidas;
- aplicar politica AP+STA;
- expor status de Wi-Fi;
- iniciar reconexao;
- oferecer `wifi_manager_wait_until_ready()`.

APIs principais:

```c
esp_err_t wifi_manager_start_with_config(const wifi_manager_config_t *config);
esp_err_t wifi_manager_wait_until_ready(TickType_t timeout_ticks);
esp_err_t wifi_manager_get_status(wifi_manager_status_t *status);
esp_err_t wifi_manager_apply_wifi_credentials(const char *ssid, const char *password);
```

### `components/tele_wifi/device_config_store.c`

Registra configuracoes Wi-Fi no `tele_config`:

- `wifi.provisioning_ssid`;
- `wifi.sta_max_retry`;
- `wifi.apsta_policy`;
- `wifi.apsta_grace_period_s`.

Tambem mantem wrappers antigos para pontos do codigo que ainda leem
configuracao pelo store.

### `main/connectivity`

Adaptador de aplicacao. Responsavel por:

- montar `wifi_manager_config_t`;
- iniciar o Wi-Fi;
- sincronizar portal e LED de status com estado Wi-Fi;
- acionar sincronismo de tempo quando a STA conecta;
- lidar com botao de boot para forcar provisionamento.

Ponto de entrada:

```c
ESP_ERROR_CHECK(connectivity_controller_start());
```

## Estados De Wi-Fi

`wifi_manager_state_t`:

- `WIFI_MANAGER_STATE_INIT`;
- `WIFI_MANAGER_STATE_STA_CONNECTING`;
- `WIFI_MANAGER_STATE_STA_CONNECTED`;
- `WIFI_MANAGER_STATE_PROVISIONING_AP`.

Use `wifi_manager_get_status()` para ler estado sem depender dos eventos
internos.

## Esperar Rede Pronta

Rotinas de aplicacao que dependem de internet, como updates por manifest,
devem esperar Wi-Fi pronto fora dos componentes de dominio:

```c
esp_err_t err = wifi_manager_wait_until_ready(pdMS_TO_TICKS(60000));
if (err == ESP_OK) {
    // rede pronta
}
```

Se retornar `ESP_ERR_WIFI_NOT_CONNECT`, o dispositivo entrou em AP de
provisionamento. Se retornar `ESP_ERR_TIMEOUT`, a rede nao ficou pronta dentro
do prazo.

## Provisionamento

Quando nao ha credencial valida, o Wi-Fi inicia AP de provisionamento. O portal
usa as rotas de `tele_portal_wifi` para salvar novas credenciais e pedir
reconexao.

## Sincronismo De Tempo

`main/connectivity` chama o modulo de time sync quando recebe evento de STA
conectada. O MQTT usa isso por meio de `mqtt_presence_ready()`, que so permite
cliente MQTT quando Wi-Fi e horario estao prontos.

## Como Integrar Uma Rotina De Rede No App

1. Inicialize NVS e componentes de base.
2. Chame `connectivity_controller_start()`.
3. Inicie uma task propria.
4. Dentro da task, chame `wifi_manager_wait_until_ready()`.
5. Execute a rotina e reprograme com intervalo/jitter se necessario.

Essa regra vale para checks periodicos de certificado e OTA.
