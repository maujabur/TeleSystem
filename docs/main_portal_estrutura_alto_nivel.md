# Estrutura de main/portal em alto nivel

## Objetivo

Este documento descreve o subsistema web em main/portal:

- funcao de cada arquivo;
- funcoes publicas e responsabilidade de cada modulo;
- fluxo interno de requisicoes e inicializacao;
- integracao com modulos fora de main/portal (somente nivel de interface);
- mecanismos de comunicacao e sincronizacao.

**Nota:** Este documento detalha o portal web core. O modulo ACR registra rotas adicionais via callback (incluindo /api/acr/status para telemetria propria); consulte [main_app_estrutura_alto_nivel.md](main_app_estrutura_alto_nivel.md) para detalhes.

## Visao geral

A pasta main/portal implementa a camada de interface web do firmware:

1. servidor HTTP principal com rotas base de UI e APIs;
2. suporte a captive portal (redirect HTTP + DNS local);
3. utilitarios de HTTP para JSON, body e arquivos;
4. buffer circular de logs para consulta pela interface.

Essa camada nao executa regras de negocio pesadas; ela agrega estado de outros modulos e expoe isso via HTTP.

## Papel de cada arquivo

### web_portal.c / web_portal.h

Orquestrador do servidor web.

- inicia e para o servidor HTTP;
- registra rotas base do sistema;
- habilita/desabilita captive portal conforme modo;
- permite extensao por callbacks de registro de rotas de outros modulos.

Funcoes publicas:

- web_portal_register_app_routes
- web_portal_start
- web_portal_stop
- web_portal_is_running

### captive_portal.c / captive_portal.h

Camada de captive portal HTTP.

- registra endpoints conhecidos de deteccao de captive portal;
- responde com redirect para a raiz do portal;
- habilita/desabilita DNS local de captura.

Funcoes publicas:

- captive_portal_register_handlers
- captive_portal_set_enabled
- captive_portal_stop

### dns_server.c / dns_server.h

Servidor DNS simplificado em task FreeRTOS.

- abre socket UDP na porta 53;
- processa consultas DNS em loop;
- responde apontando para IP local do portal quando aplicavel;
- fornece start/stop controlado para o captive mode.

Funcoes publicas:

- dns_server_start
- dns_server_stop

### http_helpers.c / http_helpers.h

Utilitarios HTTP reutilizaveis para handlers.

- envio de JSON com status code;
- leitura segura de body de requisicao;
- envio de arquivo do storage com tipo de conteudo.

Funcoes publicas:

- http_helpers_send_json
- http_helpers_recv_body
- http_helpers_send_file

### app_log_buffer.c / app_log_buffer.h

Buffer circular de logs da aplicacao.

- intercepta saida de log;
- armazena janela recente em memoria;
- retorna snapshot para endpoint de logs.

Funcoes publicas:

- app_log_buffer_init
- app_log_buffer_get_snapshot

## Rotas base registradas pelo portal

O modulo web_portal registra rotas principais, incluindo:

- paginas: /, /logs, /status, /settings
- APIs: /api/status (dados de connectivity: Wi-Fi, uptime, versao), /api/logs, /api/restart, /api/wifi, /api/wifi/networks
- rotas adicionais: vindas de callbacks registrados por outros modulos (exemplo: /api/acr/status registrada por main/app)

No modo captive, captive_portal adiciona rotas de deteccao de SOs e handler de not found para redirecionamento.

## Fluxo de inicializacao

1. main chama app_log_buffer_init para habilitar captura de logs.
2. connectivity_controller decide quando iniciar portal e chama web_portal_start(captive_mode).
3. web_portal_start sobe servidor HTTP e registra rotas base.
4. captive_portal_register_handlers registra handlers de redirect.
5. se captive_mode=true, captive_portal_set_enabled inicia dns_server.
6. callbacks de app routes (registrados antes) sao executados para incluir rotas extras.

## Ciclo de requisicao (alto nivel)

Exemplo de endpoint JSON - /api/status (dados de connectivity apenas):

1. request chega ao handler HTTP em web_portal.
2. handler consulta wifi_manager_get_status().
3. monta objeto JSON com dados de Wi-Fi, uptime e versao.
4. responde via http_helpers_send_json.

Exemplo de endpoint JSON - /api/acr/status (dados de modulo ACR, registrado via callback):

1. handler registrado por acr_routes (em main/app) recebe request.
2. handler consulta acr_runtime_status_get() e acr_config_store para telemetria de ACR.
3. monta objeto JSON separado com estado/metricas de ACRCloud.
4. responde via http_helpers_send_json.

Exemplo de pagina HTML:

1. handler verifica existencia de arquivo no storage.
2. envia arquivo por chunks com http_helpers_send_file.

## Integracao com outras pastas (interfaces)

### main/connectivity

- wifi_manager_get_status
- wifi_manager_scan_networks
- wifi_manager_apply_wifi_credentials

Uso:

- leitura de estado de conectividade;
- scan de redes para UI;
- aplicacao de credenciais recebidas por API.

### main/app

**Nota:** modulo ACR registra suas rotas HTTP via callback (web_portal_register_app_routes) em tempo de inicializacao.

Rotas registradas por acr_routes:

- GET /api/acr/status: telemetria de ACRCloud (estado, metricas, resultados)
- POST /api/acr: atualizacoes de config
- POST /api/acr/run: acionamento manual
- GET/POST /api/acr/control: controle de modo de operacao
- GET/POST /api/trigger-output: controle de saida GPIO
- POST /api/trigger-output/test: teste de pulso

Interfaces consultadas por essas rotas:

- acr_runtime_status_get: estado/telemetria de ACR
- acr_config_store: configuracoes persistidas
- acr_trigger_output_get_status: status de GPIO
- storage_file_exists: validacao de arquivos

Uso:

- expor /api/acr/status como endpoint separado (separacao de responsabilidades);
- portal core nao conhece detalhes internos de ACR;
- modulos registram suas rotas e dados quando inicializam.

### versao de firmware

- APP_VERSION_STRING

Uso:

- incluir versao no payload de status para UI/diagnostico.

### ESP-IDF e bibliotecas

- esp_http_server: infraestrutura HTTP e registro de handlers
- esp_system: restart do dispositivo
- cJSON: parse e serializacao JSON
- FreeRTOS task/sync: task do DNS e sincronizacao de buffer de logs
- sockets/lwIP: implementacao do servidor DNS UDP

## Mecanismos de comunicacao e sincronizacao

O portal usa principalmente modelo request/response sincrono:

1. chamadas diretas
- handlers HTTP chamam diretamente APIs dos modulos de dominio para coletar estado/aplicar comando.

2. task dedicada
- dns_server roda em task propria para nao bloquear o servidor HTTP.

3. estado global controlado
- web_portal mantem handle global do servidor e lista de callbacks de rotas.

4. sincronizacao de logs
- app_log_buffer protege buffer circular com secao critica/spinlock.

Nao ha orquestracao por fila de mensagens nesse modulo; a maior parte e reativa a requisicoes HTTP/DNS.

## Integracao geral do sistema

- connectivity_controller controla quando o portal sobe em modo captive ou normal.
- modulos de app/connectivity registram rotas adicionais pelo mecanismo de callback.
- o portal atua como fachada de observabilidade e configuracao do dispositivo.

## Resumo

main/portal esta organizado em blocos coesos:

- orquestracao HTTP: web_portal
- captive behavior: captive_portal + dns_server
- utilitarios de transporte: http_helpers
- observabilidade local: app_log_buffer

A arquitetura favorece extensibilidade (registro de rotas por callback) e separacao clara entre transporte web e regras de negocio dos demais subsistemas.
