# Plano de Implementacao MQTT para ESP32 Distribuido

## Contexto

Cada placa ESP32 sera instalada em uma casa diferente. Nao existe LAN unica para descoberta local confiavel de dispositivos online.

Objetivo deste plano:

- identificar online/offline de cada dispositivo em tempo quase real;
- permitir comando remoto seguro;
- manter baixo custo operacional para inicio (aprox. 10 equipamentos online).

## Status atual (Junho 2026)

- Broker piloto selecionado e validado: HiveMQ Cloud.
- Firmware ESP32 ja conecta com MQTT sobre TLS usando validacao por CA bundle.
- Presenca operacional implementada com `status`, `state`, `heartbeat` e `event`.
- Canal de comando operacional implementado com `cmd/in` e `cmd/out`.
- Comandos validados em bancada: `ping`, `get_state`, `get_settings`, `set_heartbeat_interval`, `set_settings` e `apply_and_reboot`.
- Identificacao do dispositivo ajustada para formato amigavel: `{upload_prefix}-{ultimos_3_bytes_do_mac}`.
- Deduplicacao em RAM por `cmd_id` implementada para comandos mutaveis.
- Manual operacional publicado em `docs/manual_mqtt_operacao.md`.

Itens ainda nao concluidos:

- persistencia em NVS do `heartbeat_interval_s`;
- backend/painel servidor para consolidacao de presenca e alarmes;
- ACL por dispositivo no broker ainda depende da operacao no HiveMQ;
- seguranca adicional de comando (assinatura/token por comando) ainda nao implementada.

## Escopo da Fase 1 (MVP)

- Transporte principal via MQTT sobre TLS.
- Presenca baseada em LWT + heartbeat.
- Comando remoto assinado em topico dedicado por dispositivo.
- Dashboard/backend com visao de status por dispositivo.

Fora do escopo da Fase 1:

- OTA completa via MQTT;
- acesso shell/rede tipo VPN no proprio ESP32;
- descoberta de equipamentos por scan de LAN.

## Arquitetura alvo

1. ESP32 conecta em broker MQTT (saida para internet, porta TLS).
2. ESP32 publica status, heartbeat, estado e eventos.
3. Backend assina topicos de todos os dispositivos e calcula presenca.
4. Backend publica comandos por dispositivo e recebe ack/resultado.
5. UI operacional consulta backend (nao conecta direto ao broker no MVP).

## Convencao de identificacao

- implementado no firmware: `{ACR_UPLOAD_PREFIX}-{ultimos_3_bytes_do_mac}`
- exemplo atual: `skips_999-5112D0`
- tenant_id opcional continua possivel no backend, mas nao esta no payload atual
- `session_id` agora e gerado uma vez por boot, preferencialmente como `{timestamp_utc_basico}-{mac_tail}`

## Estrutura de topicos (v1)

- v1/acr/{device_id}/status
- v1/acr/{device_id}/heartbeat
- v1/acr/{device_id}/state
- v1/acr/{device_id}/event
- v1/acr/{device_id}/cmd/in
- v1/acr/{device_id}/cmd/out

Observacao:

- o topico `telemetry` foi previsto no desenho inicial, mas ainda nao foi implementado como topico separado;
- no estado atual, a telemetria leve vai em `heartbeat`.

## Regras de QoS e retained

- status: QoS 1, retained = true
- heartbeat: QoS 0 (ou 1), retained = false
- state: QoS 1, retained = true
- telemetry: QoS 0, retained = false
- event: QoS 1, retained = false
- cmd/in: QoS 1, retained = false
- cmd/out: QoS 1, retained = false

## Presenca online/offline

Sinais usados em conjunto:

1. Mensagem status=online ao conectar.
2. LWT status=offline em queda inesperada.
3. Heartbeat periodico para timeout de ausencia.

Politica inicial recomendada:

- heartbeat a cada 60s;
- offline por timeout quando ultimo heartbeat > 180s;
- quando receber LWT offline, marcar offline imediato.

## Contrato minimo de payload

Campos comuns:

- device_id
- ts (UTC ISO8601)

Campos comuns ja implementados nos payloads principais:

- fw
- session_id

Campos uteis no heartbeat:

- uptime_s
- rssi
- heap_free
- vbat_mv

Campos de comando:

- cmd_id
- name
- args
- timeout_s

Campos de ack:

- cmd_id
- ok
- result ou error

## Seguranca

Obrigatorio no MVP:

- MQTT sobre TLS;
- validacao de certificado do broker no ESP32;
- credencial por dispositivo;
- ACL por topico para isolar device_id.

Status:

- TLS: implementado.
- validacao de certificado: implementado via CA bundle.
- credencial no broker: funcional no piloto.
- ACL por topico: ainda depende da configuracao operacional no HiveMQ.

Desejavel nas proximas fases:

- mTLS por dispositivo;
- rotacao automatizada de credenciais;
- assinatura de payload de comando para acoes criticas.

## Avaliacao de brokers gratuitos (inicio com 10 dispositivos)

### Opcao A: HiveMQ Cloud (Free)

Pontos fortes:

- onboarding simples e rapido;
- TLS nativo;
- boa estabilidade para POC/MVP.

Limitacoes:

- limites de conexao/trafego no plano free;
- recursos avancados e escalabilidade exigem plano pago.

### Opcao B: EMQX Cloud (Plano gratuito)

Pontos fortes:

- broker maduro para IoT;
- bom conjunto de recursos para evolucao.

Limitacoes:

- limites de uso no free;
- experiencia inicial depende da oferta/regiao disponivel.

### Opcao C: Mosquitto auto-hospedado em VPS free tier

Pontos fortes:

- controle total de configuracao e ACL;
- sem lock-in de plataforma de broker gerenciado.

Limitacoes:

- maior carga operacional (patching, observabilidade, backup, hardening);
- disponibilidade depende da sua operacao.

## Recomendacao para este projeto (10 equipamentos)

1. Comecar com broker gerenciado gratuito (HiveMQ Cloud ou EMQX Cloud).
2. Implementar contrato de topicos/payload ja pronto para migracao sem quebra.
3. Revisar limites apos 2 a 4 semanas de telemetria real.
4. Se limite free virar gargalo, migrar para plano pago ou broker proprio com o mesmo contrato v1.

## Plano de execucao

### Etapa 1 - Contrato e backend basico

- congelar topicos v1;
- definir esquema JSON minimo;
- criar tabela de presenca (device_id, last_seen, online, motivo_offline).

Status:

- topicos base de presenca/comando implementados e validados em firmware;
- tabela/persistencia de presenca em backend ainda nao implementada neste repositorio.

Criterio de saida:

- backend marca online/offline corretamente em simulacao de queda e reconexao.

### Etapa 2 - Firmware ESP32

- conectar MQTT com TLS;
- configurar LWT;
- publicar status online no connect;
- publicar heartbeat a cada 60s;
- assinar cmd/in e responder cmd/out.

Status:

- concluido para o escopo MVP;
- extensoes adicionais implementadas alem do plano inicial:
	- comandos de leitura e escrita de settings;
	- reboot remoto com ACK;
	- deduplicacao por `cmd_id` em comandos mutaveis.

Criterio de saida:

- cada placa aparece no painel em ate 10s apos conectar;
- transicao offline acontece por LWT ou timeout conforme esperado.

### Etapa 3 - Operacao e observabilidade

- painel com lista de placas e estado atual;
- logs de comandos por cmd_id;
- alarmes de offline prolongado.

Status:

- observabilidade basica no broker ja e possivel via subscriptions MQTT e apps Android;
- painel operacional/backoffice ainda nao foi implementado;
- alarmes de offline prolongado ainda dependem de backend.

Criterio de saida:

- operacao consegue identificar rapidamente placas indisponiveis e ultimo contato.

## Riscos e mitigacoes

- Relogio sem NTP correto: incluir sincronizacao NTP no boot e retentativas.
- Tempestade de reconexao apos queda de internet: usar backoff exponencial com jitter.
- Mensagem grande demais para RAM: limitar payload e validar tamanho antes de publicar.
- Dependencia de plano free: monitorar uso semanal e planejar migracao antecipada.

Riscos ainda abertos no estado atual:

- `heartbeat_interval_s` ainda nao persiste entre boots;
- deduplicacao por `cmd_id` e apenas em RAM;
- comandos mutaveis ainda nao exigem assinatura/token por payload;
- nao existe backend central para auditoria e trilha historica.

## Decisao inicial proposta

- Broker inicial: HiveMQ Cloud Free (ou EMQX Cloud Free se melhor latencia/regiao).
- Heartbeat: 60s.
- Timeout offline: 180s.
- QoS critico: 1 para status/state/cmd, 0 para telemetria.

Com isso, o projeto cobre o requisito de "quem esta online" em redes domesticas separadas sem depender de VPN no ESP32.

## Proximos passos recomendados

1. Persistir `heartbeat_interval_s` em NVS.
2. Configurar ACL por dispositivo no HiveMQ.
3. Criar backend simples para `last_seen`, online/offline e historico de comandos.
4. Implementar seguranca adicional para comandos sensiveis (`set_settings` e `apply_and_reboot`).