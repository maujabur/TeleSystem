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

Este componente deve permanecer reaproveitavel por outros firmwares ESP-IDF.
Ele nao registra campos de produto no `tele_config`, nao le botao fisico de
boot e nao inicia sincronismo NTP. Esses comportamentos ficam em componentes
adaptadores ou no controlador de aplicacao.

APIs principais:

```c
esp_err_t wifi_manager_start_with_config(const wifi_manager_config_t *config);
esp_err_t wifi_manager_wait_until_ready(TickType_t timeout_ticks);
esp_err_t wifi_manager_get_status(wifi_manager_status_t *status);
esp_err_t wifi_manager_apply_wifi_credentials(const char *ssid, const char *password);
```

### `components/tele_wifi_device_config`

Registra configuracoes Wi-Fi no `tele_config`:

- `wifi.provisioning_ssid`;
- `wifi.sta_max_retry`;
- `wifi.apsta_policy`;
- `wifi.apsta_grace_period_s`.

O componente nao tem mais wrappers `load/save`. Consumidores devem usar
`tele_config_get_effective()`, `tele_config_update_value()` ou
`tele_config_reset_value()` conforme o caso.

Este componente e um adaptador TeleSystem para `tele_wifi`. Firmwares que nao
usam `tele_config` podem depender apenas de `tele_wifi` e fornecer
`wifi_manager_config_t` por outro caminho.

### `components/tele_boot_config_button`

Le o botao fisico de boot usado pela aplicacao para forcar provisionamento.
Fica separado de `tele_wifi` para que a politica de entrada em modo de
configuracao nao seja imposta ao componente base.

### `components/tele_time_sync`

Cuida de sincronismo NTP e formatacao de horario. A conectividade da aplicacao
aciona `time_sync_on_network_connected()` e
`time_sync_on_network_disconnected()` a partir dos eventos do `wifi_manager`.
Outros firmwares podem trocar ou omitir esse componente sem alterar
`tele_wifi`.

### `main/connectivity`

Adaptador de aplicacao. Responsavel por:

- montar `wifi_manager_config_t`;
- iniciar o Wi-Fi;
- sincronizar portal com estado Wi-Fi;
- publicar estado Wi-Fi no `tele_indicator`;
- acionar sincronismo de tempo quando a STA conecta;
- lidar com botao de boot para forcar provisionamento.

Ponto de entrada:

```c
ESP_ERROR_CHECK(connectivity_controller_start());
```

## Como Usar Em Outro Projeto ESP-IDF Do Zero

Esta secao descreve a integracao minima para reutilizar o Wi-Fi em outro
firmware, sem carregar o restante do TeleSystem.

### 1. Copie Os Componentes Necessarios

Para o caso minimo, copie apenas:

```text
components/tele_wifi
```

Se o novo projeto tambem quiser os adaptadores prontos, copie conforme a
necessidade:

- `components/tele_time_sync`: sincronismo NTP acionado por eventos Wi-Fi;
- `components/tele_boot_config_button`: botao fisico para forcar
  provisionamento;
- `components/tele_wifi_device_config`: ponte com `tele_config`;
- `components/tele_portal_wifi`: rotas HTTP para scan, credenciais e redes
  salvas, caso o projeto tenha servidor HTTP/portal.

`tele_wifi` sozinho nao depende desses adaptadores.

### 2. Declare A Dependencia No Componente Da Aplicacao

No `main/CMakeLists.txt` do novo projeto:

```cmake
idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES nvs_flash esp_event tele_wifi
)
```

Adicione outros componentes apenas quando forem usados diretamente:

```cmake
idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES nvs_flash esp_event tele_wifi tele_time_sync tele_boot_config_button
)
```

### 3. Inicialize NVS E Inicie O Wi-Fi

O `tele_wifi` persiste credenciais em NVS, entao a aplicacao deve inicializar
NVS antes de chamar `wifi_manager_start_with_config()`.

Exemplo minimo:

```c
#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "wifi_manager.h"

static const char *TAG = "app";

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    wifi_manager_config_t wifi_config = {
        .provisioning_ssid = "MeuDispositivo-Setup",
        .force_provisioning = false,
        .sta_max_retry = 3,
        .apsta_policy = WIFI_MANAGER_APSTA_AUTO_TIMEOUT,
        .apsta_grace_period_s = 600,
    };

    ESP_ERROR_CHECK(wifi_manager_start_with_config(&wifi_config));

    err = wifi_manager_wait_until_ready(pdMS_TO_TICKS(60000));
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Wi-Fi conectado");
    } else if (err == ESP_ERR_WIFI_NOT_CONNECT) {
        ESP_LOGW(TAG, "Modo AP de provisionamento ativo");
    } else {
        ESP_LOGW(TAG, "Wi-Fi nao ficou pronto: %s", esp_err_to_name(err));
    }
}
```

Se `provisioning_ssid` for `NULL`, o `tele_wifi` gera um SSID a partir do MAC.
Se nao houver credenciais salvas, o componente sobe em AP de provisionamento.

### 4. Salve Credenciais Pelo Canal Do Seu Projeto

O projeto consumidor decide como recebe SSID e senha: serial, BLE, MQTT, HTTP,
display local ou arquivo de configuracao. Quando tiver os valores, chame:

```c
esp_err_t err = wifi_manager_apply_wifi_credentials(ssid, password);
if (err != ESP_OK) {
    // reporte falha no canal escolhido pelo projeto
}
```

Essa chamada salva as credenciais na NVS e tenta conectar em STA. Se o AP de
provisionamento estiver ativo, o `tele_wifi` pode operar em AP+STA conforme a
politica configurada.

### 5. Reaja Aos Eventos De Wi-Fi

Para integrar outros servicos, registre um handler no event loop padrao. Se o
projeto precisa capturar tambem os eventos iniciais, crie o event loop e
registre o handler antes de chamar `wifi_manager_start_with_config()`:

```c
static void app_wifi_event_handler(void *arg,
                                   esp_event_base_t event_base,
                                   int32_t event_id,
                                   void *event_data)
{
    (void)arg;
    (void)event_base;
    (void)event_data;

    if (event_id == WIFI_MANAGER_EVENT_STA_CONNECTED) {
        // iniciar MQTT, NTP, OTA, monitoramento etc.
    } else if (event_id == WIFI_MANAGER_EVENT_STA_DISCONNECTED ||
               event_id == WIFI_MANAGER_EVENT_PROVISIONING_STARTED) {
        // pausar servicos que dependem de internet
    }
}

ESP_ERROR_CHECK(esp_event_loop_create_default());
ESP_ERROR_CHECK(esp_event_handler_register(WIFI_MANAGER_EVENT,
                                           ESP_EVENT_ANY_ID,
                                           app_wifi_event_handler,
                                           NULL));
```

`wifi_manager_start_with_config()` tambem garante a criacao do event loop
padrao, entao projetos que so consultam estado por `wifi_manager_get_status()`
nao precisam registrar handlers.

Tambem e possivel consultar estado sob demanda:

```c
wifi_manager_status_t status = {0};
if (wifi_manager_get_status(&status) == ESP_OK && status.wifi_ready) {
    // status.ip, status.ssid, status.rssi
}
```

### 6. Adicione Opcionais Conforme A Necessidade

Para sincronismo NTP, copie `tele_time_sync`, adicione a dependencia no CMake,
chame `time_sync_init()` no boot e acione o modulo pelos eventos do
`wifi_manager`:

```c
if (event_id == WIFI_MANAGER_EVENT_STA_CONNECTED) {
    time_sync_on_network_connected();
} else if (event_id == WIFI_MANAGER_EVENT_STA_DISCONNECTED ||
           event_id == WIFI_MANAGER_EVENT_PROVISIONING_STARTED) {
    time_sync_on_network_disconnected();
}
```

Para botao de provisionamento forcado, copie `tele_boot_config_button` e use o
resultado ao montar `wifi_manager_config_t`:

```c
wifi_config.force_provisioning = boot_config_button_is_pressed();
```

Para configuracao via `tele_config`, copie `tele_config` e
`tele_wifi_device_config`, chame `device_config_register_fields()` e monte o
`wifi_manager_config_t` lendo `tele_config_get_effective()`. Esse caminho e
util para projetos que querem expor os mesmos campos por MQTT, web ou outro
canal generico.

### 7. Contrato De Reuso

Em outro projeto, mantenha estas fronteiras:

- `tele_wifi` gerencia Wi-Fi, credenciais, scan, estado e eventos;
- a aplicacao decide quando iniciar portal, MQTT, OTA ou outros servicos;
- provisionamento e uma politica da aplicacao, mesmo que use AP criado por
  `tele_wifi`;
- time sync, botao fisico e configuracao remota sao adaptadores opcionais.

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

`tele_portal_wifi` e apenas um adaptador HTTP/JSON sobre `wifi_manager`; ele
nao deve conter logica propria de conexao Wi-Fi.

## Sincronismo De Tempo

`main/connectivity` chama o modulo de time sync quando recebe evento de STA
conectada. O MQTT usa isso por meio de `tele_presence_ready()`, que so permite
cliente MQTT quando Wi-Fi e horario estao prontos.

## Como Integrar Uma Rotina De Rede No App

1. Inicialize NVS e componentes de base.
2. Chame `connectivity_controller_start()`.
3. Inicie uma task propria.
4. Dentro da task, chame `wifi_manager_wait_until_ready()`.
5. Execute a rotina e reprograme com intervalo/jitter se necessario.

Essa regra vale para checks periodicos de certificado e OTA.
