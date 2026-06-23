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
