# Estrategia de Componentes Reutilizaveis

Este projeto pode servir como origem de componentes ESP-IDF consumidos por
outros firmwares via ESP-IDF Component Manager. A ideia principal e separar o
contrato interno do firmware dos adaptadores de transporte.

## Camadas

### Contratos de firmware

Estes componentes nao dependem de MQTT, web ou protocolo externo especifico:

- `tele_config`: registry de settings, validacao e overrides em NVS;
- `tele_status`: registry read-only de status;
- `tele_commands`: registry de comandos e metadados de argumentos.

Eles podem ser usados por qualquer interface: MQTT, HTTP, portal web, console
serial, BLE, CAN bus, testes automatizados ou ferramentas locais.

### Adaptadores opcionais

Adaptadores expoem os contratos por um transporte ou UI especifica:

- `tele_mqtt`: publica config/status/commands via MQTT;
- `tele_portal_core`: servidor HTTP generico, registro de rotas e callbacks;
- `tele_portal_logs`: captura logs em memoria e expoe endpoint HTTP opcional;
- `tele_portal_config`: expoe `tele_config` via HTTP;
- `tele_portal_status`: expoe `tele_status` e status tecnico via HTTP;
- `tele_portal_wifi`: expoe configuracao e scan Wi-Fi via HTTP;
- `tele_portal_captive`: redirects e DNS para captive portal;
- `tele_portal_ota`: upload OTA web por callbacks;
- `tele_portal`: agregador local do TeleSystem enquanto a migracao avanca;
- futuro `tele_can`: poderia mapear config/status/commands para CAN bus;
- futuro console serial ou BLE pode seguir o mesmo principio.

Um projeto pode usar `tele_config`, `tele_status` e `tele_commands` sem usar
`tele_mqtt`.

### Integracao de produto

A camada do produto registra campos, comandos e callbacks concretos:

- defaults de `sdkconfig.defaults`;
- campos de Wi-Fi, energia, sensores e dominio;
- callbacks de aplicacao runtime;
- callbacks de readiness, timestamp e reboot;
- status tecnico especifico.

No TeleSystem, essa cola vive principalmente em `tele_presence` e nos
componentes de produto.

## Consumo via ESP-IDF Component Manager

Um projeto consumidor pode depender apenas dos contratos:

```yaml
dependencies:
  tele_config:
    git: https://github.com/maujabur/TeleSystem.git
    path: components/tele_config
    version: lib-v0.1.0

  tele_status:
    git: https://github.com/maujabur/TeleSystem.git
    path: components/tele_status
    version: lib-v0.1.0

  tele_commands:
    git: https://github.com/maujabur/TeleSystem.git
    path: components/tele_commands
    version: lib-v0.1.0
```

Se o projeto tambem quiser expor esses contratos via MQTT:

```yaml
dependencies:
  tele_config:
    git: https://github.com/maujabur/TeleSystem.git
    path: components/tele_config
    version: lib-v0.1.0

  tele_status:
    git: https://github.com/maujabur/TeleSystem.git
    path: components/tele_status
    version: lib-v0.1.0

  tele_commands:
    git: https://github.com/maujabur/TeleSystem.git
    path: components/tele_commands
    version: lib-v0.1.0

  tele_mqtt:
    git: https://github.com/maujabur/TeleSystem.git
    path: components/tele_mqtt
    version: lib-v0.1.0
```

Os nomes de tag acima sao uma proposta. Antes de publicar, o repositorio deve
criar uma tag real e estavel para o conjunto de componentes.

Para usar apenas o nucleo HTTP do portal, sem Wi-Fi, MQTT, OTA ou componentes
de produto:

```yaml
dependencies:
  tele_portal_core:
    git: https://github.com/maujabur/TeleSystem.git
    path: components/tele_portal_core
    version: lib-v0.1.0

  tele_portal_logs:
    git: https://github.com/maujabur/TeleSystem.git
    path: components/tele_portal_logs
    version: lib-v0.1.0
```

Para montar um portal mais completo, o consumidor declara somente os
adaptadores que usa. Por exemplo, status/config por HTTP:

```yaml
dependencies:
  tele_config:
    git: https://github.com/maujabur/TeleSystem.git
    path: components/tele_config
    version: lib-v0.1.0

  tele_status:
    git: https://github.com/maujabur/TeleSystem.git
    path: components/tele_status
    version: lib-v0.1.0

  tele_portal_core:
    git: https://github.com/maujabur/TeleSystem.git
    path: components/tele_portal_core
    version: lib-v0.1.0

  tele_portal_config:
    git: https://github.com/maujabur/TeleSystem.git
    path: components/tele_portal_config
    version: lib-v0.1.0
```

Enquanto os componentes vivem no mesmo repositorio multi-componente, a forma
mais previsivel e declarar explicitamente no projeto consumidor os componentes
internos que ele usa. Os `CMakeLists.txt` continuam expressando o grafo real de
compilacao; os `idf_component.yml` dos componentes ficam focados em metadados e
dependencias externas. Isso evita que o solver tente buscar componentes internos
no registry durante builds locais offline.

## Consumo externo offline/local

Durante desenvolvimento sem acesso ao registry da Espressif, um projeto externo
pode consumir os componentes por `EXTRA_COMPONENT_DIRS` e desabilitar o
Component Manager. Esse modo nao substitui o fluxo publicavel por
`git/path/version`; ele apenas evita buscas de rede enquanto os componentes e
dependencias externas ainda estao sendo validados localmente.

Exemplo de `CMakeLists.txt` de um consumidor externo minimo:

```cmake
cmake_minimum_required(VERSION 3.22)

set(EXTRA_COMPONENT_DIRS
    /workspaces/telecafezinho/managed_components/espressif__cjson
    /workspaces/telecafezinho/components/tele_config
    /workspaces/telecafezinho/components/tele_status
    /workspaces/telecafezinho/components/tele_commands
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(telesystem_consumer_test)
```

Nesse modo, o `main/CMakeLists.txt` do consumidor declara os componentes usados:

```cmake
idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES nvs_flash tele_config tele_status tele_commands espressif__cjson
)
```

Build:

```bash
IDF_COMPONENT_MANAGER=0 idf.py -C /tmp/telesystem_consumer_test set-target esp32s3 build
```

Essa validacao ja foi feita com um consumidor externo em
`/tmp/telesystem_consumer_test`, usando `tele_config`, `tele_status` e
`tele_commands`.

Observacao importante: com `idf_component.yml` puro e sem `dependencies.lock`,
o Component Manager tenta consultar o registry para resolver `espressif/cjson`,
mesmo quando os componentes TeleSystem usam `path:` local. Para validacao
offline, use `EXTRA_COMPONENT_DIRS` + `IDF_COMPONENT_MANAGER=0`. Para consumo
publicavel, use `git/path/version` depois de criar uma tag real.

## Dependencias esperadas

O consumo isolado depende de manter as dependencias pequenas:

- `tele_config` depende de `nvs_flash` e `cJSON`;
- `tele_status` depende de `cJSON`;
- `tele_commands` depende de `cJSON`;
- `tele_mqtt` depende de MQTT, TLS/cert bundle e dos tres contratos;
- `tele_portal_core` depende de `esp_http_server` e `cJSON`;
- `tele_portal_logs`, `tele_portal_assets`, `tele_portal_captive` e
  `tele_portal_ota` dependem de `tele_portal_core`;
- `tele_portal_config` depende de `tele_config` e `tele_portal_core`;
- `tele_portal_status` depende de `tele_status`, `tele_system`, `tele_wifi` e
  `tele_portal_core`;
- `tele_portal_wifi` depende de `tele_wifi` e `tele_portal_core`;
- `tele_presence` nao deve ser tratado como componente generico: ele e exemplo
  de integracao de produto.

Cada componente reutilizavel deve ter `idf_component.yml` proprio. Os
`override_path` devem ficar nos projetos consumidores locais, nao nos manifests
publicaveis dos componentes.

## Uso com CAN bus

O mesmo sistema pode ser usado com CAN bus, mas CAN deve ser tratado como um
adaptador diferente de MQTT.

O que reaproveita bem:

- `tele_config` continua sendo fonte de settings;
- `tele_status` continua sendo fonte de leituras;
- `tele_commands` continua descrevendo acoes e argumentos;
- o produto continua registrando campos e callbacks uma vez.

O que muda no adaptador CAN:

- payloads CAN sao pequenos, entao manifests JSON grandes precisam de
  fragmentacao, codificacao binaria ou consulta por indice;
- CAN nao tem retained message como MQTT, entao o consumidor deve solicitar
  snapshots ou manter cache proprio;
- e preciso definir arbitration IDs, enderecamento de device, ACK/timeout e
  estrategia de retransmissao;
- comandos mutaveis precisam de deduplicacao ou nonce, assim como no MQTT;
- status periodico deve ser compacto e provavelmente separado por grupos.

Uma abordagem viavel seria:

- `tele_can_config`: leitura/escrita de settings por id numerico ou hash;
- `tele_can_status`: publicacao periodica de grupos de status compactos;
- `tele_can_commands`: envio de comandos com `cmd_id` curto e ACK;
- ferramenta de diagnostico que usa o mesmo manifesto, possivelmente obtido por
  outro canal ou por transferencias CAN fragmentadas.

CAN e uma boa opcao para operacao local/industrial, mas nao deve tentar copiar
o formato MQTT literalmente. O contrato logico e o mesmo; o framing precisa ser
proprio para CAN.

## Estado Atual

- `examples/component_consumer_minimal` valida `tele_config`, `tele_status` e
  `tele_commands` sem `tele_mqtt`.
- `examples/component_consumer_mqtt` valida os mesmos contratos com `tele_mqtt`
  como adaptador opcional.
- `examples/component_consumer_portal` valida `tele_portal_core` e
  `tele_portal_logs` sem Wi-Fi, MQTT, OTA, `tele_system`, `tele_status` ou
  `tele_config`.
- `tele_config`, `tele_status`, `tele_commands`, `tele_mqtt` e a familia
  `tele_portal_*` possuem `idf_component.yml` proprio com metadados e
  dependencias externas.
- Os exemplos usam `path:` para consumir os componentes locais e
  `override_path:` para reaproveitar `managed_components` durante validacao
  dentro deste repositorio.
- Um consumidor externo minimo em `/tmp/telesystem_consumer_test` validou o uso
  offline/local de `tele_config`, `tele_status` e `tele_commands` por
  `EXTRA_COMPONENT_DIRS` com `IDF_COMPONENT_MANAGER=0`.

## Proximas Acoes

1. Criar um consumidor externo com `tele_wifi` + `tele_portal_core` +
   `tele_portal_logs` para validar portal acessivel pelo navegador.
2. Testar consumo `git/path/version` com rede ou com uma tag/lock real.
3. Revisar os metadados publicaveis dos componentes antes da primeira tag
   (`license`, `description`, `repository`, `url`, `version`).
4. Criar a tag `lib-v0.1.0` quando a API estiver estavel o suficiente para
   consumo por outros projetos.
5. Decidir se os componentes continuam neste repositorio ou migram para um
   repositorio compartilhado dedicado.
