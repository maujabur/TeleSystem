# Component Consumer Minimal

Projeto ESP-IDF minimo para validar consumo isolado dos contratos de firmware:

- `tele_config`
- `tele_status`
- `tele_commands`

Este exemplo nao usa MQTT. Ele prova que os registries podem existir sem
adaptador de transporte.

## Build local

Na raiz do repositorio:

```bash
idf.py -C examples/component_consumer_minimal build
```

O `main/idf_component.yml` usa `path:` para apontar para os componentes locais.
Em um projeto externo, troque esses paths pelos blocos `git` comentados no
manifesto do exemplo.

## Consumo via GitHub

Em um projeto externo com ESP-IDF Component Manager habilitado, declare os
componentes no `idf_component.yml` do consumidor:

```yaml
dependencies:
  espressif/cjson: "^1.7.19"
  tele_channels:
    git: https://github.com/maujabur/TeleSystem.git
    path: components/tele_channels
    version: main
  tele_config:
    git: https://github.com/maujabur/TeleSystem.git
    path: components/tele_config
    version: main
  tele_status:
    git: https://github.com/maujabur/TeleSystem.git
    path: components/tele_status
    version: main
  tele_commands:
    git: https://github.com/maujabur/TeleSystem.git
    path: components/tele_commands
    version: main
```

Troque `main` por uma tag ou commit para builds reproduziveis.

## Consumo externo offline

Sem acesso ao registry da Espressif, um projeto externo pode usar
`EXTRA_COMPONENT_DIRS` e compilar com `IDF_COMPONENT_MANAGER=0`. O procedimento
esta documentado em `docs/estrategia_component_manager.md`.
