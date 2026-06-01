# Estrutura da pasta main (raiz) em alto nivel

## Objetivo

Este documento descreve os arquivos raiz de main e como eles compoem o sistema:

- papel de cada arquivo raiz;
- fluxo de inicializacao em app_main;
- conexao entre subpastas (app, audio, connectivity, portal);
- interfaces de build/configuracao.

## Arquivos raiz e funcao

### main.c

Ponto de entrada da aplicacao (app_main).

Responsabilidades:

- inicializar buffer de logs;
- registrar informacoes de boot (versao e PSRAM);
- executar probe do frontend de audio (WM8782);
- inicializar NVS;
- inicializar controle ACR;
- inicializar trigger output;
- registrar rotas de app e de configuracao de dispositivo no portal;
- iniciar controlador de conectividade;
- transferir execucao para o loop principal do orquestrador ACR.

### CMakeLists.txt

Descricao do componente ESP-IDF da pasta main.

Responsabilidades:

- registrar os fontes de app/audio/connectivity/portal;
- definir diretorios de include;
- declarar dependencias IDF e externas;
- referenciar os assets embarcados em `firmware_assets/`;
- embutir os HTMLs do portal e o certificado raiz via `EMBED_TXTFILES`.

### Kconfig.projbuild

Configuracao compile-time do firmware via menuconfig.

Menus principais:

- WiFi Config
  - credenciais padrao, SSID de provisionamento, botao de boot, modem sleep.

- Status LED Config
  - enable, GPIO, ordem de cor, brilho, cores e timings por estado.

- ACRCloud Config
  - parametros de cloud, polling, retry, captura e GPIO de trigger.

### idf_component.yml

Manifest de dependencias externas do componente.

- declara uso de espressif/cjson.

## Fluxo de inicializacao em app_main

Sequencia atual:

1. app_log_buffer_init
2. logs iniciais de boot (versao e PSRAM)
3. wm8782_probe (diagnostico de audio)
4. nvs_flash_init (com erase/retry em caso de pagina invalida)
5. acr_analysis_control_init
6. acr_trigger_output_init
7. acr_routes_register_with_portal
8. device_config_routes_register_with_portal
9. connectivity_controller_start
10. acr_orchestrator_run

Observacao:

- acr_orchestrator_run entra no loop principal de operacao, assumindo o controle do ciclo ACR.

## Como os modulos se conectam

A pasta main raiz atua como composicao dos subsistemas:

- app: ciclo ACR, configuracao cloud, estado de runtime e trigger de saida
- audio: captura PCM/WAV e probe de hardware
- connectivity: maquina de estados Wi-Fi, provisionamento e LED de status
- portal: servidor HTTP, captive portal e helpers de transporte

Integracao de alto nivel:

1. connectivity inicia rede e disponibiliza portal conforme estado Wi-Fi.
2. portal agrega rotas tecnicas registradas por app e connectivity.
3. app (orquestrador) depende de rede pronta para fluxo de envio ACR.
4. audio fornece PCM para o pipeline de analise.

## Build e empacotamento de assets

No CMake da pasta main:

- os HTMLs do portal e o certificado raiz sao lidos de `firmware_assets/`;
- esses arquivos sao embutidos no firmware via `EMBED_TXTFILES`.

## Configuracao em runtime vs build-time

O projeto usa ambos:

- build-time: Kconfig (CONFIG_*)
- runtime persistente: NVS (credenciais e configuracoes operacionais)

Esse desenho permite defaults de fabrica + ajustes em campo.

## Resumo

A raiz de main e o ponto de composicao do firmware:

- define ordem de boot;
- conecta os subsistemas principais;
- controla como o componente e compilado;
- prepara os artefatos de firmware para deploy.

Com os documentos por subpasta, este arquivo fecha a visao arquitetural do nucleo da aplicacao.
