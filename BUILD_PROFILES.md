# Build Profiles: Dev e Release

Este projeto suporta dois perfis de build com baseline em `sdkconfig.defaults`:
- **Dev** (padrão): logs, erros detalhados e identificadores de rede visíveis para debug.
- **Release**: segurança endurecida com logs desativados, erros genéricos e dados sensíveis ocultos (overrides em `sdkconfig.release.defaults`).

## Diferenças entre perfis

| Configuração | Dev | Release |
|---|---|---|
| `/api/logs` endpoint | ✓ ativo | ✗ desativado |
| SSID/IP em `/api/status` | ✓ visível | ✗ oculto |
| Erros HTTP detalhados | ✓ esp_err_to_name | ✗ mensagem genérica |
| DEBUG/VERBOSE logs | ✓ capturados | ✗ filtrados |

## Comandos de Build

### Build Dev (padrão, sem parâmetros)
```bash
idf.py build
```

### Menuconfig Dev (padrão, atualiza `sdkconfig`)
```bash
idf.py -D SDKCONFIG=sdkconfig -D SDKCONFIG_DEFAULTS="sdkconfig.defaults" menuconfig
```

### Build Dev (explícito)
```bash
idf.py -B build-dev -D SDKCONFIG=sdkconfig.dev -D SDKCONFIG_DEFAULTS="sdkconfig.defaults" build
```

### Menuconfig Dev (arquivo dedicado)
```bash
idf.py -B build-dev -D SDKCONFIG=sdkconfig.dev -D SDKCONFIG_DEFAULTS="sdkconfig.defaults" menuconfig
```

### Build Release
```bash
idf.py -B build-release -D SDKCONFIG=sdkconfig.release -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.release.defaults" build
```

### Menuconfig Release
```bash
idf.py -B build-release -D SDKCONFIG=sdkconfig.release -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.release.defaults" menuconfig
```

## Comandos de Flash

### Flash Dev (build padrão)
```bash
idf.py -p /dev/ttyACM0 flash
```

### Flash Dev com Monitor
```bash
idf.py -p /dev/ttyACM0 flash monitor
```

### Flash Release
```bash
idf.py -B build-release -D SDKCONFIG=sdkconfig.release -p /dev/ttyACM0 flash
```

### Flash Release com Monitor
```bash
idf.py -B build-release -D SDKCONFIG=sdkconfig.release -p /dev/ttyACM0 flash monitor
```

## Artefatos para OTA

- Binário principal da aplicação: `build-dev/acr-cloud-test.bin`
- Binário de dados OTA inicial: `build-dev/ota_data_initial.bin`
- Para build release, os artefatos equivalentes ficam em `build-release/`.

## Workflow Recomendado

1. **Desenvolvimento**: use build padrão (dev)
   ```bash
   idf.py build
   idf.py flash monitor
   ```

2. **Testes de segurança/release**: construa release em pasta separada
   ```bash
   idf.py -B build-release -D SDKCONFIG=sdkconfig.release -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.release.defaults" build
   idf.py -B build-release -D SDKCONFIG=sdkconfig.release -p /dev/ttyACM0 flash monitor
   ```

3. **Limpar builds sem afetar o outro perfil**:
   ```bash
   rm -rf build-dev build-release
   ```

## Notas

- O perfil `dev` é o padrão quando você roda comandos sem `-B build-xxx`.
- `sdkconfig.defaults` é a fonte de verdade do perfil `dev`.
- `sdkconfig.dev.defaults` é mantido apenas por compatibilidade com comandos antigos e pode permanecer mínimo.
- O `menuconfig` sem parâmetros edita o `sdkconfig` da raiz. Neste repositório, ele deve representar o snapshot atual do perfil `dev`.
- Use `-B build-dev` e `-B build-release` para manter caches de compilação separados.
- Use `-D SDKCONFIG=sdkconfig.dev` e `-D SDKCONFIG=sdkconfig.release` para evitar contaminação entre perfis.
- O `/dev/ttyACM0` pode variar conforme a porta serial do seu dispositivo.
- Monitor inicia automático com `flash monitor` (Ctrl+] ou Ctrl+D para sair).
