# TeleSystem

TeleSystem e um firmware ESP-IDF para ESP32-S3 pensado como base de produto:
conectividade Wi-Fi, portal embarcado, presenca MQTT, comandos remotos, OTA por
upload ou manifest, CA bundle dinamico, status tecnico e indicacao visual.

O projeto nao tenta ser um monolito "esperto". A filosofia e manter dominios
pequenos, contratos explicitos e adapters finos. Wi-Fi nao conhece portal. Portal
nao decide regra de negocio. MQTT nao tem atalhos para estado interno. Cada
transporte chama registries e servicos de dominio que tambem podem ser usados
por outro transporte no futuro.

## Filosofia De Projeto

### Componentes pequenos, fronteiras claras

Cada componente deve ter um motivo simples para existir:

- `tele_wifi` gerencia Wi-Fi, credenciais, AP de provisionamento, AP+STA e
  eventos.
- `tele_system` cuida de firmware OTA, versao, VBAT e POWER_GOOD.
- `tele_portal_*` expoe HTTP local, assets, captive portal e adapters web.
- `tele_mqtt` transporta estado, comandos e telemetria por MQTT.

Quando uma regra pertence ao produto TeleSystem, ela entra em um adapter do
produto, como `main/connectivity`, `main/app_indicators.c`,
`tele_presence`, `tele_system_registry` ou `tele_wifi_device_config`.

### Registries antes de transportes

O firmware usa registries como contratos internos:

- `tele_config`: configuracoes persistiveis, validacao, NVS e callbacks runtime.
- `tele_status`: campos read-only para snapshots, heartbeat e diagnostico.
- `tele_commands`: comandos executaveis com metadados e flags por canal.
- `tele_artifacts`: tipos de artefato atualizaveis por manifest.
- `tele_indicator`: eventos logicos de indicacao visual com prioridade.

Portal HTTP, MQTT ou uma futura serial devem conversar com esses contratos, nao
duplicar regra de negocio.

### Reuso sem impor o produto

Componentes base devem poder sair deste firmware e viver em outro projeto
ESP-IDF. Por isso:

- `tele_wifi` nao depende de `tele_config`, portal, MQTT, NTP ou botao fisico.
- `tele_portal_ota` e apenas transporte HTTP de upload; a escrita OTA fica em
  `firmware_ota`.
- `status_led` recebe efeitos fisicos, nao estados semanticos de aplicacao.
- `tele_mqtt` sabe publicar/receber mensagens, mas nao conhece detalhes do
  produto.

## Arquitetura Em Camadas

```text
main/
  main.c                         boot do produto
  connectivity/                  adapter Wi-Fi/portal/time sync/indicador
  app_indicators.c               adapter indicador logico -> LED fisico

components/
  tele_channels                  flags de exposicao por canal
  tele_config                    registry de configuracao persistivel
  tele_status                    registry de status read-only
  tele_commands                  registry/dispatcher de comandos
  tele_artifacts                 registry de artefatos por manifest
  tele_manifest                  download/validacao de manifest e artefato
  tele_wifi                      Wi-Fi base, credenciais e provisionamento
  tele_wifi_device_config        campos Wi-Fi em tele_config
  tele_time_sync                 NTP e formatacao de horario
  tele_boot_config_button        botao fisico para forcar provisionamento
  tele_system                    versao, OTA, VBAT e POWER_GOOD
  tele_signal                    tipos de sinal fisico
  tele_indicator                 registry de indicadores logicos
  status_led                     driver/stub WS28xx
  tele_portal*                   portal HTTP e adapters web
  tele_mqtt                      nucleo MQTT reutilizavel
  tele_presence                  bootstrap MQTT do TeleSystem
  tele_system_registry           status/config comuns do produto
  tele_ca_store                  CA bundle dinamico em SPIFFS
  tele_ca_updater                artefato ca_bundle
```

## Fluxo De Boot

O ponto de entrada fica em `main/main.c`.

Ordem de alto nivel:

1. Inicializa logs locais do portal.
2. Inicializa VBAT, mede bateria no boot e aplica politica de shutdown.
3. Inicializa POWER_GOOD e liga perifericos quando seguro.
4. Inicializa NVS.
5. Inicializa CA store e OTA de firmware.
6. Registra artefatos `ca_bundle` e `firmware`.
7. Registra comandos genericos de artefato.
8. Registra rotas web de comandos.
9. Inicia conectividade Wi-Fi via `connectivity_controller_start()`.
10. Inicia update opcional de CA no boot, se configurado.
11. Chama `tele_presence_start()`; se MQTT estiver desligado por Kconfig, ela
    retorna sem iniciar cliente.

`main/connectivity` combina politicas do produto: cria o event loop, registra
eventos Wi-Fi, inicia indicadores, inicia NTP, carrega configuracoes Wi-Fi via
`tele_config`, le botao de boot e sincroniza portal/LED/time sync com o estado
do `wifi_manager`.

## Principais Dominios

### Wi-Fi e conectividade

`components/tele_wifi` e o componente base. Ele inicializa stack Wi-Fi, gerencia
STA/AP/AP+STA, salva credenciais em NVS, escaneia redes, publica eventos e expoe
`wifi_manager_wait_until_ready()`.

Adapters ao redor dele:

- `tele_wifi_device_config`: registra `wifi.provisioning_ssid`,
  `wifi.sta_max_retry`, `wifi.apsta_policy` e
  `wifi.apsta_grace_period_s` em `tele_config`.
- `tele_boot_config_button`: permite forcar provisionamento no boot.
- `tele_time_sync`: reage a eventos de rede para sincronizar horario.
- `main/connectivity`: decide quando iniciar portal normal ou captive.

### Portal HTTP embarcado

O portal e composto por componentes `tele_portal_*`:

- `tele_portal_core`: servidor HTTP, helpers e fila de registro de rotas.
- `tele_portal_assets`: HTML/CSS/JS embutidos a partir de `firmware_assets/web`.
- `tele_portal_captive`: endpoints de captive portal e redirecionamento.
- `tele_portal_wifi`: rotas de scan, redes salvas e credenciais.
- `tele_portal_status`: status local para UI.
- `tele_portal_config`: configuracoes expostas por HTTP.
- `tele_portal_commands`: adapter HTTP para `tele_commands`.
- `tele_portal_logs`: buffer e endpoint de logs, quando habilitado.
- `tele_portal_ota`: transporte HTTP para upload OTA.
- `tele_firmware_portal_ota`: binding entre upload web e `firmware_ota`.
- `tele_portal`: agregador que registra rotas em ordem segura.

As rotas especificas entram antes do captive portal e dos assets wildcard. Isso
mantem a UI simples sem misturar regra de negocio nos handlers HTTP.

### MQTT, config, status e comandos

`tele_mqtt` publica disponibilidade, estado, heartbeat, eventos e manifests de
config/status/commands. Tambem recebe `cmd/in` e publica `cmd/out`.

Topicos principais:

```text
{base_topic}/{device_id}/availability
{base_topic}/{device_id}/seen
{base_topic}/{device_id}/state
{base_topic}/{device_id}/heartbeat
{base_topic}/{device_id}/event
{base_topic}/{device_id}/meta/config
{base_topic}/{device_id}/meta/status
{base_topic}/{device_id}/meta/commands
{base_topic}/{device_id}/cmd/in
{base_topic}/{device_id}/cmd/out
```

`tele_presence` e o bootstrap do produto. Ele espera Wi-Fi e horario
sincronizado, registra `tele_system_registry`, configura callbacks de status
tecnico e inicia o cliente MQTT quando `CONFIG_MQTT_PRESENCE_ENABLED` esta
ativo.

Guia de operacao: [docs/manual_mqtt_operacao.md](docs/manual_mqtt_operacao.md).

### Updates por manifest e OTA

O sistema de updates e generico:

- `tele_manifest`: baixa manifest, valida schema, canal, URL HTTPS, tamanho e
  SHA-256.
- `tele_artifacts`: registra tipos de artefato e expoe `artifact/check`,
  `artifact/apply`, `artifact/status` e `artifacts/get`.
- `tele_ca_store`: monta SPIFFS `ca_store`, ativa CA bundle dinamico e promove
  bundles com `.tmp`/`.bak`.
- `tele_ca_updater`: handler do artefato `ca_bundle`.
- `tele_system/firmware_ota.c`: handler do artefato `firmware`, upload local,
  OTA por URL e OTA por manifest em streaming.

O firmware OTA e o dono unico da escrita na particao OTA, do boot partition, do
estado de progresso e do reboot. Portal e MQTT apenas acionam esse servico.

Ferramenta de manifests: [tools/update_artifacts/README.md](tools/update_artifacts/README.md).

### Sistema, energia e indicador

`tele_system` concentra:

- `firmware_version.h`: `APP_VERSION_SEMVER`, label e build id.
- `firmware_ota.c`: servico de OTA.
- `vbat_monitor.c`: leitura VBAT e politica de bateria critica.
- `power_good.c`: GPIO de alimentacao de perifericos.

Indicacao visual passa por tres camadas:

- `tele_signal`: tipos fisicos de cor, efeito e target.
- `status_led`: driver ou stub WS28xx.
- `tele_indicator`: eventos logicos com prioridade e duracao.

`main/app_indicators.c` liga o mundo logico ao LED fisico e define eventos como
`system.boot`, `wifi.connecting`, `wifi.provisioning`, `wifi.connected`,
`battery.low` e `system.error`.

## Hardware Alvo

Configuracao atual documentada para ESP32-S3 QFN56 v0.2:

- ESP32-S3 dual-core a 240 MHz.
- Flash externa de 8 MB.
- PSRAM de 8 MB.
- LED de status em GPIO 48.
- Botao de configuracao Wi-Fi.

Atualize esta secao quando o hardware final do TeleSystem for fechado.

## Particoes

Tabela atual em [partitions.csv](partitions.csv):

| Nome | Tipo | Subtipo | Offset | Tamanho | Uso |
|---|---|---|---:|---:|---|
| `nvs` | data | nvs | `0x9000` | `0x6000` | configuracoes e credenciais |
| `otadata` | data | ota | `0xf000` | `0x2000` | estado OTA |
| `phy_init` | data | phy | `0x11000` | `0x1000` | dados PHY |
| `ota_0` | app | ota_0 | `0x20000` | `0x200000` | slot app A |
| `ota_1` | app | ota_1 | `0x220000` | `0x200000` | slot app B |
| `ca_store` | data | spiffs | automatico | `0x80000` | CA bundle dinamico |

Antes de aumentar assets, firmware ou bundles, confira margem nos slots OTA e
na particao `ca_store`.

## Dependencias

- ESP-IDF com `idf.py` disponivel.
- Componentes gerenciados declarados em `idf_component.yml` e
  `main/idf_component.yml`:
  - `espressif/cjson`
  - `espressif/mqtt`

## Build E Flash

Build de desenvolvimento:

```bash
idf.py build
```

Flash e monitor:

```bash
idf.py -p /dev/ttyACM0 flash monitor
```

A porta serial pode variar conforme host e placa.

Build release:

```bash
idf.py -B build-release -D SDKCONFIG=sdkconfig.release -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.release.defaults" build
```

Mais comandos e diferencas entre perfis: [BUILD_PROFILES.md](BUILD_PROFILES.md).

## Configuracao

Configuracoes de build ficam nos `Kconfig` dos componentes e podem ser editadas
com:

```bash
idf.py menuconfig
```

Areas principais:

- Wi-Fi, provisionamento e politica AP+STA.
- Portal web, logs e exposicao de status.
- MQTT presence, QoS e heartbeat.
- Updates por manifest, CA bundle e OTA.
- LED WS28xx.
- Monitoramento de bateria e POWER_GOOD.

Configuracoes runtime que pertencem ao produto devem passar por `tele_config`
quando precisarem de persistencia, validacao, UI ou MQTT.

## Documentacao

Comece pelo indice vivo:

- [docs/arquitetura_index.md](docs/arquitetura_index.md)

Guias por grupo:

- [docs/componentes_wifi_conectividade.md](docs/componentes_wifi_conectividade.md)
- [docs/componentes_sistema.md](docs/componentes_sistema.md)
- [docs/componentes_manifest_updates.md](docs/componentes_manifest_updates.md)
- [docs/componentes_portal.md](docs/componentes_portal.md)
- [docs/componentes_mqtt_config_status_commands.md](docs/componentes_mqtt_config_status_commands.md)

Contratos especificos:

- [docs/tele_config.md](docs/tele_config.md)
- [docs/tele_status.md](docs/tele_status.md)
- [docs/tele_commands.md](docs/tele_commands.md)
- [docs/tele_indicator.md](docs/tele_indicator.md)
- [docs/manual_mqtt_operacao.md](docs/manual_mqtt_operacao.md)

## Regras Para Evoluir O Firmware

Ao adicionar comportamento novo:

1. Coloque regra de negocio em componente de dominio.
2. Exponha configuracao por `tele_config` se ela for persistivel ou operavel.
3. Exponha leitura por `tele_status` se portal/MQTT precisarem observar.
4. Exponha acao por `tele_commands` se mais de um transporte puder chamar.
5. Use adapters `tele_portal_*` ou MQTT apenas para traduzir protocolo.
6. Para rotinas de rede, espere `wifi_manager_wait_until_ready()` fora do
   componente de dominio.
7. Para indicador fisico, levante evento em `tele_indicator`; nao chame
   `status_led` direto de dominios de negocio.

Essa disciplina mantem o projeto facil de portar, testar e operar.
