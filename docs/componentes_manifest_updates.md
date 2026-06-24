# Componentes De Manifest E Updates

Este grupo implementa updates por manifest para dois artefatos:

- bundle de certificados CA;
- firmware OTA em streaming.

O componente generico conhece manifest, HTTPS, tamanho, SHA-256 e politica de
versao por callback. Os adaptadores de dominio conhecem como aplicar cada tipo
de artefato.

## Componentes

### `components/tele_manifest`

Responsavel por:

- baixar manifests pequenos por HTTPS;
- validar schema JSON;
- normalizar `url` e `urls`;
- validar `artifact_type`, `channel`, `version`, `sha256`, `size` e HTTPS;
- baixar artefato em modo arquivo ou streaming;
- calcular SHA-256 durante o download;
- chamar callbacks de progresso e aplicacao.

Use `tele_manifest_fetch()` quando so precisa consultar o manifest. Use
`tele_manifest_run_file()` para artefatos pequenos que precisam virar arquivo
local antes da aplicacao. Use `tele_manifest_run_stream()` para firmware OTA,
onde nao deve existir staging completo em filesystem.

### `components/tele_ca_store`

Responsavel por:

- montar a particao SPIFFS `ca_store`;
- carregar bundle CA salvo e ativar com `esp_crt_bundle_set()`;
- aplicar bundle novo com promocao segura por `.tmp` e `.bak`;
- guardar e ler versao do bundle ativo.

Inicializacao atual:

```c
ESP_ERROR_CHECK(tele_ca_store_init());
```

Essa chamada acontece em `main/main.c` antes do OTA, portal, Wi-Fi e MQTT.

### `components/tele_ca_updater`

Adaptador para `artifact_type = "ca_bundle"`.

Use:

```c
const tele_ca_updater_config_t config = {
    .manifest_url = "https://updates.example.com/ca/stable/manifest.json",
    .channel = "stable",
    .restart_on_update = false,
};

tele_manifest_run_result_t result = {0};
esp_err_t err = tele_ca_updater_apply(&config, &result);
```

Politica atual:

- se a versao do manifest e igual a versao salva, pula;
- se a versao salva esta vazia, aplica;
- se a versao difere, aplica;
- tipo, canal, URL, tamanho e SHA sao validados por `tele_manifest`.

Existe uma rotina opcional de boot em `main/main.c`, controlada por:

- `CONFIG_TELE_CA_UPDATER_BOOT_ENABLED`;
- `CONFIG_TELE_CA_UPDATER_MANIFEST_URL`;
- `CONFIG_TELE_CA_UPDATER_CHANNEL`;
- `CONFIG_TELE_CA_UPDATER_RESTART_ON_UPDATE`;
- `CONFIG_TELE_CA_UPDATER_BOOT_WAIT_TIMEOUT_MS`.

Por default ela fica desligada.

### `components/tele_system/firmware_ota.c`

Responsavel por OTA de firmware:

- upload manual pelo portal, em streaming;
- OTA antigo por URL direta;
- OTA por manifest em streaming direto para a particao OTA;
- status de progresso, versao alvo, build id, URLs e erro.

Para consultar:

```c
firmware_ota_manifest_config_t config = {
    .manifest_url = "https://updates.example.com/telesystem/pilot/manifest.json",
    .channel = "pilot",
    .allow_same_version = false,
    .restart_on_success = true,
};

tele_manifest_artifact_t artifact = {0};
esp_err_t err = firmware_ota_check_manifest(&config, &artifact);
```

Para aplicar:

```c
esp_err_t err = firmware_ota_start_manifest(&config);
```

`firmware_ota_start_manifest()` cria uma task; chamadores MQTT/HTTP nao devem
ficar bloqueados durante o download. O boot partition so e alterado depois de
download completo, tamanho esperado, SHA-256 e `esp_ota_end()` passarem.

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
- `artifact_type` deve ser `firmware` ou `ca_bundle`, conforme o adaptador;
- `channel` precisa bater quando configurado pelo chamador;
- todas as URLs de artefato devem ser HTTPS;
- `sha256` tem 64 caracteres hexadecimais;
- `size` precisa ser maior que zero e menor que o limite do chamador.

## Comandos MQTT

Registrados por `components/tele_presence`:

- `ota_check`;
- `ota_apply`;
- `ca_check`;
- `ca_apply`.

Exemplo:

```json
{
  "cmd_id": "ota-apply-1",
  "name": "ota_apply",
  "args": {
    "manifest_url": "https://updates.example.com/telesystem/pilot/manifest.json",
    "channel": "pilot",
    "restart_on_success": true
  }
}
```

`ota_apply` responde apenas que a task iniciou. O progresso aparece em
`/api/ota/status`, `state`, `heartbeat` e campos do registry `tele_status`.

## Verificacao Periodica

Os componentes nao possuem scheduler proprio. A verificacao de tempos em tempos
deve viver no nivel da aplicacao, por exemplo em uma task iniciada por
`main/main.c` depois de Wi-Fi pronto.

Fluxo recomendado para uma rotina principal:

1. esperar Wi-Fi pronto com `wifi_manager_wait_until_ready()`;
2. aplicar CA automaticamente se politica do produto permitir;
3. consultar OTA e publicar/notificar resultado;
4. aplicar OTA somente se configurado para auto-apply;
5. repetir com intervalo configuravel e jitter.

## Publicacao De Artefatos

Use [tools/update_artifacts/README.md](../tools/update_artifacts/README.md) para
gerar manifests. Artefatos `.bin`, bundles CA e manifests gerados devem ser
publicados fora do repositorio de firmware, em release bucket, CDN, GitHub
Release ou repositorio de artefatos.
