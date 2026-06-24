# TeleSystem

Firmware ESP-IDF para ESP32-S3 usado como base do produto TeleSystem. O projeto integra conectividade Wi-Fi com portal de provisionamento, OTA HTTP, presenca MQTT, telemetria tecnica pelo portal embarcado e base de LED WS28xx.

## Visao geral

O ponto de entrada fica em `main/main.c`. No boot, o firmware inicializa logs, monitoramento de bateria, power-good, NVS, rotas HTTP, OTA, conectividade Wi-Fi e MQTT.

Principais subsistemas:

- `main`: boot e cola da aplicacao.
- `main/connectivity`: controller de conectividade e rotas de configuracao.
- `components/tele_wifi`: Wi-Fi, provisionamento, credenciais, APSTA e NTP.
- `components/tele_portal`: servidor HTTP, captive portal, helpers JSON/HTTP,
  buffer de logs e portal OTA.
- `components/tele_mqtt`: cliente MQTT reutilizavel.
- `components/tele_presence`: adaptador de presenca MQTT do firmware.
- `components/tele_system`: OTA, versao, VBAT e POWER_GOOD.
- `components/status_led`: driver/stub de LED WS28xx.
- `firmware_assets/web`: paginas HTML embarcadas no firmware.
- `docs`: documentacao de arquitetura e operacao.

## Hardware alvo

Configuracao atual documentada para ESP32-S3 QFN56 v0.2:

- ESP32-S3 dual-core a 240 MHz.
- Flash externa de 8 MB.
- PSRAM de 8 MB.
- LED de status em GPIO 48.
- Botao de configuracao Wi-Fi.

Atualize esta secao quando o hardware final do TeleSystem for fechado.

## Dependencias

- ESP-IDF com `idf.py` disponivel no ambiente.
- Componentes gerenciados:
  - `espressif/cjson`
  - `espressif/mqtt`

As dependencias ficam declaradas em `idf_component.yml` e `main/idf_component.yml`.

## Build rapido

Build de desenvolvimento, usando o perfil padrao:

```bash
idf.py build
```

Flash e monitor serial:

```bash
idf.py -p /dev/ttyACM0 flash monitor
```

A porta serial pode variar conforme o host e a placa.

## Perfis de build

O projeto usa dois perfis principais:

- Dev: logs e informacoes tecnicas habilitados para debug.
- Release: logs reduzidos, erros genericos e menor exposicao de dados sensiveis.

Build dev explicito:

```bash
idf.py -B build-dev -D SDKCONFIG=sdkconfig.dev -D SDKCONFIG_DEFAULTS="sdkconfig.defaults" build
```

Build release:

```bash
idf.py -B build-release -D SDKCONFIG=sdkconfig.release -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.release.defaults" build
```

Mais detalhes em [BUILD_PROFILES.md](BUILD_PROFILES.md).

## Configuracao

Configuracoes de build ficam nos `Kconfig` dos componentes e podem ser editadas
com:

```bash
idf.py menuconfig
```

Areas principais de configuracao:

- Wi-Fi e provisionamento.
- Politica APSTA e modem sleep.
- Portal web e exposicao de logs/status.
- MQTT presence e heartbeat.
- LED de status WS28xx.
- Monitoramento de bateria e power-good.

Configuracoes de runtime e credenciais sao persistidas em NVS.

## Portal embarcado

O firmware embarca paginas HTML de `firmware_assets/web` e expoe um portal tecnico para:

- status do dispositivo e da rede;
- configuracao Wi-Fi;
- configuracoes de conectividade/dispositivo;
- logs recentes, quando habilitados no perfil;
- OTA via HTTP;
- reinicio remoto.

O portal tambem atua como captive portal quando o dispositivo entra em modo de provisionamento.

## MQTT

Quando habilitado, o firmware publica presenca, estado e heartbeat em topicos no namespace:

```text
v1/led/{device_id}
```

Tambem existe canal de comando para operacoes como `ping`, leitura de estado/configuracoes e reboot remoto. Veja [docs/manual_mqtt_operacao.md](docs/manual_mqtt_operacao.md).

## Particoes

A tabela atual usa NVS, OTA data, PHY init e dois slots OTA de 2 MB:

- `ota_0`: `0x20000`, 2 MB.
- `ota_1`: `0x220000`, 2 MB.

Consulte [partitions.csv](partitions.csv) antes de alterar tamanho de firmware ou estrategia OTA.

## Documentacao

Comece por [docs/arquitetura_index.md](docs/arquitetura_index.md). Ele aponta a ordem recomendada de leitura.

Leituras uteis:

- [docs/componentes_manifest_updates.md](docs/componentes_manifest_updates.md)
- [docs/componentes_mqtt_config_status_commands.md](docs/componentes_mqtt_config_status_commands.md)
- [docs/componentes_portal.md](docs/componentes_portal.md)
- [docs/componentes_wifi_conectividade.md](docs/componentes_wifi_conectividade.md)
- [docs/componentes_sistema.md](docs/componentes_sistema.md)

## Notas de desenvolvimento

- `sdkconfig.defaults` e a base do perfil dev.
- Use `build-dev` e `build-release` para manter caches separados.
- Os HTMLs do portal sao embarcados via `EMBED_TXTFILES` em
  `components/tele_portal/CMakeLists.txt`.
- `tools/mqtt_desktop` contem o Jabur Consulting MQTT Control Center para
  administracao MQTT dinamica dos dispositivos.
