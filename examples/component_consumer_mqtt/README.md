# Component Consumer MQTT

Projeto ESP-IDF minimo para validar os contratos de firmware com o adaptador
`tele_mqtt`.

Este exemplo registra campos em `tele_config`, `tele_status` e
`tele_commands`, inicializa o cliente MQTT generico e deixa os payloads
`state`, `heartbeat`, `meta/config`, `meta/status` e `meta/commands` serem
montados pelo proprio `tele_mqtt`.

## Build local

Na raiz do repositorio:

```bash
idf.py -C examples/component_consumer_mqtt build
```

Por default, `EXAMPLE_MQTT_ENABLED` fica desativado para o exemplo compilar sem
broker. Habilite no `menuconfig` do exemplo e configure `EXAMPLE_MQTT_BROKER_URI`
para testar conexao real.
