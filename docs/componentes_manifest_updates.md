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
esp_err_t tele_artifacts_register_commands(void);
```

`tele_artifacts_register_commands()` registra comandos genericos no dispatcher
`tele_commands`, sem depender de MQTT. MQTT, portal e uma futura serial podem
chamar o mesmo dispatcher.

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

- `artifact/check`;
- `artifact/apply`.

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
