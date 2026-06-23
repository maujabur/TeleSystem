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
- `tele_portal`: pode evoluir para renderizar partes de config/status/commands
  via HTTP/web;
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

## Dependencias esperadas

O consumo isolado depende de manter as dependencias pequenas:

- `tele_config` depende de `nvs_flash` e `cJSON`;
- `tele_status` depende de `cJSON`;
- `tele_commands` depende de `cJSON`;
- `tele_mqtt` depende de MQTT, TLS/cert bundle e dos tres contratos;
- `tele_presence` nao deve ser tratado como componente generico: ele e exemplo
  de integracao de produto.

Antes de publicar, cada componente deve ter `idf_component.yml` proprio ou uma
estrategia equivalente aceita pelo Component Manager para consumo por `path`.

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
- `tele_config`, `tele_status`, `tele_commands` e `tele_mqtt` possuem
  `idf_component.yml` proprio com metadados e dependencias externas.
- Os exemplos usam `path:` para consumir os componentes locais e
  `override_path:` para reaproveitar `managed_components` durante validacao
  dentro deste repositorio.

## Proximas Acoes

1. Revisar os metadados publicaveis dos componentes antes da primeira tag
   (`license`, `description`, `repository`, `url`, `version`).
2. Criar a tag `lib-v0.1.0` quando a API estiver estavel o suficiente para
   consumo por outros projetos.
3. Testar consumo em um segundo projeto usando os blocos `git` dos exemplos.
4. Decidir se os componentes continuam neste repositorio ou migram para um
   repositorio compartilhado dedicado.
