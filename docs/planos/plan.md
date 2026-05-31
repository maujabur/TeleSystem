## Plan: Ajuste Remoto de Parametros via MQTT

Planejar e validar um fluxo de ajuste tecnico remoto para 20 unidades globais usando MQTT, sem alterar codigo nesta etapa.

## Abordagem recomendada

1. Usar MQTT somente para ajuste de parametros tecnicos, sem OTA nesta fase.
2. Adotar um contrato unico por comando: validacao, aplicacao, persistencia e ack de retorno.
3. Trabalhar desde o inicio com seguranca minima: TLS, autenticacao e ACL por topico.
4. Comecar em ambiente gratuito gerenciado para reduzir complexidade operacional no piloto.
5. Fazer rollout por grupos progressivos: pilot (2), beta (5), global (20).

## Fase 1 - Baseline tecnico

Objetivo: consolidar o que ja existe de validacao, normalizacao e persistencia para os parametros que serao controlados via MQTT.

### Steps da fase

1. Mapear os parametros por categoria: audio, trigger, conectividade e credenciais ACR.
2. Inventariar os fluxos atuais de leitura/escrita para manter o padrao validar -> normalizar -> persistir -> sinalizar.
3. Definir limites tecnicos por parametro (faixa valida, tipo e comportamento em erro).

## Fase 2 - Contrato MQTT e seguranca

Objetivo: definir topicos, payloads e regras operacionais para comandos remotos confiaveis.

### Steps da fase

1. Definir taxonomia de topicos por dispositivo e escopo.
2. Definir canal de comando e canal de status/ack.
3. Definir JSON versionado de comando com request_id, timestamp, origem e params.
4. Definir JSON de resposta com status, codigo de erro, motivo e parametros aplicados.
5. Definir politica de seguranca para teste e producao: TLS obrigatorio, ACL por dispositivo/topico e protecao anti-replay.

Exemplo de topicos:

1. tenant/{device_id}/{scope}/set
2. tenant/{device_id}/{scope}/status

## Fase 3 - Ambiente de teste gratuito (POC)

Objetivo: escolher um ambiente simples, gratuito e com recursos minimos de seguranca para validar o fluxo.

### Steps da fase

1. Avaliar HiveMQ Cloud Free para POC com TLS e autenticacao.
2. Avaliar EMQX Cloud free/serverless para POC com ACL e topicos privados.
3. Usar broker publico somente para conectividade basica, sem dados sensiveis.
4. Avaliar opcao local com Mosquitto para laboratorio interno.
5. Fechar criterio de escolha: simplicidade, TLS, limite de conexoes, estabilidade e observabilidade.

## Fase 4 - Plano operacional de rollout

Objetivo: reduzir risco operacional na entrada em campo.

### Steps da fase

1. Definir grupos de rollout de parametros: pilot (2), beta (5), global (20).
2. Definir guardrails operacionais: limite de taxa, timeout de ack, retries maximos e regra de congelamento.
3. Definir matriz de responsabilidades: quem publica comando, quem aprova e quem executa reversao.
4. Definir janela de mudanca e criterio de promocao entre grupos.

## Fase 5 - Validacao e go/no-go

Objetivo: validar previsibilidade e rastreabilidade antes de implementar no firmware.

### Steps da fase

1. Executar testes de mesa com cenarios de erro e conflito de comandos.
2. Executar piloto em 2 dispositivos, em redes diferentes.
3. Validar latencia, confiabilidade de ack e rastreabilidade de mudancas.
4. Consolidar checklist de prontidao para implementacao (go/no-go).

## Relevant files

- main/app/acr_analysis_control.c - referencia de validacao/normalizacao/persistencia para parametros de analise
- main/app/acr_trigger_output.c - referencia para parametros de trigger e protecao de concorrencia
- main/connectivity/device_config_store.c - padrao de storage para parametros de conectividade
- main/app/acr_config_store.c - persistencia de credenciais e configuracoes ACR
- main/app/acr_routes.c - contrato JSON de status atual (base para canal status MQTT)
- main/portal/http_helpers.c - padrao de parsing/serializacao JSON reaproveitavel
- main/connectivity/connectivity_controller.c - ponto de orquestracao de conectividade para acoplamento futuro com MQTT
- docs/main_connectivity_estrutura_alto_nivel.md - visao de alto nivel da conectividade
- docs/main_app_estrutura_alto_nivel.md - visao de alto nivel da aplicacao

## Verification

1. Revisao documental do contrato de topicos e payloads com exemplos de sucesso e erro.
2. Checklist de seguranca validado: TLS, ACL por topico, autenticacao e anti-replay.
3. Tabela de limites operacionais definida: taxa maxima, timeout, retries e politica de bloqueio.
4. Simulacao de cenarios criticos: payload invalido, parametro fora de faixa, comando duplicado, dispositivo offline e perda intermitente.
5. Piloto em 2 unidades com criterio objetivo de aprovacao: ack >= 99%, nenhuma mudanca nao rastreada, nenhuma aplicacao fora de faixa.

## Decisions

- Incluido: somente planejamento de ajuste remoto de parametros via MQTT.
- Excluido: OTA, escrita de codigo, alteracao de build, alteracao de firmware e deploy de producao.
- Recomendacao para testes: iniciar com HiveMQ Cloud Free ou EMQX Cloud free/serverless para testar TLS e ACL desde o inicio.
- Restricao de seguranca: broker publico sem autenticacao apenas para conectividade inicial e sem parametros sensiveis.

## Further considerations

1. Padrao de confirmacao: Option A ack imediato apos validacao; Option B ack apos persistencia; Option C ack em duas fases (recebido e aplicado). Recomendado: Option C.
2. Segmentacao de topicos: Option A por dispositivo; Option B por grupo + dispositivo; Option C multi-tenant completo. Recomendado: Option B nesta fase.
3. Frequencia de alteracoes remotas: Option A livre; Option B janela de mudanca; Option C janela + aprovacao dupla. Recomendado: Option B para piloto e Option C para producao.
