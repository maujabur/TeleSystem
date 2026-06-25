# Indice de Arquitetura

Este diretorio contem a documentacao viva do TeleSystem. Planos antigos,
roadmaps ja executados e documentos de estrutura que duplicavam o codigo atual
foram removidos para evitar decisoes baseadas em informacao ultrapassada.

## Guias Por Grupo De Componentes

Leia estes documentos primeiro:

1. [componentes_manifest_updates.md](componentes_manifest_updates.md)
2. [componentes_mqtt_config_status_commands.md](componentes_mqtt_config_status_commands.md)
3. [componentes_portal.md](componentes_portal.md)
4. [componentes_wifi_conectividade.md](componentes_wifi_conectividade.md)
5. [componentes_sistema.md](componentes_sistema.md)

## Contratos Especificos

- [tele_config.md](tele_config.md): registry de configuracao persistivel.
- [tele_status.md](tele_status.md): registry de status read-only.
- [tele_commands.md](tele_commands.md): registry de comandos remotos.
- [manual_mqtt_operacao.md](manual_mqtt_operacao.md): topicos, payloads e comandos MQTT.
- [tools/update_artifacts/README.md](../tools/update_artifacts/README.md): geracao de manifests de firmware e CA.

## Guias Complementares

- [home_page_branding_manual.md](home_page_branding_manual.md): orientacao visual do portal.

## Planos Historicos

Planos em `docs/superpowers/plans/` sao historico de implementacao. Eles podem
ajudar a entender decisoes, mas nao devem substituir os guias acima ao usar ou
integrar componentes.

## Mapa Rapido

- `main/main.c`: inicializa NVS, CA store, OTA, portal, conectividade e MQTT.
- `main/connectivity`: adapta eventos Wi-Fi ao portal, indicador e sincronismo de tempo.
- `components/tele_manifest`, `tele_artifacts`, `tele_ca_store`,
  `tele_ca_updater` e `tele_system/firmware_ota.c`: updates por manifest.
- `components/tele_config`, `tele_status`, `tele_commands`, `tele_mqtt` e
  `tele_presence`: contrato MQTT/config/status/commands.
- `components/tele_portal*`: servidor HTTP embarcado e rotas do portal.
- `components/tele_wifi`: Wi-Fi, provisionamento, credenciais e NTP.
- `components/tele_system`, `components/tele_indicator` e `components/status_led`:
  OTA, versao, bateria, power-good e indicacao visual.

## Regra De Manutencao

Ao adicionar ou alterar um componente, atualize o guia do grupo correspondente.
Se a mudanca cria uma nova interface publica, inclua um exemplo minimo de uso e
o ponto de inicializacao esperado.
