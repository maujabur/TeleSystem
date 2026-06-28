# Indice de Arquitetura

Este diretorio contem a documentacao viva do TeleSystem. Planos antigos,
roadmaps ja executados e documentos de estrutura que duplicavam o codigo atual
foram removidos para evitar decisoes baseadas em informacao ultrapassada.

A arquitetura atual e organizada em camadas pequenas:

- registries reutilizaveis: `tele_config`, `tele_status`, `tele_commands`,
  `tele_artifacts` e `tele_indicator`;
- dominios de firmware: Wi-Fi, portal HTTP, updates, energia/OTA e MQTT;
- adapters do produto: `main/connectivity`, `main/app_indicators.c`,
  `tele_presence`, `tele_system_registry`, `tele_wifi_device_config` e
  bindings `tele_portal_*`;
- transportes: portal HTTP embarcado e MQTT, ambos chamando os registries em vez
  de duplicar regra de negocio.

## Guias Por Grupo De Componentes

Leia estes documentos primeiro:

1. [componentes_wifi_conectividade.md](componentes_wifi_conectividade.md)
2. [componentes_sistema.md](componentes_sistema.md)
3. [componentes_manifest_updates.md](componentes_manifest_updates.md)
4. [componentes_portal.md](componentes_portal.md)
5. [componentes_mqtt_config_status_commands.md](componentes_mqtt_config_status_commands.md)

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

- `main/main.c`: boot do produto. Inicializa logs, VBAT, power-good, NVS, CA
  store, OTA, artefatos, comandos de artefato, rotas web de comandos,
  conectividade, updater opcional de CA no boot e presenca MQTT.
- `main/connectivity`: adapter da aplicacao para Wi-Fi. Monta
  `wifi_manager_config_t` a partir de `tele_config`, inicia Wi-Fi, sincroniza
  portal, time sync e indicadores.
- `main/app_indicators.c`: adapter do produto entre eventos semanticos de
  `tele_indicator` e o driver `status_led`.
- `components/tele_wifi`, `tele_wifi_device_config`,
  `tele_boot_config_button` e `tele_time_sync`: Wi-Fi base, configuracao
  remota de Wi-Fi, botao de provisionamento e NTP.
- `components/tele_system`, `tele_signal`, `tele_indicator` e `status_led`:
  versao, OTA de firmware, VBAT, power-good, tipos de sinal e indicacao visual.
- `components/tele_manifest`, `tele_artifacts`, `tele_ca_store` e
  `tele_ca_updater`: updates por manifest para firmware e CA bundle.
- `components/tele_portal`, `tele_portal_core`, `tele_portal_assets`,
  `tele_portal_captive`, `tele_portal_config`, `tele_portal_status`,
  `tele_portal_wifi`, `tele_portal_logs`, `tele_portal_commands`,
  `tele_portal_ota` e `tele_firmware_portal_ota`: servidor HTTP embarcado,
  assets, rotas locais e upload OTA por portal.
- `components/tele_channels`, `tele_config`, `tele_status`, `tele_commands`,
  `tele_core_commands`, `tele_mqtt`, `tele_presence` e
  `tele_system_registry`: contratos config/status/commands, canais de exposicao,
  comandos core, transporte MQTT e bindings do produto.

## Fluxo De Boot Atual

Ordem de alto nivel:

1. Inicializar logs do portal, VBAT e power-good.
2. Medir bateria no boot; se critica, desligar perifericos e entrar em deep
   sleep.
3. Inicializar NVS.
4. Inicializar CA store e OTA de firmware.
5. Registrar handlers `ca_bundle` e `firmware` em `tele_artifacts`.
6. Registrar comandos genericos de artefato em `tele_commands`.
7. Registrar rotas web de comandos via `tele_portal_core_register_routes()`.
8. Iniciar `connectivity_controller_start()`.
9. Iniciar task opcional de update de CA por manifest, se configurada.
10. Chamar `tele_presence_start()`; se MQTT estiver desabilitado por Kconfig,
    ela retorna sem iniciar cliente.

## Fronteiras Importantes

- Regra de negocio fica nos componentes de dominio; portal HTTP e MQTT sao
  adapters.
- `tele_wifi` nao conhece portal, MQTT, botao fisico, NTP nem `tele_config`; a
  aplicacao combina esses comportamentos em `main/connectivity`.
- `firmware_ota` e o dono unico da escrita na particao OTA, do boot partition e
  do status de progresso. Portal e comandos de artefato chamam esse servico.
- `tele_portal_ota` e transporte por upload; `tele_firmware_portal_ota` e apenas
  binding para `firmware_ota`.
- `tele_system_registry` registra status/config de produto quando a presenca
  MQTT esta habilitada. O portal de status tambem pode consultar dominios
  diretamente por seus adapters.
- Indicacao visual passa por `tele_indicator`; dominios levantam eventos
  semanticos e o adapter do produto decide o efeito fisico.

## Regra De Manutencao

Ao adicionar ou alterar um componente, atualize o guia do grupo correspondente.
Se a mudanca cria uma nova interface publica, inclua um exemplo minimo de uso e
o ponto de inicializacao esperado.
