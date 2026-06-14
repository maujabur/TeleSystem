# Estrutura da raiz main em alto nivel

## Objetivo

A raiz `main` conecta os subsistemas do firmware TeleSystem. O projeto atual
mantem conectividade, portal, OTA, VBAT, power-good, LED de status e MQTT de
presenca. O dominio especifico de produto ainda sera adicionado em etapa
posterior.

## Boot atual

`app_main` executa, em alto nivel:

1. inicializa logs da aplicacao;
2. inicializa power-good e VBAT;
3. inicializa NVS;
4. inicializa pilha de rede;
5. registra rotas de configuracao do dispositivo;
6. registra portal OTA;
7. inicia controlador de conectividade;
8. inicia MQTT de presenca, se habilitado.

## Composicao por pastas

### `main/connectivity`

Orquestracao de conectividade especifica da aplicacao:

- controlador de conectividade;
- rotas de configuracao do dispositivo;
- integracao com portal, `components/tele_wifi` e `components/status_led`.

### `components/tele_system`

Componente reutilizavel para apoio de sistema:

- versao de firmware;
- OTA;
- VBAT;
- POWER_GOOD.

### `components/tele_wifi`

Componente reutilizavel para base de rede:

- Wi-Fi STA/AP/APSTA;
- persistencia de credenciais;
- SSID de provisionamento e politicas APSTA;
- botao de configuracao no boot;
- sincronizacao NTP.

### `components/tele_portal`

Interface web e APIs base:

- paginas embarcadas;
- captive portal;
- logs;
- status;
- restart;
- Wi-Fi;
- OTA via registrador externo;
- Kconfig proprio.

### `components/tele_mqtt`

Componente reutilizavel para MQTT:

- cliente MQTT ESP-IDF;
- topicos;
- LWT/availability;
- heartbeat;
- comandos e respostas;
- deduplicacao de comandos mutaveis.

### `components/tele_presence`

Adaptador de presenca MQTT:

- configura `components/tele_mqtt`;
- registra campos comuns em `components/tele_status`;
- injeta estado de Wi-Fi, NTP, VBAT, POWER_GOOD e versao;
- aplica configuracoes remotas de conectividade;
- reserva os callbacks de comando para o dominio TeleSystem.

### `components/tele_status`

Registro reutilizavel de status read-only:

- campos observaveis com tipo, unidade opcional, flags e callback de leitura;
- sem NVS e sem persistencia;
- fonte comum para `state` e `heartbeat` MQTT;
- base futura para `meta/status`.

### `components/status_led`

Componente reutilizavel para LED WS28xx:

- API de estados;
- Kconfig proprio;
- driver RMT real ou stub quando desabilitado.

## Direcao de evolucao

Adicionar o modulo TeleSystem de dominio como camada separada, consumindo:

- conectividade;
- MQTT via callbacks;
- status LED;
- APIs do portal via registrador, se necessario.

Evite recolocar logica de produto dentro de `web_portal`, `wifi_manager` ou
`tele_mqtt`.
