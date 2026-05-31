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
- montar storage FATFS;
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
- preparar staging da pasta storage em build-time;
- gerar imagem FATFS de storage e incluir no flash.

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

### prepare_storage_root.cmake

Script de preparo do conteudo da particao storage.

Responsabilidades:

- limpar staging anterior;
- recriar diretorio staging;
- copiar conteudo de storage para staging;
- servir de etapa anterior a geracao da imagem FATFS.

## Fluxo de inicializacao em app_main

Sequencia atual:

1. app_log_buffer_init
2. logs iniciais de boot (versao e PSRAM)
3. wm8782_probe (diagnostico de audio)
4. nvs_flash_init (com erase/retry em caso de pagina invalida)
5. acr_analysis_control_init
6. storage_mount
7. acr_trigger_output_init
8. acr_routes_register_with_portal
9. device_config_routes_register_with_portal
10. connectivity_controller_start
11. acr_orchestrator_run

Observacao:

- acr_orchestrator_run entra no loop principal de operacao, assumindo o controle do ciclo ACR.

## Como os modulos se conectam

A pasta main raiz atua como composicao dos subsistemas:

- app: ciclo ACR, configuracao cloud, estado de runtime, trigger de saida e storage
- audio: captura PCM/WAV e probe de hardware
- connectivity: maquina de estados Wi-Fi, provisionamento e LED de status
- portal: servidor HTTP, captive portal e helpers de transporte

Integracao de alto nivel:

1. connectivity inicia rede e disponibiliza portal conforme estado Wi-Fi.
2. portal agrega rotas tecnicas registradas por app e connectivity.
3. app (orquestrador) depende de rede pronta para fluxo de envio ACR.
4. audio fornece PCM para o pipeline de analise.

## Build e empacotamento de storage

No CMake da pasta main:

- o target prepare_storage_root executa prepare_storage_root.cmake;
- arquivos de storage sao copiados para staging;
- fatfs_create_spiflash_image gera imagem da particao storage;
- a imagem e incluída no processo de flash do projeto.

Efeito pratico:

- web assets, certs e arquivos de configuracao em storage passam a fazer parte do artefato de firmware (particao FATFS).

## Configuracao em runtime vs build-time

O projeto usa ambos:

- build-time: Kconfig (CONFIG_*)
- runtime persistente: NVS (credenciais e configuracoes operacionais)
- runtime em arquivo: storage FATFS (conteudo web/certs/config)

Esse desenho permite defaults de fabrica + ajustes em campo.

## Resumo

A raiz de main e o ponto de composicao do firmware:

- define ordem de boot;
- conecta os subsistemas principais;
- controla como o componente e compilado;
- prepara os artefatos de storage para deploy.

Com os documentos por subpasta, este arquivo fecha a visao arquitetural do nucleo da aplicacao.
