# Componentes De Manifest E Artefatos

Este grupo implementa updates por manifest de forma generica. O sistema atual
registra dois artefatos:

- `ca_bundle`, aplicado como arquivo validado no `tele_ca_store`;
- `firmware`, aplicado em streaming direto na particao OTA.

Novos tipos devem registrar um handler em `tele_artifacts`, escolhendo se a
aplicacao usa arquivo local ou streaming.

## Componentes

### `components/tele_manifest`

Motor de manifest, download e verificacao.

Responsavel por:

- baixar manifests pequenos por HTTPS;
- validar schema JSON;
- normalizar `url` e `urls`;
- validar `artifact_type`, `channel`, `version`, `sha256`, `size` e HTTPS;
- baixar artefato em modo arquivo ou streaming;
- calcular SHA-256 durante o download;
- chamar callbacks de progresso e aplicacao.

Use:

- `tele_manifest_fetch()` para consultar o manifest;
- `tele_manifest_run_file()` para artefatos que devem ser baixados para um
  arquivo temporario antes da aplicacao;
- `tele_manifest_run_stream()` para artefatos grandes ou que devem ser gravados
  direto no destino, como firmware OTA.

### `components/tele_artifacts`

Registry generico de tipos de artefato.

Cada handler declara:

- `artifact_type`;
- `mode`: `TELE_ARTIFACT_MODE_FILE` ou `TELE_ARTIFACT_MODE_STREAM`;
- default de `restart_on_success`;
- callback de `check`;
- callback de `apply`.

API principal:

```c
esp_err_t tele_artifacts_register(const tele_artifact_handler_t *handler);
const tele_artifact_handler_t *tele_artifacts_find(const char *artifact_type);
esp_err_t tele_artifacts_check(const tele_artifact_request_t *request,
                               tele_artifact_check_result_t *out_result);
esp_err_t tele_artifacts_apply(const tele_artifact_request_t *request,
                               tele_artifact_apply_result_t *out_result);
esp_err_t tele_artifacts_get_status(const char *artifact_type,
                                    tele_artifact_status_t *out_status);
esp_err_t tele_artifacts_add_manifest_to_json(cJSON *root);
esp_err_t tele_artifacts_add_status_to_json(cJSON *root);
esp_err_t tele_artifacts_register_commands(void);
```

`tele_artifacts_register_commands()` registra comandos genericos no dispatcher
`tele_commands`, sem depender de MQTT. MQTT, portal e uma futura serial podem
chamar o mesmo dispatcher.

`tele_artifacts_add_manifest_to_json()` expõe os tipos registrados para UI e
ferramentas. `tele_artifacts_add_status_to_json()` expõe estado local dos
handlers que implementam callback de status.

### Atualizando Outros Tipos De Arquivo

O sistema de manifest e artefatos nao e limitado a firmware e CA bundle. Esses
dois tipos sao apenas handlers ja registrados no firmware atual. Qualquer dado
que possa ser baixado, validado e aplicado de forma segura pode virar um novo
`artifact_type`.

Bons candidatos:

- arquivos de configuracao;
- assets do portal web;
- tabelas de calibracao;
- modelos pequenos ou bases locais;
- pacotes de traducao;
- temas de UI;
- certificados, chaves ou outros blobs usados pela aplicacao.

Para arquivos comuns, use `TELE_ARTIFACT_MODE_FILE`. Nesse modo,
`tele_manifest_run_file()` baixa o artefato para um arquivo temporario, valida
tamanho e SHA-256, e so entao chama o callback do handler com o caminho do
arquivo verificado. O handler ainda precisa validar a semantica do conteudo
antes de promover o arquivo para uso.

Exemplo de handler para um pacote de configuracao:

```c
static const tele_artifact_handler_t handler = {
    .artifact_type = "config",
    .label = "Config bundle",
    .mode = TELE_ARTIFACT_MODE_FILE,
    .check = check_config_artifact,
    .apply = apply_config_artifact,
    .status = get_config_artifact_status,
};

ESP_ERROR_CHECK(tele_artifacts_register(&handler));
```

O adapter do novo artefato deve:

1. montar um `tele_manifest_request_t` com `artifact_type`, canal e limites;
2. implementar politica de versao propria, quando necessario;
3. chamar `tele_manifest_fetch()` no `check`;
4. chamar `tele_manifest_run_file()` ou `tele_manifest_run_stream()` no `apply`;
5. validar o conteudo no dominio do arquivo, por exemplo JSON, ranges,
   compatibilidade e campos obrigatorios;
6. aplicar com promocao segura, preferencialmente usando arquivo `.tmp`, backup
   e troca atomica quando o filesystem permitir;
7. expor status local se UI, MQTT ou operador precisarem acompanhar a aplicacao.

Use `TELE_ARTIFACT_MODE_STREAM` apenas quando o destino precisa receber bytes
diretamente durante o download, como uma particao OTA. Para a maioria dos
arquivos de aplicacao, o modo arquivo e mais simples e permite validacao
semantica antes de alterar o estado ativo.

Evite usar esse mecanismo diretamente para conjuntos grandes de multiplos
arquivos sem empacotamento, instalacoes parciais complexas ou dados que exigem
rollback transacional sofisticado. Nesses casos, prefira publicar um pacote
unico com indice interno, ou criar uma camada de instalacao especifica que seja
chamada depois que o pacote inteiro for verificado pelo manifest.

### `components/tele_ca_store`

Responsavel por:

- montar a particao SPIFFS `ca_store`;
- carregar bundle CA salvo e ativar com `esp_crt_bundle_set()`;
- aplicar bundle novo com promocao segura por `.tmp` e `.bak`;
- guardar e ler versao do bundle ativo.

Inicializacao:

```c
ESP_ERROR_CHECK(tele_ca_store_init());
```

### `components/tele_ca_updater`

Handler do artefato `ca_bundle`.

Uso:

```c
ESP_ERROR_CHECK(tele_ca_updater_register_artifact());
```

Politica atual:

- se a versao do manifest e igual a versao salva, pula;
- se a versao salva esta vazia, aplica;
- se a versao difere, aplica;
- tipo, canal, URL, tamanho e SHA sao validados por `tele_manifest`;
- `restart_on_success` reinicia depois de aplicar quando solicitado.

A rotina opcional de boot em `main/main.c` usa `tele_artifacts_apply()` para
aplicar `artifact_type = "ca_bundle"` depois que o Wi-Fi fica pronto.

### `components/tele_system/firmware_ota.c`

Handler do artefato `firmware`.

Responsavel por:

- upload manual pelo portal, em streaming;
- OTA por URL direta;
- OTA por manifest em streaming direto para a particao OTA;
- status de progresso, versao alvo, build id, URLs e erro;
- registro do handler `firmware` com `firmware_ota_register_artifact()`.

O comando generico `artifact/apply` apenas inicia a task OTA. O download roda em
segundo plano e continua usando `tele_manifest_run_stream()`. O boot partition
so e alterado depois de download completo, tamanho esperado, SHA-256 e
`esp_ota_end()` passarem.

## Inicializacao No App

Fluxo atual em `main/main.c`:

```c
ESP_ERROR_CHECK(tele_ca_store_init());
ESP_ERROR_CHECK(firmware_ota_init());
ESP_ERROR_CHECK(tele_ca_updater_register_artifact());
ESP_ERROR_CHECK(firmware_ota_register_artifact());
ESP_ERROR_CHECK(tele_artifacts_register_commands());
```

## Como Implementar Em Outro Projeto Do Zero

Esta sequencia parte de um projeto ESP-IDF novo ou de um firmware que ainda nao
usa o sistema de manifest. A ideia e escolher apenas as camadas necessarias para
o produto, mantendo cada papel isolado.

### 1. Prepare as particoes

Para atualizar firmware, o projeto precisa ter:

- particao `otadata`;
- pelo menos duas particoes de app, por exemplo `ota_0` e `ota_1`;
- espaco livre na particao OTA de destino maior que o maior `.bin` esperado.

Para atualizar CA bundle dinamico, adicione tambem uma particao SPIFFS para o
store de CA, usando o mesmo label configurado em
`CONFIG_TELE_CA_STORE_STORAGE_LABEL`.

### 2. Escolha os componentes

Use somente o necessario:

- `tele_manifest`: obrigatorio para qualquer artefato por manifest;
- `tele_artifacts`: necessario quando o app quer um registry comum para UI,
  MQTT, portal ou outro transporte chamar `artifact/check`,
  `artifact/apply` e `artifact/status`;
- `tele_ca_store`: necessario apenas para armazenar e ativar CA bundle
  dinamico;
- `tele_ca_updater`: adapter do artefato `ca_bundle`;
- `tele_system/firmware_ota.c`: servico de OTA de firmware, usado tanto por
  upload web quanto por URL direta ou manifest;
- `tele_portal_ota`: opcional, se o projeto quiser upload manual via portal web;
- `tele_commands`: necessario se `tele_artifacts_register_commands()` for usado.

Um projeto que quer somente upload OTA via web pode usar `firmware_ota` com
`tele_portal_ota` e nao precisa expor `artifact/check` ou `artifact/apply`.

### 3. Configure CMake, Kconfig e versao

Copie os componentes escolhidos para `components/` e ajuste os `REQUIRES` no
`CMakeLists.txt` do app. O firmware OTA depende de `app_update`,
`esp_https_ota`, `esp_http_client`, `esp-tls` e `esp_system`. O uso por manifest
acrescenta `tele_manifest` e, quando registrado como artefato, `tele_artifacts`.

O app precisa fornecer `firmware_version.h` com pelo menos:

```c
#define APP_VERSION_SEMVER "1.0.0"
#define APP_VERSION_LABEL "Meu Produto"
#define APP_BUILD_ID "1.0.0-local"
#define APP_VERSION_STRING APP_VERSION_SEMVER " " APP_VERSION_LABEL
```

`APP_VERSION_SEMVER` deve usar `MAJOR.MINOR.PATCH`, pois a politica atual de
firmware compara versoes nesse formato. Ajuste tambem os Kconfigs de tamanho,
timeout, caminho SPIFFS e URLs default conforme o produto.

### 4. Inicialize na ordem correta

Depois de `nvs_flash_init()`, inicialize as stores e os servicos locais antes de
registrar comandos ou rotas:

```c
ESP_ERROR_CHECK(tele_ca_store_init());              // se usar CA dinamico
ESP_ERROR_CHECK(firmware_ota_init());               // se usar OTA de firmware
ESP_ERROR_CHECK(tele_ca_updater_register_artifact());
ESP_ERROR_CHECK(firmware_ota_register_artifact());
ESP_ERROR_CHECK(tele_artifacts_register_commands());
```

Registre apenas o que existir no produto. Se nao houver CA dinamico, remova
`tele_ca_store_init()` e `tele_ca_updater_register_artifact()`. Se nao houver
comandos genericos de artefato, nao chame `tele_artifacts_register_commands()`.

### 5. Conecte os transportes

Para portal web manual, conecte `tele_portal_ota` por callbacks:

```c
static const tele_portal_ota_config_t ota_config = {
    .begin = portal_ota_begin_cb,
    .write = portal_ota_write_cb,
    .finalize = portal_ota_finalize_cb,
    .abort = portal_ota_abort_cb,
    .status = portal_ota_status_cb,
    .restart_delay_ms = 1200,
};

ESP_ERROR_CHECK(tele_portal_ota_init(&ota_config));
ESP_ERROR_CHECK(tele_portal_ota_register_routes());
```

Os callbacks devem chamar `firmware_ota_upload_begin()`,
`firmware_ota_upload_write()`, `firmware_ota_upload_finalize()`,
`firmware_ota_upload_abort()` e `firmware_ota_get_status()`. Assim o portal
continua sendo apenas transporte HTTP; a escrita OTA permanece no servico de
sistema.

Para MQTT, portal de comandos ou outro canal, use `tele_commands` ou chame
diretamente:

```c
tele_artifact_request_t request = {
    .artifact_type = "firmware",
    .manifest_url = "https://updates.example.com/prod/manifest.json",
    .channel = "prod",
    .restart_on_success = true,
};

tele_artifact_apply_result_t result = {0};
ESP_ERROR_CHECK(tele_artifacts_apply(&request, &result));
```

### 6. Publique os artefatos

Gere o manifest com `tools/update_artifacts/generate_manifest.py`, publique o
`.bin`, o bundle CA e os manifests em bucket, CDN, GitHub Release ou repositorio
de artefatos, e configure o app para buscar esses manifests somente depois que o
Wi-Fi estiver pronto.

Quando CA e firmware forem atualizados pelo mesmo ciclo, aplique CA primeiro e
firmware depois. Assim downloads futuros ja usam o bundle novo antes de uma
eventual troca de firmware.

### Observacao Sobre Isolamento Do Firmware OTA

`firmware_ota` deve continuar sendo o dono unico da escrita na particao OTA, do
boot partition, do estado de progresso e do reboot. `tele_portal_ota` deve
continuar desacoplado por callbacks, e `tele_artifacts` deve atuar apenas como
fachada generica para comandos de manifest.

Hoje o adapter do artefato `firmware` vive dentro de
`tele_system/firmware_ota.c`, junto com upload web e OTA por URL direta. Isso e
adequado para o TeleSystem porque todos os caminhos compartilham o mesmo estado
e a mesma exclusao mutua de escrita OTA. Em outro projeto, se a portabilidade ou
o uso de OTA web sem manifest virar requisito forte, esse adapter pode ser
separado futuramente em `firmware_ota_artifact.c` ou protegido por Kconfig, para
que o servico base de OTA nao precise depender obrigatoriamente de
`tele_manifest` e `tele_artifacts`.

## Schema De Manifest

Manifest minimo:

```json
{
  "schema": 1,
  "artifact_type": "firmware",
  "channel": "pilot",
  "version": "0.6.10",
  "build_id": "2026-06-24T12:00:00Z-0.6.10",
  "url": "https://updates.example.com/telesystem/pilot/TeleSystem.bin",
  "sha256": "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
  "size": 1286144,
  "critical": false
}
```

Campos opcionais:

- `urls`: lista de mirrors HTTPS;
- `min_version`: versao minima do firmware atual;
- `notes`: texto informativo para operador.

Regras:

- `schema` deve ser `1`;
- `artifact_type` deve corresponder a um handler registrado;
- `channel` precisa bater quando configurado pelo chamador;
- todas as URLs de artefato devem ser HTTPS;
- `sha256` tem 64 caracteres hexadecimais;
- `size` precisa ser maior que zero e menor que o limite do handler.

## Comandos

Registrados por `components/tele_artifacts`:

- `artifacts/get`;
- `artifact/check`;
- `artifact/status`;
- `artifact/apply`.

`artifacts/get` lista tipos registrados, seus modos (`file` ou `stream`) e o
default de reboot. `artifact/status` consulta progresso/estado local de um
tipo.

Exemplo OTA:

```json
{
  "cmd_id": "ota-apply-1",
  "name": "artifact/apply",
  "args": {
    "artifact_type": "firmware",
    "manifest_url": "https://updates.example.com/telesystem/pilot/manifest.json",
    "channel": "pilot",
    "restart_on_success": true
  }
}
```

Exemplo status OTA:

```json
{
  "cmd_id": "ota-status-1",
  "name": "artifact/status",
  "args": {
    "artifact_type": "firmware"
  }
}
```

Exemplo CA:

```json
{
  "cmd_id": "ca-apply-1",
  "name": "artifact/apply",
  "args": {
    "artifact_type": "ca_bundle",
    "manifest_url": "https://updates.example.com/ca/stable/bundle_ca.manifest.json",
    "channel": "stable"
  }
}
```

`artifact/apply` para `firmware` responde com `started_async = true`; progresso
e resultado aparecem em `/api/ota/status`, `state`, `heartbeat` e campos do
registry `tele_status`. Para `ca_bundle`, a aplicacao e sincronizada e o
resultado inclui URL selecionada, bytes recebidos e mensagem.

## Verificacao Periodica

Os componentes nao possuem scheduler proprio. A verificacao de tempos em tempos
deve viver no nivel da aplicacao, por exemplo em uma task iniciada por
`main/main.c` depois de Wi-Fi pronto.

Fluxo recomendado:

1. esperar Wi-Fi pronto com `wifi_manager_wait_until_ready()`;
2. chamar `tele_artifacts_check()` para `ca_bundle` e `firmware`;
3. aplicar CA automaticamente se a politica do produto permitir;
4. aplicar firmware somente se configurado para auto-apply;
5. repetir com intervalo configuravel e jitter.

## Publicacao De Artefatos

Use [tools/update_artifacts/README.md](../tools/update_artifacts/README.md) para
gerar manifests. Artefatos `.bin`, bundles CA e manifests gerados devem ser
publicados fora do repositorio de firmware, em release bucket, CDN, GitHub
Release ou repositorio de artefatos.
