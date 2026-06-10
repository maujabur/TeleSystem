# Plano de reaproveitamento para proximo projeto com LED enderecavel

## Objetivo

Definir uma forma segura de aproveitar este projeto como base para um novo
firmware sem contaminar o repositorio atual.

O proximo projeto deve reaproveitar principalmente:

- captive portal e portal web;
- configuracao web/NVS;
- Wi-Fi manager e politicas de APSTA/provisionamento;
- MQTT de presenca/comando;
- OTA, se o novo produto tambem precisar de atualizacao remota;
- parte do driver de LED enderecavel ja existente.

O proximo projeto nao tera audio nem fluxo ACRCloud.

## Decisao recomendada

Duplicar a pasta inteira do projeto para fora deste repositorio e trabalhar
somente na copia.

Motivos:

- preserva o projeto atual exatamente no estado em que esta;
- mantem a configuracao de container, ESP-IDF, CMake, partitions, sdkconfig e
  dependencias ja funcionais;
- evita criar um projeto aninhado dentro deste repositorio;
- evita poluir `git status` com uma arvore temporaria grande;
- permite apagar a copia e tentar de novo sem risco para esta base.

Estrutura sugerida:

```text
/workspaces/acr-cloud-test        projeto atual, referencia intacta
/workspaces/telecafezinho        novo projeto derivado
```

## Antes de duplicar

1. Nao fazer commit deste plano se ele for apenas apoio temporario.
2. Confirmar que o projeto atual compila ou esta no estado desejado de
   referencia.
3. Fechar qualquer servidor, monitor serial ou processo de build rodando dentro
   da pasta atual.
4. Duplicar a pasta inteira para um novo nome fora deste repositorio.

Exemplo conceitual:

```sh
cp -a /workspaces/acr-cloud-test /workspaces/telecafezinho
```

Depois de duplicar, abrir a nova pasta como workspace e fazer todas as
alteracoes apenas nela.

## Primeira limpeza na copia

Na copia nova, remover do build os modulos especificos de audio e ACR.

Arquivos/pastas a remover ou deixar fora do `CMakeLists.txt`:

- `main/audio/*`
- `firmware_assets/teste.wav`
- `main/app/acr_analysis_control.*`
- `main/app/acr_client.*`
- `main/app/acr_config_store.*`
- `main/app/acr_orchestrator.*`
- `main/app/acr_parser.*`
- `main/app/acr_routes.*`
- `main/app/acr_runtime_status.*`
- `main/app/acr_trigger_output.*`, se a saida GPIO do projeto atual nao for
  reaproveitada
- qualquer probe de audio, especialmente `wm8782_probe.*`

Dependencias a revisar no `main/CMakeLists.txt`:

- remover `esp_driver_i2s`;
- remover dependencias usadas apenas por audio/ACR;
- manter `esp_wifi`, `esp_event`, `nvs_flash`, `esp_netif`, `esp_driver_gpio`,
  `esp_driver_rmt`, `esp_http_server`, `esp_https_ota`, `app_update`, `lwip`,
  `mbedtls`, `mqtt` e `espressif__cjson`.

## Modulos que devem permanecer

Conectividade:

- `components/tele_wifi`, com Wi-Fi, credenciais, SSID de provisionamento,
  botao de boot e NTP
- `main/connectivity/connectivity_controller.*`
- `main/connectivity/device_config_routes.*`
- `components/status_led`, driver WS28xx extraido como componente
- `components/tele_mqtt`, cliente MQTT reutilizavel
- `components/tele_presence`, adaptador de presenca MQTT

Portal:

- `components/tele_portal`, com web portal, captive portal, DNS, helpers HTTP,
  logs e portal OTA

Sistema/app de apoio:

- `components/tele_system`, com firmware version, OTA, VBAT e POWER_GOOD

Assets web:

- manter inicialmente `firmware_assets/web/*` para preservar o portal;
- depois substituir textos, marca, paginas e APIs especificas do ACR.

## Refatoracao do `main.c`

O `app_main` da copia deve virar um boot minimo do novo produto.

Fluxo alvo:

```c
app_log_buffer_init();
nvs_flash_init();
firmware_ota_init();              // opcional
ota_portal_register_with_portal(); // opcional
device_config_routes_register_with_portal();
led_controller_init();
led_routes_register_with_portal();
connectivity_controller_start();
mqtt_presence_start();
```

Remover do `main.c` novo:

- probe `WM8782`;
- inicializacao ACR;
- task do orquestrador ACR;
- qualquer dependencia direta de audio.

## MQTT: separacao necessaria

O arquivo `mqtt_presence.c` atual deve ser tratado como parcialmente
reaproveitavel.

Manter no nucleo MQTT:

- conexao TLS com broker;
- LWT online/offline;
- topicos `status`, `heartbeat`, `state`, `event`, `cmd/in`, `cmd/out`;
- `device_id`;
- `session_id`;
- deduplicacao por `cmd_id`;
- comandos genericos:
  - `ping`;
  - `get_state`;
  - `set_heartbeat_interval`;
  - `apply_and_reboot`.

Remover ou mover para adapter especifico:

- includes de `acr_*`;
- montagem de payloads ACR;
- comandos `get_settings` e `set_settings` baseados em ACR;
- suspensao de MQTT por ciclo ACR;
- campos de audio em status tecnico.

Criar um contrato de callback para comandos do produto:

```c
typedef esp_err_t (*mqtt_app_command_handler_t)(const char *cmd_name,
                                                const cJSON *args,
                                                cJSON **out_result);
```

No novo produto, implementar um adapter de LED:

- `set_color`;
- `set_brightness`;
- `set_effect`;
- `set_power`;
- `get_led_state`;
- `save_led_settings`.

Tambem ajustar o namespace default:

```text
v1/acr -> v1/led
```

## LED enderecavel

O `status_led` atual ja contem uma base boa para WS28xx via RMT:

- configuracao de GPIO;
- quantidade de LEDs;
- brilho;
- ordem RGB/GRB/RBG/etc.;
- encoder RMT;
- envio do buffer.

No novo projeto, separar em dois niveis:

```text
main/led/led_strip_driver.*   baixo nivel WS28xx/RMT
main/led/led_controller.*     estado, brilho, efeitos e cenas
main/led/led_config_store.*   persistencia NVS das preferencias
main/led/led_routes.*         API HTTP do produto
main/led/led_mqtt_commands.*  comandos remotos do produto
```

O `connectivity_controller` pode continuar exibindo estados de rede no LED, mas
isso deve ser uma politica clara:

- modo A: conectividade usa um LED reservado;
- modo B: conectividade aplica overlay temporario no mesmo strip;
- modo C: conectividade nao controla o LED principal, apenas publica estado.

Para produto de LED, a opcao B tende a ser a mais flexivel.

## Portal web

Manter inicialmente as rotas base:

- `GET /`
- `GET /status`
- `GET /settings`
- `GET /networks`
- `GET /logs`, se habilitado
- `GET /api/status`
- `GET /api/logs`, se habilitado
- `POST /api/restart`
- `POST /api/wifi`
- `GET /api/wifi/networks`
- `GET/PUT/DELETE /api/wifi/saved`
- `GET/POST /api/device/config`

Adicionar no novo produto:

- `GET /api/led/status`
- `POST /api/led`
- `POST /api/led/effect`
- `GET/POST /api/led/settings`

Depois de validar o firmware limpo, substituir os HTMLs em
`firmware_assets/web/` por uma interface focada em LED.

## Kconfig e defaults

Na copia, limpar menus que falam de ACR/audio e manter:

- Wi-Fi config;
- MQTT presence config;
- status LED ou LED strip config;
- OTA config;
- bateria/power-good somente se usados.

Renomear defaults de produto:

- `CONFIG_WIFI_PROVISIONING_SSID`;
- `CONFIG_MQTT_TOPIC_NAMESPACE`;
- versao/nome em `firmware_version.h`;
- textos de branding no portal.

## Sequencia de validacao na copia

1. Build limpo apos remover audio/ACR.
2. Boot sem inicializacao de audio.
3. AP de provisionamento sobe quando nao ha credenciais.
4. Portal abre no captive mode.
5. Credenciais Wi-Fi sao salvas em NVS.
6. Dispositivo conecta em STA.
7. Portal fica disponivel em modo normal.
8. MQTT publica `status=online`.
9. MQTT publica heartbeat periodico.
10. Comando MQTT `ping` responde.
11. Comando MQTT de LED altera cor/brilho.
12. Configuracao de LED persiste apos reboot.
13. OTA web/remoto continua funcional, se mantido.

## Criterio de sucesso da migracao

A copia sera considerada uma base boa para o novo projeto quando:

- compilar sem qualquer arquivo de audio ou ACR;
- iniciar Wi-Fi, portal e MQTT;
- controlar o LED enderecavel por API web;
- controlar o LED enderecavel por MQTT;
- persistir configuracoes principais em NVS;
- manter o projeto original sem alteracoes relevantes.

## Observacao sobre este repositorio

Este documento pode ser usado apenas como nota temporaria. Como a intencao e
nao dar commit antes de clonar a pasta, ele pode ser apagado depois que a copia
externa estiver criada.
