# ACR ESP32 MQTT Desktop MVP

Aplicativo desktop simples para operar dispositivos ESP32 via MQTT.

## Localizacao

- Este modulo fica em `tools/mqtt_desktop`.
- Execute os comandos abaixo dentro dessa pasta.

## Stack

- Python 3.11+
- CustomTkinter
- paho-mqtt

## Pre-requisitos do sistema (Linux)

- Python 3.11+ instalado
- Tkinter instalado no sistema (necessario para GUI)

Ubuntu/Debian:

```bash
sudo apt update
sudo apt install -y python3 python3-venv python3-tk python3-pip
```

## Funcionalidades MVP

- Conexao MQTT (host, porta, usuario, senha, TLS on/off)
  - opcao `auto_connect_on_start` no `config.json` para conectar ao iniciar
  - botoes Connect/Disconnect refletem o estado atual da conexao
- Descoberta de dispositivos por wildcard:
  - `v1/acr/+/status`
  - `v1/acr/+/heartbeat`
  - `v1/acr/+/state`
  - `v1/acr/+/event`
  - `v1/acr/+/cmd/out`
- Lista de dispositivos com visao compacta:
  - busca por device, firmware, sessao ou resumo
  - filtros `Todos`, `Online`, `Offline`, `Triagem` e `Retained`
  - contadores de total, online, offline, triagem e retained
  - colunas `estado`, `device_id`, `idade`, `fw` e `resumo`
  - `idade` mostra tempo desde o ultimo contato observado pelo desktop
  - `resumo` condensa RSSI, bateria, estado, evento, erro e comandos pendentes
  - menu de contexto por device para copiar ID, enviar comandos, abrir abas e limpar retained
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
  - `get_settings`
  - `get_technical_status`
  - `apply_and_reboot`
  - `set_heartbeat_interval`
- Settings via MQTT:
  - botao `Ler settings` envia `get_settings` e preenche o formulario
  - botao `Salvar settings` envia `set_settings` apenas com alteracoes e em blocos menores
  - botao `Salvar heartbeat` envia `set_heartbeat_interval`
  - botao `Apply + reboot` envia `apply_and_reboot`
  - botao `Limpar retained` publica payload vazio retido nos topicos do device para remover ghosts
  - use `Ler settings` antes de salvar para sincronizar valores atuais do device
  - campos invalidos ficam destacados em vermelho e validos em verde
  - `trigger_mode` e `apsta_policy` usam selects com rotulos amigaveis
  - area de log e redimensionavel com mouse (splitter vertical)
- Presenca de device:
  - mensagens MQTT retained nao marcam o device como online
  - online/offline considera apenas mensagem live (nao retained) dentro do timeout
  - `retained` indica device conhecido apenas por snapshot retido do broker nesta sessao
  - se o device ja foi visto live na sessao, ao expirar o timeout ele vira `offline`, nao `retained`
  - em uma nova abertura do app, um device pode aparecer como `retained` novamente se ainda houver payload retido no broker
  - `pendente` indica device online com comando enviado e ainda sem resposta
  - `triagem` indica offline ou erro real, como `cmd/out` com falha, evento de erro ou erro tecnico reportado
  - toggle `Auto-probe presenca` na UI para ativar/desativar sonda automatica
  - com auto-probe ligado, o app envia `get_state` para devices conhecidos sem live recente
- Aba Status:
  - mostra cards formatados para loop, automatico, resultado, audio, BT_NEXT, erros, contadores, bateria e tempos
  - mantem tabela de campos brutos do payload de `state`, do resultado de `get_state` e do status tecnico
  - botoes `get_state` e `status_tecnico` para atualizacao ativa
  - auto-update configuravel para `status_tecnico` com default de 3 segundos
- UX de botoes:
  - barras de acoes de Comandos, Settings e Status ficam fixas (nao rolam com o conteudo)
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
python esp32_mqtt_desktop.py
```

Se ocorrer `ModuleNotFoundError: No module named 'tkinter'`, instale `python3-tk` e execute novamente.

## Exemplo de payload de comando publicado

Topico:

- `v1/acr/{device_id}/cmd/in`

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
