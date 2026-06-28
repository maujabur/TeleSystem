# Componentes Do Portal HTTP

Este grupo implementa o portal web embarcado e APIs HTTP locais.

## Componentes

### `components/tele_portal_core`

Base do servidor HTTP. Responsavel por helpers e registro de rotas. Outros
componentes do portal devem depender dele em vez de manipular diretamente todos
os detalhes do servidor.

### `components/tele_portal_assets`

Arquivos estaticos/embutidos usados pelo portal.

O componente le os fontes em `firmware_assets/web`. Durante o build, seu
`CMakeLists.txt` chama `components/tele_portal_assets/tools/gen_assets.py` para
gerar `tele_portal_assets_generated.c` e `tele_portal_assets_generated.h` com o
mapa de URI, content-type e flags dos assets. Os arquivos continuam sendo
embutidos via `EMBED_TXTFILES`, mas a tabela servida em runtime nasce desse
script gerador, nao do agregador `tele_portal`.

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

### `components/tele_firmware_portal_ota`

Binding fino entre `tele_portal_ota` e `firmware_ota`. Ele configura os
callbacks de upload/status e deixa o agregador `tele_portal` registrar as rotas
OTA junto com as demais rotas especificas, antes dos assets wildcard.

O componente existe para que `tele_portal_ota` continue reaproveitavel com
outro backend de update. Em outro sistema embarcado, substitua este binding por
um equivalente que implemente os mesmos callbacks.

Uso direto, quando o app nao usa o agregador `tele_portal`:

```c
ESP_ERROR_CHECK(tele_firmware_portal_ota_register_routes());
```

Internamente o binding chama:

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
ESP_ERROR_CHECK(connectivity_controller_start());
```

O agregador `tele_portal` registra o OTA local automaticamente via
`tele_firmware_portal_ota`.

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

## Usando O Portal Em Outro Projeto

Para reaproveitar o portal em outro firmware, escolha primeiro entre usar o
agregador padrao ou montar o servidor com componentes avulsos.

### Opção A: agregador `tele_portal`

Use quando o projeto quer o portal completo: assets, captive portal, status,
config, Wi-Fi, logs e upload OTA local.

Componentes principais:

```text
components/tele_portal
components/tele_portal_assets
components/tele_portal_captive
components/tele_portal_config
components/tele_portal_core
components/tele_portal_logs
components/tele_portal_ota
components/tele_portal_status
components/tele_portal_wifi
components/tele_firmware_portal_ota
components/tele_config
components/tele_status
components/tele_commands
components/tele_wifi
components/tele_system
firmware_assets/web
```

Ao copiar o portal completo para outro projeto, mantenha tambem
`components/tele_portal_assets/tools/gen_assets.py`; ele e chamado pelo
`CMakeLists.txt` de `tele_portal_assets` para construir a tabela de assets.

Inicializacao minima:

```c
#include "firmware_ota.h"
#include "tele_portal_commands.h"
#include "tele_portal_core.h"
#include "tele_portal_logs.h"
#include "web_portal.h"

ESP_ERROR_CHECK(firmware_ota_init());
tele_portal_logs_init();
ESP_ERROR_CHECK(tele_portal_core_register_routes(tele_portal_commands_register_routes));
ESP_ERROR_CHECK(web_portal_start(false));
```

O agregador registra as rotas internas em ordem segura: rotas especificas
primeiro, captive portal depois, assets wildcard por ultimo. O OTA local entra
por `tele_firmware_portal_ota`, que conecta `tele_portal_ota` ao servico
`firmware_ota`.

### Opção B: portal por componentes avulsos

Use quando o projeto quer somente o servidor HTTP e algumas rotas, ou quando
nao quer depender de `tele_wifi`/`firmware_ota`.

Componentes minimos:

```text
components/tele_portal_core
components/tele_portal_assets
components/tele_portal_ota      # somente se quiser upload OTA
firmware_assets/web             # se usar assets embutidos
```

Se usar assets embutidos, o build precisa incluir o script
`components/tele_portal_assets/tools/gen_assets.py`, pois ele gera o header e o
source consumidos por `tele_portal_assets.c`.

Nesse modo, o app inicializa `tele_portal_core`, registra rotas com
`tele_portal_core_register_routes()` e chama `tele_portal_core_start()`. Para
OTA com outro backend, configure `tele_portal_ota` diretamente:

```c
static const tele_portal_ota_config_t ota_config = {
    .begin = product_ota_begin,
    .write = product_ota_write,
    .finalize = product_ota_finalize,
    .abort = product_ota_abort,
    .status = product_ota_status,
    .restart_delay_ms = 1200,
};

ESP_ERROR_CHECK(tele_portal_ota_init(&ota_config));
ESP_ERROR_CHECK(tele_portal_ota_register_routes());
```

Assim `tele_portal_ota` continua sendo apenas transporte HTTP. A escrita em
flash, validacao de imagem, troca de particao e reboot ficam no backend do
produto.

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
