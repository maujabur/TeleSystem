# Plano do Nucleo MQTT Generico

Este documento consolida o plano para transformar a base MQTT do TeleCafezinho
em um nucleo reutilizavel em outros projetos ESP32, sem carregar historico de
projetos anteriores.

## Objetivo

Criar um contrato generico entre firmware ESP32 e ferramentas externas de
operacao, especialmente o `tools/mqtt_desktop`, permitindo que cada produto
declare dinamicamente:

- presenca e ultima atividade;
- status operacional e tecnico;
- configuracoes editaveis;
- comandos disponiveis;
- metadados suficientes para renderizacao no Control Center.

O firmware deve continuar pequeno e previsivel. O Control Center deve ser
dinamico, dirigido por manifests, sem telas especificas por produto no nucleo.

## Componentes

### `tele_mqtt`

Responsavel pelo transporte MQTT:

- conexao, LWT e availability;
- topicos baseados em `base_topic` configuravel;
- publicacao retained de manifests;
- despacho de comandos remotos;
- publicacao de respostas em `cmd/out`;
- heartbeat e seen.

Nao deve conhecer campos especificos de produto. Deve apenas chamar callbacks
ou builders registrados pela camada integradora.

### `tele_config`

Responsavel por registry de settings:

- id, tipo, default, valor efetivo e origem;
- limites numericos ou de tamanho;
- `choices` opcionais para enum;
- flags como `mqtt`, `web`, `secret`, `read_only`, `reboot_required`;
- callback opcional de aplicacao runtime;
- persistencia de overrides em NVS;
- exportacao de `meta/config`.

Settings sao estado configuravel. A forma normal de altera-los por MQTT e
`config/set` ou `config/reset`.

### `tele_status`

Responsavel por registry de status:

- campos comuns, como Wi-Fi, IP, RSSI, heap, uptime e sincronismo de tempo;
- campos tecnicos;
- grupos para renderizacao;
- flags como `state`, `heartbeat`, `mqtt`;
- exportacao de `meta/status`.

Status descreve o estado atual do equipamento. Nao deve ser usado para
configuracao.

### `tele_commands`

Responsavel por registry de comandos:

- comandos read-only, como `ping`, `get_state`, `get_technical_status`;
- comandos mutaveis, como `apply_and_reboot`;
- metadados de grupo, label, descricao, argumentos e flags;
- exportacao de `meta/commands`.

Comandos sao acoes pontuais. Settings nao devem aparecer como comandos
individuais; eles devem ser configurados por `meta/config`.

### `tele_presence`

Camada integradora do produto atual:

- registra campos comuns de config/status;
- conecta callbacks runtime;
- fornece builders opcionais para diagnosticos especificos do produto;
- inicia `tele_mqtt` quando rede/tempo estiverem prontos.

Deve ficar pequeno e ser substituivel/adaptavel por outros projetos.

### `tools/mqtt_desktop`

Cliente generico para operacao:

- descobre devices por `availability`, `seen`, `heartbeat` e `state`;
- renderiza status a partir de `meta/status`;
- renderiza settings a partir de `meta/config`;
- renderiza comandos a partir de `meta/commands`;
- esconde comandos de infraestrutura de Settings, como `config/set` e
  `config/reset`, da aba principal de Comandos;
- usa `config/set` e `config/reset` internamente na aba Settings.

## Contrato de Topicos

Formato base:

```text
{base_topic}/{device_id}/{message_type}
```

Topicos principais:

- `availability`: estado online/offline, incluindo LWT;
- `seen`: ultima vez que o equipamento foi visto, retained;
- `heartbeat`: batimento periodico nao necessariamente retained;
- `state`: snapshot operacional curto;
- `event`: eventos pontuais;
- `meta/config`: manifesto retained de settings;
- `meta/status`: manifesto retained de status;
- `meta/commands`: manifesto retained de comandos;
- `cmd/in`: comandos enviados ao device;
- `cmd/out`: respostas de comando.

## Regras de Modelagem

- Setting e estado configuravel persistivel.
- Command e acao pontual.
- Status e leitura do estado atual.
- Um setting pode ser alterado por comando generico, mas nao deve virar um
  comando especifico.
- Comandos de infraestrutura podem existir no manifest, mas a UI pode oculta-los
  da lista principal.
- `runtime_apply` indica aplicacao imediata por callback.
- `reboot_required` indica que o override foi salvo, mas o efeito pleno depende
  de reboot.
- Campos sem essas flags sao apenas armazenados, e seu efeito depende de quem os
  le depois.

## Estado Atual

Ja implementado:

- `base_topic` configuravel no firmware e no Control Center;
- availability, seen, heartbeat, state e event;
- `meta/config` com tipos, limites, origem, default, valor efetivo, flags e
  choices para enum;
- `meta/status` com grupos e campos declarados;
- `meta/commands` com descoberta dinamica;
- aba Settings dinamica, compacta, com indicador `alterado`;
- dropdown para enum com `choices`;
- ocultacao de `config/set` e `config/reset` na aba Comandos;
- JSON cru de config ocultavel;
- copiar log pelo menu de contexto.
- builders genericos de `state`, `heartbeat`, `meta/config` e `meta/status`
  como defaults internos de `tele_mqtt`;
- `tele_presence` reduzido para registrar campos e fornecer apenas integracao
  especifica do TeleCafezinho, como status tecnico de energia.

## Proximas Fatias

### 1. Separar adaptadores MQTT genericos

Extrair de `tele_presence` o que ja e generico para dentro dos componentes:

- publicacao de `meta/config`: concluido;
- handlers `config/get`, `config/set`, `config/reset`: ja estavam no nucleo;
- publicacao de `meta/status`: concluido;
- handler `get_state`: concluido via builder default;
- handler `get_technical_status`: mantido no nucleo, com builder opcional de
  produto;
- publicacao de `meta/commands`: ja estava no nucleo.

Resultado esperado: produtos novos registram config/status/comandos e recebem o
contrato MQTT com pouca cola.

### 2. Definir API publica de integracao do nucleo

Criar uma superficie clara para projetos:

- inicializar MQTT generico: concluido via `tele_mqtt_start()`;
- registrar campos de produto: concluido via registries `tele_config` e
  `tele_status`;
- registrar comandos de produto: disponivel via `tele_commands`;
- declarar callbacks de readiness, timestamp e reboot: documentado em
  `tele_mqtt_config_t`;
- declarar builders opcionais de status tecnico/produto: documentado em
  `tele_mqtt_config_t`;
- usar `base_topic` como nome publico da raiz de topicos.

Resultado esperado: `tele_presence` vira um exemplo de integracao, nao o lugar
onde mora o nucleo.

### 3. Consolidar documentacao do contrato

Atualizar `manual_mqtt_operacao.md`, `tele_config.md`, `tele_status.md` e
`tele_commands.md` para refletirem o contrato final.

Concluido:

- `manual_mqtt_operacao.md` documenta a API publica de integracao do
  `tele_mqtt`;
- `tele_config.md` documenta registro, callbacks, manifesto `meta/config` e
  fluxo `config/get`, `config/set`, `config/reset`;
- `tele_status.md` documenta registro, payloads `state`/`heartbeat` e
  manifesto `meta/status`;
- `tele_commands.md` documenta registro, manifesto `meta/commands`, execucao
  por `cmd/in`/`cmd/out` e a diferenca entre command e setting.

Resultado esperado: outro projeto consegue implementar ou consumir o protocolo
sem ler o codigo do TeleCafezinho.

### 4. Preparar extracao futura

Quando o nucleo estiver estavel:

- separar nomes especificos de TeleCafezinho;
- revisar Kconfig defaults;
- manter exemplos minimos;
- avaliar mover componentes para repositorio compartilhado.

Resultado esperado: nucleo reaproveitavel sem copiar uma aplicacao inteira.

## Proximo Passo Recomendado

A proxima implementacao deve ser a Fatia 4: preparar a extracao futura,
separando nomes e defaults especificos de TeleCafezinho dos componentes
genericos, revisando Kconfig e deixando um exemplo minimo de integracao para
copiar em novos projetos.
