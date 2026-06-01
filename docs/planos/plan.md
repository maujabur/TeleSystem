## Plano de Apoio: Ajuste Remoto de Parametros via MQTT

Status: documento de apoio para fase piloto, sem implementacao de firmware nesta etapa.

Objetivo: definir um caminho seguro para ajuste remoto de parametros tecnicos em 20 unidades, com rastreabilidade e baixo risco operacional.

## Escopo

- Incluido: contrato de comando/resposta, seguranca basica, estrategia de rollout e criterios de validacao.
- Excluido: OTA, alteracao de codigo, alteracao de build e deploy de producao.

## Decisoes base

1. MQTT sera usado apenas para parametros tecnicos nesta fase.
2. Cada comando deve seguir o fluxo validar -> aplicar -> persistir -> responder.
3. Seguranca minima obrigatoria desde o piloto: TLS, autenticacao e ACL por topico.
4. Rollout progressivo: piloto (2) -> beta (5) -> global (20).

## Entregaveis da etapa de planejamento

1. Contrato de topicos e payloads versionado.
2. Matriz de limites por parametro (faixa, default, erro).
3. Politica operacional de rollout e rollback.
4. Checklist de go/no-go para iniciar implementacao.

## Contrato minimo recomendado

Topicos:

1. tenant/{device_id}/{scope}/set
2. tenant/{device_id}/{scope}/status

Payload de comando (minimo):

1. request_id
2. timestamp
3. origem
4. params

Resposta (minima):

1. request_id
2. status
3. codigo_erro
4. motivo
5. params_aplicados

Padrao de confirmacao recomendado: duas fases (recebido e aplicado).

## Sequencia sugerida

### Fase A - Baseline tecnico

1. Mapear parametros de audio, trigger, conectividade e ACR.
2. Inventariar validacao/normalizacao/persistencia existente.
3. Definir limites tecnicos por parametro.

### Fase B - Contrato e seguranca

1. Fechar taxonomia de topicos por dispositivo.
2. Definir esquema JSON de comando e ack.
3. Definir controles anti-replay e expiracao de comando.

### Fase C - POC operacional

1. Testar em broker gerenciado (HiveMQ Cloud Free ou EMQX free/serverless).
2. Executar cenario de erro: payload invalido, duplicado, timeout e dispositivo offline.
3. Medir confiabilidade de ack no grupo piloto (2 unidades).

### Fase D - Go/No-Go

1. Aprovar somente se houver rastreabilidade completa por request_id.
2. Exigir sucesso de ack >= 99% no piloto.
3. Exigir zero aplicacao fora de faixa em testes de mesa e campo.

## Referencias tecnicas

- main/app/acr_analysis_control.c
- main/app/acr_trigger_output.c
- main/connectivity/device_config_store.c
- main/app/acr_config_store.c
- main/app/acr_routes.c
- main/portal/http_helpers.c
- main/connectivity/connectivity_controller.c
- docs/main_connectivity_estrutura_alto_nivel.md
- docs/main_app_estrutura_alto_nivel.md

## Criterios de saida deste plano

1. Documento curto, executavel e revisavel por operacao e firmware.
2. Decisoes de seguranca e rollout consolidadas em um unico lugar.
3. Pronto para virar backlog tecnico quando houver aprovacao de implementacao.
