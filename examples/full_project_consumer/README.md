# Full Project Consumer

Exemplo ESP-IDF que replica o funcionamento do projeto principal `TeleSystem`,
mas consome os componentes pela lista de dependencias do `main/idf_component.yml`
do proprio exemplo.

Ele reutiliza o mesmo boot do produto:

- inicializacao de logs do portal;
- politica de VBAT e POWER_GOOD;
- NVS, CA store e OTA de firmware;
- artefatos `ca_bundle` e `firmware`;
- comandos genericos de artefato;
- portal HTTP, Wi-Fi, NTP, indicadores e presence MQTT.

Por padrao, o exemplo compila sem credenciais reais, sem broker MQTT e sem GPIO
de LED. Habilite esses recursos no `menuconfig` do exemplo quando for testar em
hardware.

## Build local

Na raiz do repositorio:

```bash
idf.py -C examples/full_project_consumer build
```

## Consumo via GitHub

Em um projeto externo com ESP-IDF Component Manager habilitado, copie a lista de
dependencias comentada em `main/idf_component.yml` e troque `version: main` por
uma tag ou commit para builds reproduziveis.

Formato de cada componente:

```yaml
dependencies:
  espressif/cjson: "^1.7.19"
  espressif/mqtt: "*"

  tele_wifi:
    git: https://github.com/maujabur/TeleSystem.git
    path: components/tele_wifi
    version: main
```

O exemplo declara todos os componentes usados pelo app principal porque muitos
deles sao componentes internos do mesmo repositorio e precisam estar visiveis
para o CMake do consumidor externo.
