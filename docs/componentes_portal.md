# Componentes Do Portal HTTP

Este grupo implementa o portal web embarcado e APIs HTTP locais.

## Componentes

### `components/tele_portal_core`

Base do servidor HTTP. Responsavel por helpers e registro de rotas. Outros
componentes do portal devem depender dele em vez de manipular diretamente todos
os detalhes do servidor.

### `components/tele_portal_assets`

Arquivos estaticos/embutidos usados pelo portal.

### `components/tele_portal_captive`

Rotas para deteccao de captive portal e redirecionamento durante
provisionamento.

### `components/tele_portal_config`

APIs HTTP para configuracao exposta pelo produto. Deve usar `tele_config` como
fonte de verdade quando possivel.

### `components/tele_portal_status`

APIs HTTP para status. Deve usar `tele_status` quando o dado ja esta no
registry.

### `components/tele_portal_commands`

Adapter HTTP para comandos JSON. Usa `tele_commands` como fonte de verdade e
executa apenas comandos expostos com `TELE_CHANNEL_FLAG_WEB`.

Rotas:

- `GET /api/commands`: manifesto de comandos web;
- `POST /api/commands/execute`: executa `{ "cmd_id": "...", "name": "...",
  "args": {} }` e responde `{ "cmd_id": "...", "ok": true|false, "error":
  "...", "result": {} }`.

O adapter e para comandos administrativos pequenos. Upload OTA por arquivo
continua em `tele_portal_ota`.

### `components/tele_portal_wifi`

Rotas de Wi-Fi:

- salvar credenciais;
- listar redes salvas;
- escanear redes;
- alterar politica AP+STA.

### `components/tele_portal_ota`

Pagina e endpoints de OTA local por upload. O componente e baseado em
callbacks para nao depender diretamente de `firmware_ota`.

Integração atual em `main/main.c`:

```c
static const tele_portal_ota_config_t config = {
    .begin = portal_ota_begin_cb,
    .write = portal_ota_write_cb,
    .finalize = portal_ota_finalize_cb,
    .abort = portal_ota_abort_cb,
    .status = portal_ota_status_cb,
    .restart_delay_ms = 1200,
};

ESP_ERROR_CHECK(tele_portal_ota_init(&config));
ESP_ERROR_CHECK(tele_portal_ota_register_routes());
```

Os callbacks chamam:

- `firmware_ota_upload_begin()`;
- `firmware_ota_upload_write()`;
- `firmware_ota_upload_finalize()`;
- `firmware_ota_upload_abort()`;
- `firmware_ota_get_status()`.

### `components/tele_portal_logs`

Rotas para leitura de logs locais do firmware.

### `components/tele_portal`

Agregador do portal usado pelo app. Mantem o ponto de entrada simples para quem
inicializa o produto.

## Inicializacao No App

Fluxo atual:

```c
tele_portal_logs_init();
ESP_ERROR_CHECK(tele_portal_core_register_routes(tele_portal_commands_register_routes));
ESP_ERROR_CHECK(register_portal_ota_routes());
ESP_ERROR_CHECK(connectivity_controller_start());
```

`connectivity_controller_start()` inicializa Wi-Fi e sincroniza estado do portal
com eventos de conectividade.

## Status OTA No Portal

`/api/ota/status` expõe:

- `state`;
- `in_progress`;
- `restart_pending`;
- `current_version`;
- `target_version`;
- `build_id`;
- `configured_url`;
- `manifest_url`;
- `artifact_url`;
- `last_error`;
- `running_partition`;
- `next_update_partition`;
- `bytes_written`;
- `total_size`;
- `progress_pct`.

Upload manual usa `artifact_url = "upload"` e `manifest_url = ""`.

## Como Adicionar Nova Rota

1. Prefira criar/adaptar um componente `tele_portal_*` focado no dominio.
2. Mantenha a regra de negocio fora do handler HTTP.
3. Exponha callbacks no componente de portal quando o dominio estiver em outro
   componente.
4. Registre a rota no startup do app ou no agregador apropriado.

Handlers HTTP devem validar entrada, chamar API de dominio e responder JSON
curto.

Para comandos JSON, prefira registrar o comando em `tele_commands` com
`TELE_CHANNEL_FLAG_WEB` e usar `tele_portal_commands`, em vez de criar uma rota
nova para cada acao pontual.
