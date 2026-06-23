# Estrutura de components/tele_portal em alto nivel

## Objetivo

`components/tele_portal` agrega a interface HTTP do firmware enquanto a
componentizacao avanca:

- paginas embarcadas via `components/tele_portal_assets`;
- APIs base de status, configuracao, logs, restart e Wi-Fi;
- captive portal HTTP/DNS;
- utilitarios de body, JSON e arquivos via `components/tele_portal_core`;
- buffer circular de logs via `components/tele_portal_logs`.

## Arquivos principais

- `web_portal.c/.h`: agregador temporario de rotas base e extensao por callbacks.
- `captive_portal.c/.h`: rotas de deteccao de captive portal e redirects.
- `dns_server.c/.h`: DNS local usado no modo captive.
- `ota_portal.c/.h`: pagina e APIs de upload OTA.

Componentes extraidos:

- `tele_portal_core`: servidor HTTP, registro de rotas e helpers.
- `tele_portal_assets`: paginas estaticas embarcadas.
- `tele_portal_logs`: captura e endpoint de logs recentes.
- `tele_portal_status`: `/api/status` e manifesto `/api/status/meta`.
- `tele_portal_config`: rotas genericas HTTP sobre `tele_config`.

## Rotas base

Paginas:

- `/`
- `/status`
- `/settings`
- `/networks`
- `/logs`
- `/ota`, registrada por `ota_portal`

APIs:

- `GET /api/status`
- `GET /api/status/meta`
- `GET /api/config/meta`
- `POST /api/config/set`
- `POST /api/config/reset`
- `POST /api/config/apply-reboot`
- `GET /api/logs`
- `POST /api/restart`
- `POST /api/wifi`
- `GET /api/wifi/networks`
- `GET/PUT/DELETE /api/wifi/saved`
- `GET/POST /api/ota/*`, registrada por `ota_portal`

## Contrato de status

`GET /api/status` agrega dados de:

- firmware (`APP_VERSION_STRING`, de `components/tele_system`);
- uptime;
- Wi-Fi e APSTA (`components/tele_wifi`);
- NTP (`components/tele_wifi`);
- power-good (`components/tele_system`);
- VBAT (`components/tele_system`).

O manifesto `GET /api/status/meta` vem de `tele_status` com campos marcados
como `TELE_STATUS_FLAG_WEB`. Estado de dominio futuro deve entrar no registry
`tele_status`, evitando endpoints especificos quando o dado puder ser descrito
por manifesto.

## Fluxo de inicializacao

1. `main` inicializa `tele_portal_logs`.
2. `connectivity_controller` decide se o portal sobe em modo captive ou normal.
3. `web_portal_start` registra rotas base.
4. `captive_portal_register_handlers` adiciona rotas de deteccao.
5. Se `captive_mode=true`, o DNS local e iniciado.
6. Registradores externos adicionam APIs opcionais.

## Manutencao

As paginas embarcadas devem chamar apenas APIs existentes no firmware atual.
Quando uma API de dominio for removida, remova tambem seus fetches dos assets
em `firmware_assets/web`.
