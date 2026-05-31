# Arquitetura atual: conectividade Wi-Fi com captive portal

## Objetivo

Esta arquitetura organiza o projeto em torno de um nucleo reutilizavel de conectividade:

- tentar conectar em STA usando credenciais persistidas;
- cair para AP + captive portal quando nao houver credenciais ou a conexao falhar;
- expor uma interface web minima para provisioning e manutencao;
- manter Wi-Fi, portal web, persistencia e logica da aplicacao em camadas separadas.

O ponto principal e que `wifi_manager` nao conhece HTTP, HTML, DNS captive portal ou arquivos de UI. Ele e somente a maquina de estados Wi-Fi. A politica que liga Wi-Fi e portal vive em `connectivity_controller`.

## Camadas

```text
app_main
  |
  v
connectivity_controller
  |                  |
  v                  v
wifi_manager      web_portal
  |                  |
  v                  v
esp_wifi          browser/client
esp_netif

stores
  |
  +-- wifi_config       credenciais Wi-Fi
  +-- app modules       configuracoes especificas da aplicacao
```

Responsabilidades:

- `app_main`: boot minimo, montagem de storage, inicio do controlador e fluxo principal da aplicacao.
- `connectivity_controller`: politica de conectividade; inicia `wifi_manager`, escuta eventos Wi-Fi e sobe/ajusta `web_portal`.
- `wifi_manager`: estado Wi-Fi, transicoes STA/AP/APSTA, retries, eventos e API publica de conectividade.
- `wifi_config`: persistencia das credenciais Wi-Fi.
- `status_led`: apresentacao fisica de estados do produto em LED WS28xx; nao controla Wi-Fi.
- `web_portal`: HTTP server, captive portal, assets web e endpoints REST.
- modulos de aplicacao: configuracoes e rotas especificas do produto, como ACRCloud neste projeto.

## Regra de dependencias

Direcao desejada:

```text
app_main -> connectivity_controller
connectivity_controller -> wifi_manager
connectivity_controller -> web_portal
connectivity_controller -> status_led
web_portal -> wifi_manager
web_portal -> application modules
wifi_manager -> wifi_config
```

Regras:

- `wifi_manager` nao inclui `web_portal.h`.
- `wifi_manager` nao inclui `status_led.h`.
- `wifi_manager` nao le HTML, nao registra rotas HTTP e nao controla DNS captive portal.
- `wifi_manager` publica estados; quem decide como exibir esses estados no LED e o `connectivity_controller`.
- `web_portal` nao chama `esp_wifi_*` diretamente.
- handlers HTTP nao escrevem arquivo/NVS diretamente quando houver um store dedicado.
- configuracoes especificas da aplicacao nao devem virar requisito do nucleo de conectividade.

Politica permanente: `wifi_manager` deve continuar livre de dependencias de portal web, captive portal, storage de UI e modulos de aplicacao. Ele pode expor estado de provisionamento porque isso faz parte da conectividade, mas nao deve conhecer como esse provisionamento e apresentado ao usuario.

## Fluxo de boot atual

Fluxo desejado e atualmente usado:

```c
ESP_ERROR_CHECK(nvs_flash_init());
ESP_ERROR_CHECK(storage_mount());
ESP_ERROR_CHECK(connectivity_controller_start());
ESP_ERROR_CHECK(app_network_ready_wait(timeout));
```

`app_network_ready_wait(...)` e um marcador conceitual: a implementacao pode chamar
`wifi_manager_wait_until_ready(...)` diretamente no `app_main` ou encapsular essa espera
em um orquestrador chamado no fluxo principal. O requisito de arquitetura e manter essa
sincronizacao no `main` ou imediatamente ao redor dele, antes das operacoes que exigem rede.

Com credenciais validas:

1. `connectivity_controller_start()` registra eventos do `wifi_manager`.
2. `connectivity_controller` verifica politicas de boot, como o botao fisico de configuracao.
3. Se nenhuma politica forcar provisionamento, `wifi_manager` carrega credenciais por `wifi_config`.
4. `wifi_manager` entra em `STA_CONNECTING`.
5. Ao obter IP, publica evento de STA conectado.
6. `connectivity_controller` inicia ou ajusta `web_portal` para modo normal.
7. O fluxo principal da aplicacao prossegue quando a espera de rede retorna `ESP_OK`.

Com botao de configuracao pressionado no boot:

1. `boot_config_button` le o GPIO configurado, por padrao IO45 ativo em nivel baixo.
2. `connectivity_controller` passa `force_provisioning = true` para `wifi_manager`.
3. `wifi_manager` ignora credenciais salvas apenas neste boot e entra diretamente em `PROVISIONING_AP`.
4. `connectivity_controller` inicia `web_portal` em modo captive.

Sem credenciais, ou apos falha de conexao:

1. `wifi_manager` entra em `PROVISIONING_AP`.
2. Publica evento de inicio de provisionamento.
3. `connectivity_controller` inicia `web_portal` em modo captive.
4. Usuario envia credenciais por `/api/wifi`.
5. `web_portal` chama `wifi_manager_apply_wifi_credentials(...)`.
6. `wifi_manager` persiste via `wifi_config` e tenta STA.
7. Ao conectar, a aplicacao principal e liberada.

## Politica APSTA atual

O projeto ja possui politica configuravel de APSTA, definida em configuracao de dispositivo
e aplicada no boot pelo `connectivity_controller` ao iniciar o `wifi_manager`.

Politicas implementadas:

- `WIFI_MANAGER_APSTA_ALWAYS_ON`: mantem APSTA ativo apos conectar em STA;
- `WIFI_MANAGER_APSTA_AUTO_TIMEOUT`: mantem APSTA por janela de grace period e depois derruba AP;
- `WIFI_MANAGER_APSTA_STA_ONLY`: opera apenas em STA apos conexao.

A regra de produto continua fora do `wifi_manager`: o controlador decide politica e parametros,
enquanto o `wifi_manager` executa a politica selecionada.

## `wifi_manager`

Responsavel por:

- inicializar e controlar `esp_netif`, `esp_event` e `esp_wifi`;
- criar interfaces STA/AP;
- conectar em STA com credenciais ativas;
- iniciar AP de provisioning quando necessario;
- controlar retries;
- manter status consultavel;
- publicar eventos internos;
- aplicar novas credenciais recebidas por qualquer frontend.

API atual/recomendada:

```c
typedef enum {
    WIFI_MANAGER_STATE_INIT = 0,
    WIFI_MANAGER_STATE_STA_CONNECTING,
    WIFI_MANAGER_STATE_STA_CONNECTED,
    WIFI_MANAGER_STATE_PROVISIONING_AP,
} wifi_manager_state_t;

typedef enum {
    WIFI_MANAGER_APSTA_ALWAYS_ON = 0,
    WIFI_MANAGER_APSTA_AUTO_TIMEOUT,
    WIFI_MANAGER_APSTA_STA_ONLY,
} wifi_manager_apsta_policy_t;

typedef struct {
    bool wifi_ready;
    bool provisioning_active;
    wifi_manager_state_t state;
    char ip[16];
    char ssid[33];
    char provisioning_ssid[33];
    char last_error[96];
    int rssi;
    int sta_max_retry;
    uint32_t sta_reconnect_attempts;
    uint32_t invalid_transition_count;
    wifi_manager_apsta_policy_t apsta_policy;
    uint32_t apsta_grace_period_s;
    bool apsta_auto_drop_pending;
} wifi_manager_status_t;

typedef struct {
    const char *provisioning_ssid;
    bool force_provisioning;
    int sta_max_retry;
    wifi_manager_apsta_policy_t apsta_policy;
    uint32_t apsta_grace_period_s;
} wifi_manager_config_t;

esp_err_t wifi_manager_start(void);
esp_err_t wifi_manager_start_with_config(const wifi_manager_config_t *config);
esp_err_t wifi_manager_wait_until_ready(TickType_t timeout_ticks);
esp_err_t wifi_manager_apply_wifi_credentials(const char *ssid, const char *password);
esp_err_t wifi_manager_reconnect_sta(void);
esp_err_t wifi_manager_set_sta_max_retry(int retry);
esp_err_t wifi_manager_set_apsta_policy(wifi_manager_apsta_policy_t policy,
                                        uint32_t grace_period_s);
esp_err_t wifi_manager_note_portal_activity(void);
esp_err_t wifi_manager_set_high_throughput_mode(bool enabled);
esp_err_t wifi_manager_get_status(wifi_manager_status_t *status);
esp_err_t wifi_manager_scan_networks(wifi_manager_network_t *networks,
                                     size_t max_networks,
                                     size_t *network_count);
```

Eventos atuais/recomendados:

- `WIFI_MANAGER_EVENT_PROVISIONING_STARTED`
- `WIFI_MANAGER_EVENT_PROVISIONING_STOPPED`
- `WIFI_MANAGER_EVENT_STA_CONNECTING`
- `WIFI_MANAGER_EVENT_STA_CONNECTED`
- `WIFI_MANAGER_EVENT_STA_DISCONNECTED`
- `WIFI_MANAGER_EVENT_CREDENTIALS_UPDATED`

Observacao importante: parametros de provisioning, como SSID do AP, entram por `wifi_manager_config_t`. O `wifi_manager` nao deve abrir NVS ou arquivos de configuracao diretamente.

## `connectivity_controller`

Responsavel por:

- carregar configuracoes de conectividade que nao pertencem ao driver Wi-Fi, como SSID de provisionamento em NVS com fallback para menuconfig;
- ler politicas de boot do produto, como botao fisico que forca provisionamento;
- registrar handler para eventos `WIFI_MANAGER_EVENT`;
- iniciar `wifi_manager_start_with_config(...)`;
- subir `web_portal` em modo captive quando o estado for `PROVISIONING_AP`;
- subir ou ajustar `web_portal` em modo normal quando o estado for `STA_CONNECTED`;
- mapear estados Wi-Fi para apresentacoes de produto, como cores/piscadas do LED de status;
- concentrar politicas de produto relacionadas a conectividade.

## `status_led`

Responsavel por:

- controlar o LED WS28xx por RMT;
- aplicar brilho global, cores e padroes configurados por menuconfig;
- converter as cores logicas RGB (`0xRRGGBB`) para a ordem fisica do chip, como `GRB`, `RGB`, `BRG` ou outras variantes de 3 canais;
- expor API simples para estado base e overlay de captura.

API atual:

```c
esp_err_t status_led_start(void);
status_led_state_t status_led_get_state(void);
esp_err_t status_led_set_state(status_led_state_t state);
esp_err_t status_led_set_capture_overlay(bool enabled);
```

Politicas de conectividade (APSTA, disponibilidade de portal e recuperacao de
rede) continuam fora do `status_led`, em `connectivity_controller` e
`wifi_manager`.

## `wifi_config`

Responsavel por persistir apenas credenciais Wi-Fi.

Contrato:

```c
typedef struct {
    char ssid[33];
    char password[65];
    bool provisioned;
} wifi_credentials_t;

esp_err_t wifi_config_load(wifi_credentials_t *cfg);
esp_err_t wifi_config_save(const wifi_credentials_t *cfg);
esp_err_t wifi_config_clear(void);
```

Backend atual:

- namespace NVS: `wifi`;
- chaves: `ssid`, `password`;
- `provisioned` e derivado de SSID nao vazio.

Motivo para NVS:

- leitura rapida no boot;
- nao depende de `/data` para conectividade basica;
- combina melhor com segredo curto e mutavel.

## `web_portal`

Responsavel por:

- iniciar/parar `esp_http_server`;
- servir assets da UI;
- registrar rotas HTTP;
- expor endpoints JSON;
- chamar APIs publicas de `wifi_manager` e dos modulos de aplicacao.

O comportamento captive especifico fica em `captive_portal`:

- registrar rotas de deteccao captive;
- responder 404 com redirect quando o modo captive esta ativo;
- iniciar/parar o DNS wildcard via `dns_server`.

Rotas de conectividade atuais/recomendadas:

- `GET /`
- `GET /api/status`
- `GET /api/wifi/networks`
- `POST /api/wifi`

Observacao de ownership: `GET /api/status` permanece no nucleo e expoe apenas estado
de sistema/conectividade. Estado tecnico de modulo de aplicacao (como ACR) deve viver
em endpoint do proprio modulo, por exemplo `GET /api/acr/status`.

Rotas captive portal atuais/recomendadas:

- `/generate_204`
- `/gen_204`
- `/hotspot-detect.html`
- `/connecttest.txt`
- `/ncsi.txt`
- `/fwlink`
- outras rotas usadas por Android, iOS e Windows para deteccao de captive portal.

Endpoints de aplicacao podem ser anexados ao mesmo servidor, mas nao fazem parte do nucleo reutilizavel.

## Modulos de aplicacao

Este projeto tem configuracao ACRCloud, mas ela nao deve ser hardcoded na arquitetura basica de conectividade.

Modelo desejado:

```text
connectivity core
  - connectivity_controller
  - wifi_manager
  - wifi_config
  - web_portal
  - captive_portal
  - dns_server

application modules
  - acr_config_store
  - acr_routes
  - acr_ui/assets
  - acr_client
```

Para este projeto, `acr_config_store.c` cumpre o papel de store ACR:

- `region`, `container_id` e `bearer_token` em NVS, com fallback para menuconfig;
- certificado raiz em arquivo, com fallback embarcado.

Esse store e valido para o produto ACR, mas deve ser tratado como modulo anexo. Em uma biblioteca reutilizavel, o nucleo de conectividade permite registrar rotas/modulos externos em vez de conhecer `acr_config_store` ou ACRCloud.

Separacao aplicada:

- store ACR -> `acr_config_store.c`;
- handlers `/api/acr` e `/api/config` especificos -> `acr_routes.c`;
- pagina ACR -> asset ou modulo de UI separado;
- `web_portal` -> infraestrutura comum de servidor/rotas;
- `captive_portal` -> redirects, rotas captive e DNS wildcard.

## Contrato de extensao do portal

Para permitir reuso em outros projetos, o portal deveria evoluir para aceitar registro de rotas de aplicacao.

Formato conceitual:

```c
typedef esp_err_t (*web_portal_routes_register_fn)(httpd_handle_t server);

esp_err_t web_portal_register_app_routes(web_portal_routes_register_fn register_fn);
```

Assim:

- o nucleo registra `/`, `/api/status`, `/api/wifi`, captive redirects;
- o modulo ACR registra `/acr-settings`, `/api/acr`, `/api/config`, `/api/acr/status`;
- outro projeto poderia registrar suas proprias paginas e endpoints sem tocar no nucleo.

## UI

Estado atual:

- mesma infraestrutura em AP captive e em STA;
- Wi-Fi sempre parte do nucleo;
- paginas especificas da aplicacao aparecem como extensoes por rota.

O projeto usa `storage/web/index.html` como portal Wi-Fi principal. A rota tecnica `/acr-settings` serve `storage/web/acr-settings.html`, mantendo a UI ACR fora da pagina do usuario final.

## Estrutura de arquivos atual

Arquivos principais hoje:

```text
main/
  main.c
  audio/
    audio_capture.c
    audio_capture.h
    wav_writer.c
    wav_writer.h
    wm8782_probe.c
    wm8782_probe.h
  connectivity/
    boot_config_button.c
    boot_config_button.h
    connectivity_controller.c
    connectivity_controller.h
    device_config_routes.c
    device_config_routes.h
    device_config_store.c
    device_config_store.h
    status_led.c
    status_led.h
    status_led_stub.c
    wifi_manager.c
    wifi_manager.h
    wifi_config.c
    wifi_config.h
  portal/
    app_log_buffer.c
    app_log_buffer.h
    web_portal.c
    web_portal.h
    captive_portal.c
    captive_portal.h
    dns_server.c
    dns_server.h
    http_helpers.c
    http_helpers.h
  app/
    acr_analysis_control.c
    acr_analysis_control.h
    acr_config_store.c
    acr_config_store.h
    acr_orchestrator.c
    acr_orchestrator.h
    acr_runtime_status.c
    acr_runtime_status.h
    acr_routes.c
    acr_routes.h
    acr_trigger_output.c
    acr_trigger_output.h
    acr_client.c
    acr_client.h
    acr_parser.c
    acr_parser.h
    firmware_version.h
    storage.c
    storage.h
```

Possiveis separacoes futuras para reuso ainda maior:

```text
main/
  connectivity/
    connectivity_controller.c
    connectivity_controller.h
    wifi_manager.c
    wifi_manager.h
    wifi_config.c
    wifi_config.h
  portal/
    web_portal.c
    web_portal.h
    dns_server.c
    dns_server.h
    http_helpers.c
    http_helpers.h
    web_routes_core.c
    web_assets_core.c
  app/
    acr_config_store.c
    acr_config_store.h
    acr_routes.c
    acr_client.c
    acr_parser.c
```

A regra mais importante permanece aplicada: `wifi_manager` nao depende de `web_portal`, `captive_portal`, `dns_server`, storage de UI ou modulos de aplicacao.

## Endpoints ACR atuais

Estes endpoints existem neste projeto, mas pertencem ao modulo ACR, nao ao nucleo:

- `GET /api/config`
- `GET /api/acr/status`
- `POST /api/acr`
- `GET /api/acr/control`
- `POST /api/acr/control`
- `POST /api/acr/run`
- `GET /api/trigger-output`
- `POST /api/trigger-output`
- `POST /api/trigger-output/test`
- `GET /acr-settings`

Semantica desejada para token:

- `GET /api/config` nunca devolve bearer token completo;
- retorna apenas algo como `token_configured`;
- `POST /api/acr` aceita `bearer_token` apenas quando preenchido;
- valor vazio nao apaga token por acidente;
- leitura do token continua restrita ao backend.

## Checklist de ownership

Para manter a arquitetura reaproveitavel:

- `wifi_manager` controla Wi-Fi e estado de provisionamento, nao portal web, DNS captive, UI ou aplicacao.
- `connectivity_controller` controla politica, nao renderiza UI.
- `web_portal` serve HTTP, nao conhece regras internas de ACR.
- `wifi_config` salva credenciais Wi-Fi, nao conhece HTTP.
- stores de aplicacao salvam configuracoes da aplicacao, nao controlam Wi-Fi.
- `app_main` orquestra o boot, mas nao implementa retries nem captive portal.
- clientes de aplicacao, como `acr_client`, podem publicar eventos de dominio; a traducao desses eventos para LED fica no orquestrador/apresentacao, nao dentro do cliente HTTP.

## Proximos passos recomendados

1. Evoluir testes de contrato HTTP para garantir a separacao entre `/api/status` (nucleo) e `/api/acr/status` (modulo).
2. Avaliar se `web_portal` deve expor mais extensoes de UI alem do registro de rotas.
