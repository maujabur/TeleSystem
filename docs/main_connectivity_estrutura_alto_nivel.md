# Estrutura de main/connectivity em alto nivel

## Objetivo

Este documento descreve, em alto nivel, o subsistema de conectividade em main/connectivity:

- papel de cada arquivo;
- funcoes publicas e responsabilidade de cada modulo;
- como os modulos se integram;
- interfaces usadas fora de main/connectivity, sem detalhar internals dessas outras pastas;
- mecanismos de comunicacao, disparo e sincronizacao.

**Nota:** Este documento detalha a implementacao concreta. Para entender os padroes e regras arquiteturais de conectividade, consulte [wifi_manager_architecture.md](wifi_manager_architecture.md).

## Visao geral

A pasta main/connectivity implementa o ciclo completo de conectividade do dispositivo:

1. Inicializa infraestrutura de eventos e LED de status.
2. Decide entre modo STA (com credenciais salvas) ou AP de provisionamento.
3. Mantem estado Wi-Fi e publica eventos de mudanca.
4. Sincroniza portal web e LED conforme estado de conectividade.
5. Expoe rotas HTTP para configurar SSID de provisionamento.

## Papel de cada arquivo

### boot_config_button.c / boot_config_button.h

Detecta se o botao de configuracao foi pressionado no boot para forcar provisionamento Wi-Fi.

- Configura GPIO de entrada com pull adequado.
- Faz debounce por multiplas amostras com intervalo.
- Retorna decisao booleana para o controlador.

Funcao publica:

- boot_config_button_is_pressed

### connectivity_controller.c / connectivity_controller.h

Orquestrador de alto nivel da conectividade.

- Registra handler de eventos do wifi_manager.
- Inicializa LED de status.
- Carrega SSID de provisionamento configurado.
- Decide se inicia forcando provisionamento (botao de boot).
- Inicia wifi_manager com configuracao e sincroniza portal/LED.
- Aplica short-circuit de sincronizacao para evitar updates redundantes quando o estado Wi-Fi nao mudou.

Funcao publica:

- connectivity_controller_start

### device_config_store.c / device_config_store.h

Persistencia da configuracao de SSID de provisionamento (NVS + fallback menuconfig).

- Le SSID da NVS.
- Se nao existir, usa default de menuconfig e tenta persistir.
- Sanitiza entrada (trim e validacao de tamanho/conteudo).
- Persiste e carrega retry maximo de STA (fallback para menuconfig).
- Persiste e carrega politica APSTA e janela de grace para auto-timeout.

Funcoes publicas:

- device_config_store_load_provisioning_ssid
- device_config_store_save_provisioning_ssid
- device_config_store_load_sta_max_retry
- device_config_store_save_sta_max_retry
- device_config_store_load_apsta_policy
- device_config_store_save_apsta_policy

### device_config_routes.c / device_config_routes.h

Rotas HTTP para configuracao de dispositivo (pagina e API JSON).

- Serve pagina /device-config a partir do storage.
- Le/atualiza SSID de provisionamento via /api/device/config.
- Le/atualiza retry maximo de STA via /api/device/config.
- Le/atualiza politica APSTA e janela de grace via /api/device/config.
- aplica hardening de resposta: detalhes internos de erro HTTP podem ficar ocultos por default em builds de release.
- pode ocultar provisioning_ssid no payload de GET por configuracao de build.
- Registra rotas no web portal.

Funcao publica:

- device_config_routes_register_with_portal

### status_led.c / status_led.h

Controle do LED de status (WS2812B via RMT).

- Inicializa canal RMT e encoder para protocolo WS28xx.
- Roda task FreeRTOS dedicada para animacoes/padroes.
- Mapeia estados logicos em cor/padrao visual.

Funcoes publicas:

- status_led_start
- status_led_get_state
- status_led_set_state
- status_led_set_capture_overlay (controle de overlay de pulso durante captura de audio)

Observacao de build:

- quando STATUS_LED_ENABLED estiver desativado, o componente usa implementacao stub (sem backend RMT), mantendo a mesma API publica para facilitar isolamento/remocao na fase final.

### wifi_config.c / wifi_config.h

Persistencia de credenciais STA em NVS.

- Carrega SSID/senha salvos.
- Salva novas credenciais provisionadas.
- Limpa credenciais da NVS quando necessario.
- logs de persistencia evitam imprimir SSID em claro.

Funcoes publicas:

- wifi_config_load
- wifi_config_save
- wifi_config_clear

### wifi_manager.c / wifi_manager.h

Nucleo de conectividade Wi-Fi (estado, transicoes, eventos, scan, reconnect).

- Inicializa stack (netif, event loop e esp_wifi).
- Alterna entre AP, STA e APSTA conforme estado.
- Controla tentativas de reconexao e fallback para AP.
- Valida transicoes de estado via helper central e ignora/loga transicoes invalidas.
- Publica eventos WIFI_MANAGER_EVENT para consumidores.
- Expoe no status o retry efetivo de STA e contadores de reconexao/transicao invalida.
- Aplica politica APSTA apos conectar STA (always_on, auto_timeout, sta_only).
- Oferece API de status, scan e aplicacao de credenciais.
- logs operacionais evitam imprimir SSID em claro durante provisionamento/conexao.

Funcoes publicas:

- wifi_manager_start
- wifi_manager_start_with_config
- wifi_manager_wait_until_ready
- wifi_manager_apply_wifi_credentials
- wifi_manager_reconnect_sta
- wifi_manager_set_sta_max_retry
- wifi_manager_set_apsta_policy
- wifi_manager_set_high_throughput_mode
- wifi_manager_get_status
- wifi_manager_scan_networks

## Fluxo de integracao interna

Fluxo de inicializacao:

1. connectivity_controller_start cria/garante event loop default.
2. Registra wifi_manager_event_handler para WIFI_MANAGER_EVENT.
3. Inicializa status_led.
4. Le SSID de provisionamento (device_config_store).
5. Consulta botao de boot para force_provisioning.
6. Inicia wifi_manager_start_with_config.
7. Sincroniza portal e LED com o estado atual.

Fluxo de estado Wi-Fi:

1. wifi_manager decide STA ou AP no start.
2. Em STA, tenta conectar; ao obter IP, marca estado conectado.
3. Em falhas repetidas de STA, cai para AP de provisionamento.
4. Cada transicao publica evento WIFI_MANAGER_EVENT.
5. connectivity_controller recebe evento e chama:
   - sync_web_portal_with_wifi_state
   - sync_status_led_with_wifi_state
6. se o estado Wi-Fi recebido for igual ao ultimo estado sincronizado, o controller ignora o sync (short-circuit).

Fluxo de provisionamento:

1. Cliente HTTP envia configuracao (rotas de device_config_routes).
2. Dados sao validados e persistidos em device_config_store.
3. Em fluxo Wi-Fi (credenciais), wifi_manager_apply_wifi_credentials persiste em wifi_config e reinicia conexao STA.

## Mecanismos de comunicacao e disparo

Este subsistema tambem e hibrido, combinando chamadas diretas e eventos:

1. Chamadas diretas
- Dominam no caminho de controle (controller -> wifi_manager, controller -> status_led, rotas -> stores).

2. EventGroup FreeRTOS (sinalizacao de prontidao)
- wifi_manager usa bits internos para indicar:
  - conectado (WIFI_CONNECTED_BIT)
  - provisioning ativo (WIFI_PROVISIONING_BIT)
- wifi_manager_wait_until_ready aguarda esses bits com timeout.

3. esp_event (pub/sub de mudanca de estado)
- wifi_manager publica WIFI_MANAGER_EVENT em cada transicao relevante.
- connectivity_controller escuta esses eventos para sincronizar portal e LED.

4. Estado compartilhado
- wifi_manager mantem s_status como snapshot de estado para consultas.
- o acesso ao snapshot de estado no wifi_manager usa secao critica leve para escrita/leitura consistente sob concorrencia.
- status_led mantem estado atual consumido pela task de animacao.

## Maquina de estados (wifi_manager)

Estados principais:

- WIFI_MANAGER_STATE_INIT
- WIFI_MANAGER_STATE_STA_CONNECTING
- WIFI_MANAGER_STATE_STA_CONNECTED
- WIFI_MANAGER_STATE_PROVISIONING_AP

Transicoes tipicas:

- INIT -> STA_CONNECTING (quando ha credenciais validas)
- INIT -> PROVISIONING_AP (sem credenciais ou force_provisioning)
- STA_CONNECTING -> STA_CONNECTED (IP obtido)
- STA_CONNECTING -> PROVISIONING_AP (falhas de reconexao acima do limite)
- PROVISIONING_AP -> STA_CONNECTING (apos aplicar credenciais)

Observacao:

- Durante transicao de provisionamento para conectado, o modulo pode operar em APSTA; depois pode reduzir para STA conforme flags de configuracao.
- Transicoes sao aplicadas por um ponto unico (helper), com log quando uma origem tenta uma mudanca nao permitida.
- Com politica auto_timeout, APSTA permanece por uma janela de grace e depois alterna para STA-only automaticamente.
- Atividade HTTP no portal durante a janela auto_timeout renova a contagem da janela APSTA (idle extension).

## Interfaces externas usadas por main/connectivity (sem internals)

### main/portal

- web_portal_start(...)
  - Inicia portal em modo captive (AP) ou normal (STA conectado).

- web_portal_register_app_routes(...)
  - Registra rotas de configuracao de dispositivo.

- http_helpers_send_file(...)
- http_helpers_send_json(...)
- http_helpers_recv_body(...)
  - Utilitarios HTTP usados pelos handlers de rotas.

### main/app

- storage_file_exists(...)
  - Verifica existencia do HTML de configuracao no storage local.

### ESP-IDF / FreeRTOS

- esp_wifi_*, esp_netif_*, esp_event_*
  - Stack de rede, eventos e operacao Wi-Fi.

- nvs_* 
  - Persistencia de credenciais e configuracao de provisionamento.

- EventGroup e Task APIs
  - Sincronizacao de prontidao e task de LED.

- RMT driver
  - Sinalizacao do WS2812B do LED de status.

## Integracao com o restante do sistema

- Em main/main.c, connectivity_controller_start inicializa o subsistema de conectividade.
- Outros modulos (como o orquestrador ACR) consomem wifi_manager_wait_until_ready e wifi_manager_get_status para condicionar operacoes de rede.
- O portal web central agrega as rotas de conectividade e de aplicacao, mantendo uma interface unica para configuracao.

## Resumo

main/connectivity esta organizado em camadas claras:

- controle/orquestracao: connectivity_controller
- nucleo de estado/rede: wifi_manager
- persistencia: wifi_config e device_config_store
- interface web: device_config_routes
- observabilidade local: status_led
- gatilho de boot: boot_config_button

O desenho privilegia robustez em campo: fallback para AP de provisionamento, sincronizacao por eventos e feedback visual de estado.
