# Component Consumer Portal

Projeto ESP-IDF minimo para validar consumo isolado do portal HTTP:

- `tele_portal_core`
- `tele_portal_logs`

Este exemplo nao usa Wi-Fi, MQTT, OTA, `tele_system`, `tele_status` ou
`tele_config`. Ele registra uma rota propria `GET /api/ping` e inicia o servidor
HTTP generico.

Como o exemplo nao cria Wi-Fi/AP, ele valida o consumo e a inicializacao do
servidor HTTP, mas nao oferece acesso pelo navegador sozinho. Para teste real no
navegador, use um consumidor que combine `tele_wifi` com `tele_portal_core`.

## Build local

Na raiz do repositorio:

```bash
idf.py -C examples/component_consumer_portal build
```

O `main/idf_component.yml` usa `path:` para apontar para os componentes locais.
Em um projeto externo, troque esses paths pelos blocos `git` comentados no
manifesto do exemplo.
