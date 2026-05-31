# Estrutura de main/audio em alto nivel

## Objetivo

Este documento descreve o subsistema de audio em main/audio:

- funcao de cada arquivo;
- funcoes publicas e responsabilidade de cada modulo;
- fluxo de captura/processamento;
- interfaces usadas fora de main/audio, em nivel de contrato;
- mecanismos de sincronizacao e estado.

## Visao geral

A pasta main/audio fornece a base de captura e empacotamento de audio para o pipeline ACR:

1. captura PCM via I2S;
2. processamento basico (ganho, RMS, pico, atividade/silencio);
3. armazenamento em buffer (PSRAM) ou arquivo WAV;
4. suporte utilitario para cabecalho/escrita WAV;
5. probe de hardware do codec no boot.

## Papel de cada arquivo

### audio_capture.c / audio_capture.h

Modulo principal de captura de audio.

- inicializa e configura I2S RX;
- grava audio em WAV no filesystem ou em buffer PCM em memoria;
- calcula metricas de captura (RMS, pico, bytes, tempo ativo);
- aplica ganho digital com saturacao e sinaliza clipping;
- oferece helper para liberar buffer alocado.

Funcoes publicas:

- audio_capture_record_wav
- audio_capture_record_pcm_to_buffer
- audio_capture_buffer_free

### wav_writer.c / wav_writer.h

Utilitario de serializacao WAV.

- constroi cabecalho RIFF/WAV;
- abre/escreve/fecha arquivo WAV;
- no fechamento atualiza tamanhos finais no cabecalho.

Funcoes publicas:

- wav_writer_build_header
- wav_writer_open
- wav_writer_write_pcm
- wav_writer_close

### wm8782_probe.c / wm8782_probe.h

Diagnostico de hardware do frontend de audio no boot.

- inicializa leitura I2S curta;
- coleta amostras para aferir faixa minima/maxima;
- retorna indicativo de presenca/resposta do codec WM8782.

Funcao publica:

- wm8782_probe

## Fluxo interno de captura

Fluxo tipico em audio_capture_record_pcm_to_buffer:

1. valida parametros de captura;
2. aloca buffer de destino em PSRAM;
3. inicializa canal I2S RX e habilita leitura;
4. executa pre-roll de estabilizacao inicial;
5. loop de captura por blocos:
   - le amostras I2S;
   - aplica ganho digital com saturacao;
   - acumula metricas (RMS, pico, clipping, atividade);
   - escreve bloco no buffer de saida;
6. desabilita/deleta canal I2S;
7. preenche audio_capture_result_t;
8. retorna buffer para consumidor.

No fluxo de arquivo (audio_capture_record_wav), a diferenca principal e escrever PCM no wav_writer durante o loop.

## Dados e metricas produzidos

audio_capture_result_t agrega informacoes de telemetria da captura, incluindo:

- bytes_written;
- RMS agregado;
- pico de amostra;
- tempo ativo estimado (active_ms) com base em limiar de silencio;
- flag de clipping.

audio_capture_buffer_t encapsula o bloco PCM para consumo posterior por outros modulos.

## Integracao com outras pastas (interfaces)

### main/app

- acr_orchestrator consome:
  - audio_capture_record_pcm_to_buffer
  - audio_capture_buffer_free

Uso:

- executar captura no ciclo ACR e repassar PCM para upload/analise.

- acr_client consome:
  - wav_writer_build_header

Uso:

- montar WAV em memoria a partir de PCM antes do envio HTTP.

- acr_analysis_control usa constantes de limite/duracao da captura (via headers) para normalizar configuracao.

### main/main.c

- wm8782_probe

Uso:

- diagnostico inicial do hardware de audio no boot.

### ESP-IDF / FreeRTOS / drivers

- I2S driver (i2s_common/i2s_std): captura RX
- GPIO driver: pinos de interface de audio
- heap_caps (PSRAM): buffers grandes de captura
- esp_check / esp_log: tratamento de erro e observabilidade
- math/stdlib/string: processamento numerico e utilitarios

## Mecanismos de comunicacao e sincronizacao

O modulo audio opera majoritariamente de forma sincrona e bloqueante:

1. chamadas diretas
- quem invoca a API aguarda a captura terminar.

2. sem fila/event bus interno
- nao ha EventGroup/esp_event dentro de main/audio.

3. estado local por chamada
- metricas e buffers retornam por estruturas de saida;
- nao ha estado global complexo exportado.

4. serializacao de uso
- desenho assume uso sequencial pelo orquestrador (fluxo principal).

## Consideracoes de arquitetura

Pontos fortes:

- separacao clara entre captura (audio_capture), formato (wav_writer) e diagnostico (wm8782_probe);
- API objetiva para captura em memoria, adequada ao pipeline ACR;
- telemetria rica de captura para diagnostico de campo.

Pontos de atencao:

- configuracoes de taxa/pinos/ganho sao majoritariamente fixas em build-time;
- captura bloqueante pode alongar ciclo se duracao for alta;
- sem mecanismos internos de concorrencia, assumindo acesso serial.

## Resumo

main/audio fornece a infraestrutura de ingestao de audio do firmware com foco em robustez e simplicidade:

- captura I2S com metricas;
- entrega PCM para a camada de aplicacao;
- suporte WAV reutilizavel;
- probe de hardware no boot.

A integracao com main/app e direta e bem definida, o que facilita manutencao do pipeline de analise ACR.
