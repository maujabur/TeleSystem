# Migração para ESP32-S3 QFN56 (8 MB Flash)

## Hardware Novo
- **Modelo**: ESP32-S3 QFN56 (56-pin QFN 7mm x 7mm)
- **Revisão**: v0.2
- **CPU**: 2 Cores @ 240 MHz (Dual)
- **Embedded PSRAM**: 8 MB (3.3V AP Memory)
- **Flash**: 8 MB GigaDevice GD25Q64 (64 Mbit)
- **Flash ID**: 0x1740C8 (GigaDevice 0xC8)

## Mudanças Implementadas

### 1. Tamanho de Flash (sdkconfig.defaults)
**Antes:**
```
CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y
```

**Depois:**
```
CONFIG_ESPTOOLPY_FLASHSIZE_8MB=y
```

### 2. Partições de Storage (partitions.csv)
**Antes (16 MB):**
- nvs: 0x9000, tamanho 0x6000
- otadata: 0xF000, tamanho 0x2000
- phy_init: 0x11000, tamanho 0x1000
- factory: 0x20000, tamanho 0x400000 (4 MB)
- storage: 0x420000, tamanho 0xBE0000 (12 MB)
- **Total**: ~16 MB

**Depois (8 MB):**
- nvs: 0x9000, tamanho 0x6000 (24 KB) - *mantido*
- otadata: 0xF000, tamanho 0x2000 (8 KB) - *mantido*
- phy_init: 0x11000, tamanho 0x1000 (4 KB) - *mantido*
- factory: 0x20000, tamanho 0x200000 (2 MB) - *reduzido de 4 MB*
- storage: 0x220000, tamanho 0x5E0000 (6 MB) - *reduzido de 12 MB*
- **Total**: 8 MB (0x800000)

### 3. Versão de Firmware (main/app/firmware_version.h)
**Antes:**
```c
#define APP_VERSION_STRING "0.2.45 i2s pin runtime logs"
```

**Depois:**
```c
#define APP_VERSION_STRING "0.2.46 ESP32-S3 QFN56 8MB flash config"
```

## Como Compilar para Esta Placa

### Build Dev (padrão):
```bash
idf.py build
```

### Build com Monitor:
```bash
idf.py build flash monitor
```

### Build Release:
```bash
idf.py -B build-release -D SDKCONFIG=sdkconfig.release -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.release.defaults" build
```

### Flash para Porta Serial:
```bash
idf.py -p /dev/ttyACM0 flash monitor
```

## Verificação de Configuração

Para verificar se as configurações estão aplicadas:

```bash
# Verificar tamanho de flash
grep FLASHSIZE sdkconfig.defaults

# Verificar partições
cat partitions.csv

# Verificar versão de firmware
grep APP_VERSION_STRING main/app/firmware_version.h
```

## PSRAM Configuration (Mantido)
- Mode: OCT (Octal)
- Speed: 40 MHz
- Type: AUTO
- Boot init: Enabled
- SPIRAM use malloc: Enabled

A configuração de PSRAM foi mantida pois o novo hardware também possui 8 MB de PSRAM com suporte a modo OCT, assim como o anterior.

## Notas Importantes

1. **Redução de Storage**: A partição de storage foi reduzida de 12 MB para 6 MB. Verificar se o sistema de arquivos FAT ainda comporta os dados necessários.

2. **Tamanho da App**: O firmware da aplicação (factory partition) foi reduzido de 4 MB para 2 MB. O firmware atual deve ser verificado para garantir que cabe neste espaço.

3. **GPIO de Áudio**: Os GPIOs de I2S mantêm a mesma configuração:
   - MCLK: GPIO 1
   - DIN: GPIO 2
   - WS: GPIO 3
   - BCLK: GPIO 4

4. **Status LED**: GPIO 48 (mantido)

5. **Trigger Button**: GPIO 5 (mantido)

## Próximos Passos

1. Compilar e testar o firmware
2. Verificar se o tamanho do firmware cabe em 2 MB
3. Fazer flash e testar em hardware
4. Monitorar via Serial para verificar mensagens de inicialização

---
**Data**: April 23, 2026
**Versão**: 0.2.46
**Hardware Target**: ESP32-S3 QFN56 v0.2
