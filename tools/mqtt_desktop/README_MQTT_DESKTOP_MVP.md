# Jabur Consulting MQTT Control Center

Aplicativo desktop para administrar dispositivos ESP32 em campo via MQTT.

## Localizacao

- Este modulo fica em `tools/mqtt_desktop`.
- Execute os comandos abaixo dentro dessa pasta.

## Stack

- Python 3.10+
- CustomTkinter
- paho-mqtt

## Pre-requisitos do sistema (Linux)

- Python 3.10+ instalado
- Tkinter instalado no sistema (necessario para GUI)

Ubuntu/Debian:

```bash
sudo apt update
sudo apt install -y python3 python3-venv python3-tk python3-pip
```

## Funcionalidades MVP

- Conexao MQTT (host, porta, usuario, senha, TLS on/off)
  - opcao `auto_connect_on_start` no `config.json` para conectar ao iniciar
  - estado visual intermediario `Starting...`/`Auto-connect...`/`Connecting...` evita mostrar desconectado enquanto a configuracao e a conexao automatica estao pendentes
  - botoes Connect/Disconnect refletem o estado atual; `Disconnect` atua como cancelamento antes de conectar
- Descoberta de dispositivos por wildcard usando `mqtt.base_topic`:
  - `{base_topic}/+/availability`
  - `{base_topic}/+/seen`
  - `{base_topic}/+/heartbeat`
  - `{base_topic}/+/state`
  - `{base_topic}/+/meta/config`
  - `{base_topic}/+/meta/status`
  - `{base_topic}/+/meta/commands`
  - `{base_topic}/+/event`
  - `{base_topic}/+/cmd/out`
- Lista de dispositivos com visao compacta:
  - busca por device, firmware, sessao ou resumo
  - filtros `Todos`, `Online`, `Offline`, `Triagem` e `Retained`
  - contadores de total, online, offline, triagem e retained
  - colunas `estado`, `device_id`, `idade`, `fw` e `resumo`
  - `idade` mostra tempo desde o ultimo contato observado pelo desktop
  - `resumo` condensa IP, RSSI, bateria, estado, evento, erro e comandos pendentes
  - tooltip do resumo mostra SSID/IP quando disponivel
  - menu de contexto por device para copiar ID/IP, enviar comandos, abrir abas e limpar retained
- Painel do dispositivo selecionado com:
  - campos destacados para heartbeat, status/state, evento e cmd/out
  - foco em leitura rapida de valores relevantes
- Abas na area principal:
  - Resumo
  - Status
  - Comandos
  - Settings
- Comandos para `cmd/in`:
  - `ping`
  - `get_state`
  - `config/get`
  - `commands/get`
  - `get_technical_status`
  - `apply_and_reboot`
- Comandos via MQTT:
  - botao `commands/get` solicita o manifesto de comandos do device
  - renderiza `meta/commands` agrupado por `group`, usando `label` e `description`
  - gera entradas simples e botao `Enviar` para comandos descobertos no manifesto
  - oculta `config/set` e `config/reset` da lista principal; esses comandos sao usados pela aba Settings
- Settings via MQTT:
  - botao `Atualizar config` envia `config/get`, recarrega o manifesto do device e descarta edicoes locais nao salvas
  - atualizacoes automaticas preservam edicoes locais ainda nao salvas
  - renderiza `meta/config` como formulario compacto agrupado por base topic/grupo
  - mostra na tela apenas nome do campo, entrada, `Salvar` e `Reset`
  - marca campos modificados localmente com `alterado` ate salvar ou recarregar
  - usa cores no nome do campo: verde para `runtime_apply`, laranja para `reboot_required`, cor normal para configuracao apenas armazenada
  - mostra valor atual, default, origem, aplicacao, limites, tipo e flags por mouse over
  - campos booleanos usam switch; demais tipos usam entrada textual validada
  - campos `enum` com `choices` no manifest usam menu de selecao e enviam o valor numerico
  - cada campo editavel pode ser salvo com `config/set` ou resetado com `config/reset`
  - `mqtt.heartbeat_interval_s` e configurado como campo normal de Settings
  - JSON cru de `meta/config` fica oculto por padrao e pode ser mostrado/ocultado por botao
  - botao `Apply + reboot` envia `apply_and_reboot`
  - botao `Limpar retained` publica payload vazio retido nos topicos do device para remover ghosts
  - area de log e redimensionavel com mouse (splitter vertical)
  - clique direito no log permite copiar todo o log ou limpar a area
- Presenca de device:
  - mensagens MQTT retained nao marcam o device como online
  - payload vazio usado para limpar retained remove o snapshot local e nao conta como presenca
  - online/offline considera apenas mensagem live de `availability`, `heartbeat`, `state` ou `event` dentro do timeout
  - envio de comando nao conta como presenca; `cmd/out` OK de comando pendente conta, pois confirma resposta do device
  - `retained` indica device conhecido apenas por snapshot retido do broker nesta sessao, mesmo quando o snapshot retido e um LWT `offline` sem timestamp
  - se o device ja foi visto live na sessao, ao expirar o timeout ele vira `offline`, nao `retained`
  - em uma nova abertura do app, um device pode aparecer como `retained` novamente se ainda houver payload retido no broker
  - `pendente` indica device online com comando enviado e ainda sem resposta
  - `triagem` indica offline ou erro real, como `cmd/out` com falha ou evento de erro
  - toggle `Auto-probe presenca` na UI para ativar/desativar sonda automatica
  - com auto-probe ligado, o app envia `get_state` para devices conhecidos sem live recente
- Aba Status:
  - mostra cards genericos de conectividade, runtime, heartbeat, energia, memoria, manifesto e erros
  - renderiza `meta/status` em `Status declarado`, agrupado por `group`, usando `label` e `description`
  - separa campos com flag `technical` em `Status tecnico declarado`
  - mantem `Snapshot tecnico` para o payload detalhado de `get_technical_status`
  - mantem area `Raw/debug` com campos brutos de `state`, `get_state` e `status_tecnico`
  - botoes `get_state` e `status_tecnico` para atualizacao ativa
  - auto-update configuravel para `status_tecnico` com default de 3 segundos
- UX de botoes:
  - barras de acoes de Comandos, Settings e Status ficam fixas (nao rolam com o conteudo)
- Performance visual:
  - abas pesadas sao montadas sob demanda ao serem abertas
  - manifests e diagnosticos atualizam valores sem recriar widgets quando a estrutura nao mudou
- `cmd_id` gerado automaticamente
- comandos pendentes expiram automaticamente para nao bloquear auto-update indefinidamente
- Log em tempo real com destaque de `cmd/out` do dispositivo selecionado
- Log limitado em quantidade de linhas para reduzir degradacao em execucoes longas

## Execucao rapida

1. Ajuste (opcional) o arquivo `config.example.json`.
2. Para usar configuracao local, copie para `config.json` e ajuste os valores.
3. Rode:

```bash
cd tools/mqtt_desktop
chmod +x run_app.sh
./run_app.sh
```

## Execucao manual

```bash
cd tools/mqtt_desktop
python3 -m venv .venv
source .venv/bin/activate
pip install -r requirements.txt
python mqtt_control_center.py
```

Se ocorrer `ModuleNotFoundError: No module named 'tkinter'`, instale `python3-tk` e execute novamente.

Nao execute a GUI com `sudo`. Se a `.venv` ficou com dono `root`, corrija uma vez:

```bash
sudo chown -R "$USER:$USER" tools/mqtt_desktop/.venv
```

## Exemplo de payload de comando publicado

Topico:

- `{base_topic}/{device_id}/cmd/in`

Payload (exemplo):

```json
{
  "name": "get_state",
  "cmd_id": "cmd-20260602153001-a1b2c3",
  "ts": "2026-06-02T15:30:01"
}
```

## Observacoes

- Sem banco de dados.
- Sem autenticacao avancada.
- Tratamento basico para erro de conexao e JSON invalido.
- UI atualiza via fila de eventos para nao travar com alto volume de mensagens.

## Exemplo HiveMQ Cloud (TLS)

Use em `config.json`:

```json
{
  "mqtt": {
    "host": "d77a33536b2143ba8d70a3abd3188ae5.s1.eu.hivemq.cloud",
    "port": 8883,
    "base_topic": "v1/telesystem",
    "username": "SEU_USUARIO_HIVEMQ",
    "password": "SUA_SENHA_HIVEMQ",
    "tls": true
  },
  "auto_connect_on_start": true,
  "heartbeat_timeout_sec": 180,
  "technical_status_auto_update": true,
  "technical_status_update_interval_sec": 3
}
```

Tambem e aceito no campo Host/config: `mqtts://d77a33536b2143ba8d70a3abd3188ae5.s1.eu.hivemq.cloud:8883`.
