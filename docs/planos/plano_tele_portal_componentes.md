# Plano de Componentizacao do Portal Web

## Objetivo

Transformar o portal web atual em um conjunto de componentes reaproveitaveis,
com um nucleo HTTP generico e adaptadores opcionais para Wi-Fi, OTA,
configuracao, status e assets. O portal deve poder ser usado em outro projeto
ESP-IDF sem carregar dependencias de produto que esse projeto nao usa.

## Diagnostico Atual

O componente `components/tele_portal` ja concentra a funcionalidade do portal,
mas ainda nao e um componente generico. Hoje ele:

- embute diretamente arquivos de `firmware_assets/web`;
- depende de `tele_system`, `tele_wifi` e `espressif__cjson`;
- mistura servidor HTTP, captive portal, assets, logs, status tecnico, Wi-Fi,
  OTA e restart;
- chama funcoes de produto diretamente, como `wifi_manager_note_portal_activity`;
- publica endpoints que conhecem detalhes de bateria, horario, firmware,
  politica AP+STA e identificadores de rede.

A direcao correta e manter o portal como familia de componentes, nao como um
unico bloco monolitico.

## Arquitetura Alvo

### 1. `tele_portal_core`

Nucleo generico do portal.

Responsabilidades:

- iniciar e parar servidor HTTP;
- registrar rotas de aplicacao;
- controlar limite de sockets, timeouts e ciclo de vida do servidor;
- fornecer helpers HTTP comuns;
- permitir callbacks de atividade, restart e status basico;
- opcionalmente hospedar o captive portal DNS sem conhecer `tele_wifi`.

Nao deve depender de:

- `tele_wifi`;
- `tele_system`;
- firmware OTA;
- assets especificos do produto;
- nomes TeleSystem no HTML.

API proposta:

```c
typedef esp_err_t (*tele_portal_routes_register_fn)(httpd_handle_t server);
typedef void (*tele_portal_activity_cb_t)(void *ctx);
typedef void (*tele_portal_restart_cb_t)(uint32_t delay_ms, void *ctx);

typedef struct {
    size_t max_uri_handlers;
    size_t max_open_sockets;
    uint32_t socket_timeout_s;
    tele_portal_activity_cb_t on_activity;
    tele_portal_restart_cb_t restart;
    void *ctx;
} tele_portal_core_config_t;

esp_err_t tele_portal_core_init(const tele_portal_core_config_t *config);
esp_err_t tele_portal_core_register_routes(tele_portal_routes_register_fn fn);
esp_err_t tele_portal_core_start(bool captive_mode);
esp_err_t tele_portal_core_stop(void);
bool tele_portal_core_is_running(void);
void tele_portal_core_note_activity(void);
```

### 2. `tele_portal_assets`

Camada opcional para servir paginas estaticas.

Responsabilidades:

- registrar rotas para HTML/CSS/JS embutidos;
- permitir que o projeto consumidor escolha quais paginas embutir;
- manter a home, status, settings, networks e logs fora do core.

Estrategia inicial:

- manter os assets atuais em `firmware_assets/web`;
- criar um adaptador que registra esses assets no core;
- em etapa posterior, aceitar uma tabela de assets fornecida pelo projeto.

API proposta:

```c
typedef struct {
    const char *uri;
    const unsigned char *start;
    const unsigned char *end;
    const char *content_type;
    bool captive_root;
} tele_portal_asset_t;

esp_err_t tele_portal_assets_register(const tele_portal_asset_t *assets,
                                      size_t asset_count);
```

### 3. `tele_portal_captive`

Adaptador opcional para captive portal.

Responsabilidades:

- registrar handlers de redirect/captive check;
- iniciar/parar DNS server;
- chamar callback de atividade via core;
- nao conhecer diretamente o Wi-Fi manager.

Dependencias:

- `tele_portal_core`;
- `lwip`;
- `esp_http_server`.

### 4. `tele_portal_wifi`

Adaptador opcional para configuracao e status Wi-Fi.

Responsabilidades:

- rotas de credenciais Wi-Fi;
- rotas de redes salvas;
- rotas de scan;
- rotas de politica AP+STA;
- integrar `tele_wifi` com `tele_config`, quando aplicavel.

Dependencias:

- `tele_portal_core`;
- `tele_wifi`;
- `tele_config`;
- `cJSON`.

Este adaptador substitui a parte de Wi-Fi que hoje ainda esta no agregador
`web_portal.c`.

### 5. `tele_portal_ota`

Adaptador opcional para upload OTA via portal.

Responsabilidades:

- pagina/endpoint de upload de firmware;
- endpoint de status OTA;
- finalize/restart controlado por callback.

Dependencias:

- `tele_portal_core`;
- componente de OTA do projeto, que deve ser isolado como `tele_ota` antes de
  virar dependencia publicavel.

Observacao: enquanto `firmware_ota` continuar em `main/app`, este adaptador
deve ficar especifico do projeto TeleSystem, ou receber callbacks para
`begin/write/finalize/status`.

### 6. `tele_portal_config`

Adaptador opcional para expor `tele_config` por HTTP.

Responsabilidades:

- `GET /api/config/meta`;
- `POST /api/config/set`;
- `POST /api/config/reset`;
- `POST /api/config/apply-reboot`, se fizer sentido;
- mesma semantica usada no MQTT control center: default, NVS, runtime apply,
  reboot required, read-only e secret.

Dependencias:

- `tele_portal_core`;
- `tele_config`;
- `cJSON`.

### 7. `tele_portal_status`

Adaptador opcional para expor `tele_status` por HTTP.

Responsabilidades:

- `GET /api/status`;
- `GET /api/status/meta`;
- grupos de status por manifest;
- suporte a status tecnico via campos registrados, evitando endpoints
  especificos hardcoded.

Dependencias:

- `tele_portal_core`;
- `tele_status`;
- `cJSON`.

### 8. `tele_portal_logs`

Adaptador opcional para logs em memoria.

Responsabilidades:

- buffer circular de logs;
- endpoint `GET /api/logs`;
- pagina opcional de logs.

Dependencias:

- `tele_portal_core`;
- `esp_log`.

## Ordem Recomendada de Implementacao

### Fase 1: Extrair Core Sem Mudar Comportamento

Criar `components/tele_portal_core` com:

- `web_portal_start`;
- `web_portal_stop`;
- `web_portal_is_running`;
- registro de rotas;
- `http_helpers`;
- callback `on_activity`;
- callback `restart`.

Manter wrappers compativeis no `tele_portal` atual durante a migracao interna.
Como estamos em desenvolvimento e sem compromisso de legado externo, os wrappers
podem ser removidos assim que o app principal for migrado.

Resultado esperado:

- firmware principal compila;
- portal abre como antes;
- nenhuma rota de Wi-Fi/OTA/status muda ainda;
- core nao depende de `tele_wifi` nem `tele_system`.

### Fase 2: Separar Assets e Logs

Criar:

- `tele_portal_assets`;
- `tele_portal_logs`.

Mover:

- buffer circular de logs para `tele_portal_logs.c`;
- handlers de pagina estatica para `tele_portal_assets.c`;
- endpoint de logs para `tele_portal_logs.c`.

Resultado esperado:

- assets continuam os mesmos;
- logs continuam opcionais por Kconfig;
- o core nao contem HTML embutido.

### Fase 3: Extrair Status e Config Genericos

Criar:

- `tele_portal_status`;
- `tele_portal_config`.

Mover endpoints que devem conversar com:

- `tele_status`;
- `tele_config`.

Evitar duplicar regras ja existentes no MQTT. O HTTP deve ser outro adaptador
para os mesmos contratos, nao um segundo sistema de configuracao.

Resultado esperado:

- settings web passa a usar `tele_config`;
- status web passa a usar `tele_status`;
- status tecnico hardcoded diminui ou desaparece.

### Fase 4: Extrair Wi-Fi

Criar `tele_portal_wifi`.

Mover:

- rotas de credenciais;
- rotas de scan;
- rotas de redes salvas;
- rotas de AP+STA;
- notificacao de atividade para callback do core.

Resultado esperado:

- projetos sem Wi-Fi podem usar `tele_portal_core`;
- projetos com Wi-Fi adicionam `tele_portal_wifi`;
- `tele_portal_core` continua sem dependencia de `tele_wifi`.

### Fase 5: Extrair OTA

Escolher entre duas estrategias:

1. criar primeiro `tele_ota` e depois `tele_portal_ota`;
2. criar `tele_portal_ota` baseado em callbacks.

Recomendacao: usar callbacks primeiro, porque isso evita prender o portal a uma
implementacao OTA unica.

API proposta:

```c
typedef esp_err_t (*tele_portal_ota_begin_cb_t)(void *ctx);
typedef esp_err_t (*tele_portal_ota_write_cb_t)(const void *data,
                                                size_t len,
                                                void *ctx);
typedef esp_err_t (*tele_portal_ota_finalize_cb_t)(void *ctx);
typedef void (*tele_portal_ota_abort_cb_t)(void *ctx);
typedef esp_err_t (*tele_portal_ota_status_cb_t)(cJSON *json, void *ctx);
```

Resultado esperado:

- OTA web continua funcional no TeleSystem;
- outro projeto pode usar o adaptador com sua propria implementacao OTA;
- `tele_portal_ota` nao depende diretamente de `main/app/firmware_ota`.

## Estrutura de Componentes Pretendida

```text
components/
  tele_portal_core/
    CMakeLists.txt
    idf_component.yml
    include/tele_portal_core.h
    tele_portal_core.c
    http_helpers.c
    include/http_helpers.h

  tele_portal_assets/
    CMakeLists.txt
    idf_component.yml
    include/tele_portal_assets.h
    tele_portal_assets.c

  tele_portal_captive/
    CMakeLists.txt
    idf_component.yml
    include/tele_portal_captive.h
    tele_portal_captive.c
    dns_server.c
    include/dns_server.h

  tele_portal_logs/
    CMakeLists.txt
    idf_component.yml
    include/tele_portal_logs.h
    tele_portal_logs.c

  tele_portal_config/
    CMakeLists.txt
    idf_component.yml
    include/tele_portal_config.h
    tele_portal_config.c

  tele_portal_status/
    CMakeLists.txt
    idf_component.yml
    include/tele_portal_status.h
    tele_portal_status.c

  tele_portal_wifi/
    CMakeLists.txt
    idf_component.yml
    include/tele_portal_wifi.h
    tele_portal_wifi.c

  tele_portal_ota/
    CMakeLists.txt
    idf_component.yml
    include/tele_portal_ota.h
    tele_portal_ota.c
```

O componente `tele_portal` atual pode existir temporariamente como agregador
local do TeleSystem, mas a meta e remove-lo ou transforma-lo em um pacote de
compatibilidade interno enquanto os consumidores passam a depender dos
adaptadores especificos.

## Kconfig

Mover opcoes globais para os componentes correspondentes:

- `CONFIG_TELE_PORTAL_LOGS_ENABLE_ENDPOINT` -> `tele_portal_logs`;
- `CONFIG_WEB_PORTAL_EXPOSE_NETWORK_IDENTIFIERS` -> `tele_portal_wifi` ou
  `tele_portal_status`;
- `CONFIG_WEB_PORTAL_DETAILED_HTTP_ERRORS` -> `tele_portal_core`;
- limites de socket/handler/timeout -> `tele_portal_core`;
- captive portal DNS -> `tele_portal_captive`;
- OTA upload -> `tele_portal_ota`.

## Testes e Validacao

### Builds

Rodar:

```bash
CCACHE_DIR=/tmp/telesystem-ccache idf.py build
CCACHE_DIR=/tmp/telesystem-example-min-ccache idf.py -C examples/component_consumer_minimal build
CCACHE_DIR=/tmp/telesystem-example-mqtt-ccache idf.py -C examples/component_consumer_mqtt build
```

### Exemplo Novo

Adicionar um terceiro exemplo:

```text
examples/component_consumer_portal/
```

Este exemplo deve:

- iniciar `tele_portal_core`;
- registrar uma rota simples `GET /api/ping`;
- opcionalmente servir uma pagina estatica minima;
- nao depender de `tele_wifi`;
- nao depender de `tele_mqtt`;
- nao depender de OTA.

### Validacao Manual no TeleSystem

Depois de cada fase:

- abrir portal normal;
- abrir portal captive;
- salvar configuracao Wi-Fi;
- listar redes;
- abrir status;
- abrir settings;
- abrir logs quando habilitado;
- testar upload OTA apenas na fase OTA.

## Riscos

- A extracao pode quebrar simbolos de assets embutidos se muitos arquivos forem
  movidos de uma vez.
- O portal atual mistura status generico e status especifico do produto; mover
  tudo para `tele_status` deve ser gradual.
- OTA ainda esta acoplado a `firmware_ota`; extrair por callbacks reduz o risco.
- O Component Manager pode exigir ajuste de `idf_component.yml` para dependencias
  locais e externas, como ja aconteceu com `tele_config` e `tele_mqtt`.

## Proximo Passo Recomendado

Fase 7 executada:

- manifests dos componentes `tele_portal_*` foram padronizados;
- `override_path` foi removido dos manifests publicaveis dos componentes;
- dependencias externas foram mantidas nos manifests; dependencias internas
  seguem no `CMakeLists.txt` e nos exemplos consumidores para evitar buscas no
  registry durante builds locais offline;
- `tele_system` e `tele_wifi` receberam manifests proprios porque sao
  dependencias de adaptadores de portal;
- `docs/estrategia_component_manager.md` foi atualizado com consumo do portal
  por Component Manager;
- os exemplos consumidores validam a resolucao local desses manifests.

Fase 8 executada como verificacao da Fase 7:

- compilado firmware principal;
- compilado `examples/component_consumer_minimal`;
- compilado `examples/component_consumer_mqtt`;
- compilado `examples/component_consumer_portal`;
- ajustado lock gerado pelo Component Manager.

O proximo passo recomendado e **Fase 9: validar consumo externo limpo e preparar
tag candidata**:

Fase 9 parcialmente executada:

- criado consumidor externo minimo em `/tmp/telesystem_consumer_test`;
- validado consumo offline/local de `tele_config`, `tele_status` e
  `tele_commands` fora do repositorio principal;
- confirmado que, sem rede e sem `dependencies.lock`, o Component Manager tenta
  consultar o registry para resolver `espressif/cjson`;
- documentado em `docs/estrategia_component_manager.md` o fluxo offline com
  `EXTRA_COMPONENT_DIRS` + `IDF_COMPONENT_MANAGER=0`.

O proximo passo recomendado e **Fase 9B: validar consumidor externo com Wi-Fi e
portal real**:

1. criar consumidor externo com `tele_wifi`, `tele_portal_core` e
   `tele_portal_logs`;
2. validar boot, provisionamento/rede e acesso pelo navegador;
3. decidir se `tele_wifi` esta generico o suficiente ou se precisa separar
   inicializacao Wi-Fi, store de configuracao e NTP;
4. depois disso, testar consumo `git/path/version` com uma tag/lock real e
   preparar uma tag candidata `lib-v0.1.0`.

Historico da recomendacao anterior:

Fase 6 executada:

- criado `examples/component_consumer_portal`;
- o exemplo consome `tele_portal_core` e `tele_portal_logs`;
- registra a rota propria `GET /api/ping`;
- nao depende de `tele_wifi`, `tele_mqtt`, `tele_system`, `tele_status`,
  `tele_config` ou OTA;
- serve como prova local de consumo dos componentes de portal fora do firmware
  TeleSystem.

O proximo passo recomendado naquele momento era **Fase 7: preparar consumo externo por Component
Manager**:

1. revisar `idf_component.yml` dos componentes de portal extraidos;
2. declarar dependencias entre componentes onde ainda estiverem implicitas;
3. padronizar `description`, `license`, `repository`, `url` e `version`;
4. documentar blocos `git/path/version` para consumidores externos;
5. compilar os tres exemplos consumidores apos os ajustes.

Historico da recomendacao anterior:

Fase 5B executada em `0.3.31 TeleSystem portal OTA adapter`:

- criado `components/tele_portal_ota`;
- movidos pagina `/ota` e endpoints `/api/ota/status` e `/api/ota/upload`;
- removidos `ota_portal.c/.h` de `components/tele_portal`;
- `tele_portal_ota` recebe callbacks de `begin/write/finalize/abort/status`;
- o TeleSystem conecta esses callbacks em `firmware_ota` no `main`;
- `components/tele_portal` deixou de depender de `tele_system` e de `cJSON`.

O proximo passo recomendado naquele momento era criar **Fase 6: exemplo consumidor do portal**:

1. adicionar `examples/component_consumer_portal`;
2. consumir `tele_portal_core` e um conjunto pequeno de adaptadores;
3. registrar uma rota propria simples, como `GET /api/ping`;
4. compilar o exemplo sem `tele_wifi`, sem `tele_mqtt` e sem `tele_system`;
5. usar esse exemplo para validar a estrategia de Component Manager.

Historico da recomendacao anterior:

Fase 5A executada em `0.3.30 TeleSystem portal captive adapter`:

- criado `components/tele_portal_captive`;
- movidas rotas de deteccao de captive portal para
  `tele_portal_captive_register_handlers`;
- movido DNS captive para o novo componente;
- `components/tele_portal` deixou de conter `captive_portal.c` e
  `dns_server.c`;
- `components/tele_portal` segue como agregador temporario para assets, logs,
  status, config, Wi-Fi, restart e OTA.

O proximo passo recomendado naquele momento era executar **Fase 5B (`tele_portal_ota`)**:

1. criar `components/tele_portal_ota`;
2. expor uma API baseada em callbacks para `begin/write/finalize/abort/status`;
3. mover `ota_portal.c/.h` para esse componente;
4. adaptar o TeleSystem para conectar esses callbacks em `firmware_ota`;
5. manter `/ota` e `/api/ota/*` funcionando como hoje.

Historico da recomendacao anterior:

Fase 4 executada em `0.3.29 TeleSystem portal wifi adapter`:

- criado `components/tele_portal_wifi`;
- movidas rotas `POST /api/wifi`, `GET /api/wifi/networks` e
  `GET/PUT/DELETE /api/wifi/saved`;
- movida a notificacao de atividade do portal para callback exposta por
  `tele_portal_wifi`;
- `components/tele_portal` deixou de depender diretamente de `tele_wifi`;
- `components/tele_portal` segue como agregador temporario para assets, logs,
  status, config, Wi-Fi, restart, captive portal e OTA.

O proximo passo recomendado naquele momento era decidir entre duas fatias:

1. **Fase 5A: `tele_portal_captive`** - extrair `captive_portal.c`,
   `dns_server.c` e Kconfig futuro de DNS captive.
2. **Fase 5B: `tele_portal_ota`** - extrair OTA web por callbacks, reduzindo o
   acoplamento com `firmware_ota`.

Minha recomendacao naquele momento foi fazer primeiro **Fase 5A (`tele_portal_captive`)**, porque
ela e menor e termina de tirar mais infraestrutura HTTP do agregador antes do
recorte OTA.

Historico da recomendacao anterior:

Fatia pos-Fase 3 executada em `0.3.28 TeleSystem settings config manifest`:

- `firmware_assets/web/settings.html` passou a ler `GET /api/config/meta`;
- salvamento de conectividade passou a usar `POST /api/config/set`;
- reset de defaults passou a usar `POST /api/config/reset`;
- `main/connectivity/device_config_routes.c/.h` foi removido do build;
- `device_config_routes_register_with_portal()` foi removido de `main`;
- `device_config_store` agora registra callbacks runtime de `tele_config` para
  SSID de provisionamento, retry STA e politica APSTA;
- a pagina de settings continua visualmente equivalente, mas dirigida por
  manifesto.

O passo recomendado naquele momento era executar a Fase 4:

1. criar `components/tele_portal_wifi`;
2. mover rotas `/api/wifi`, `/api/wifi/networks` e `/api/wifi/saved`;
3. mover o callback de atividade Wi-Fi para o adaptador;
4. deixar `components/tele_portal` ainda menor como agregador temporario;
5. compilar e validar portal normal/captive quando houver hardware.

Historico da recomendacao anterior para Fase 3:

Fase 3 executada em `0.3.27 TeleSystem portal status config adapters`:

- criado `components/tele_portal_status`;
- criado `components/tele_portal_config`;
- `GET /api/status` foi movido para `tele_portal_status`;
- `GET /api/status/meta` expõe manifesto de `tele_status` filtrado por
  `TELE_STATUS_FLAG_WEB`;
- `GET /api/config/meta`, `POST /api/config/set`, `POST /api/config/reset` e
  `POST /api/config/apply-reboot` expõem `tele_config` por HTTP;
- campos comuns de status passaram a marcar tambem `TELE_STATUS_FLAG_WEB`;
- `components/tele_portal` continua como agregador temporario;
- a rota antiga `GET/POST /api/device/config` permaneceu temporariamente para
  compatibilidade com `settings.html`;
- `idf.py build` passou gerando `build/TeleSystem.bin`.

Historico da recomendacao anterior para Fase 2:

Fase 2 executada em `0.3.26 TeleSystem portal assets logs extraction`:

- criado `components/tele_portal_assets`;
- criado `components/tele_portal_logs`;
- movidas paginas estaticas para `tele_portal_assets`;
- movido buffer circular de logs e endpoint `/api/logs` para
  `tele_portal_logs`;
- `components/tele_portal` continua como agregador temporario;
- os Kconfigs de logs agora vivem em `tele_portal_logs`;
- `idf.py build` passou gerando `build/TeleSystem.bin`.

Historico da recomendacao anterior para Fase 1:

Fase 1 executada em `0.3.25 TeleSystem portal core extraction`:

- criado `components/tele_portal_core`;
- movidos ciclo de vida do servidor HTTP, registro de rotas e helpers HTTP;
- `components/tele_portal` ficou como agregador temporario;
- `captive_portal` passou a notificar atividade via callback do core;
- `main/idf_component.yml` e `idf_component.yml` usam `override_path` local para
  `cJSON` e `mqtt`, permitindo build sem consultar o registry em desenvolvimento;
- `idf.py build` passou gerando `build/TeleSystem.bin`.

Historico da recomendacao original para Fase 1:

1. criar `tele_portal_core`;
2. mover servidor HTTP, registro de rotas e helpers;
3. manter `tele_portal` atual como agregador temporario;
4. compilar firmware principal;
5. testar portal sem alterar UI nem endpoints.

Depois disso, seguir para assets/logs. Esta ordem preserva o comportamento
visivel e limpa a fronteira mais importante primeiro.
