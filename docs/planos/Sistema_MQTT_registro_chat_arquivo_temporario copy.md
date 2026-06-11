aqui está o registro da conversa anterior:
> vamos falar do painel mqtt e sua integração num sistema generico
>
> o esp32 poderia receber nos primeiros contatos com os campos configuraveis - e agora me acorre, com os campos de status tambem, talvez um registry de campos de status seja bom, me ajude a avaliar
>
> o mqtt pode ter alguns itens sempre presentes que serao comuns a todos, como IP. tambem quero ser capaz de saber até pelo LWT a ultima fez que o equipamento foi visto
>
> qual a avaliação?

<details><summary>4 previous messages</summary>

> Vou usar a skill de brainstorming aqui porque estamos desenhando comportamento e contrato entre componentes antes de implementar. Vou manter como avaliação técnica, sem mexer em código ainda.
>
> <details><summary>Explored 1 file</summary>
>
> - Read `/root/.codex/plugins/cache/openai-curated/superpowers/9da6a7da/skills/brainstorming/SKILL.md`
>
> </details>
>
> Vou olhar rapidinho o desenho atual de MQTT/presença para a avaliação ficar ancorada no que já existe, não em abstração no ar.
>
> <details><summary>Explored 4 files</summary>
>
> - Read `./components/tele_presence/mqtt_presence.c`
> - Read `./docs/manual_mqtt_operacao.md`
> - Read `./components/tele_mqtt/include/tele_mqtt.h`
> - Read `./docs/tele_config.md`
>
> </details>
</details>

Sim, faz bastante sentido. Eu separaria em dois registries irmãos:

**1. `tele_config`: valores configuráveis**
Já começamos isso. Campos têm default, override NVS, validação, flags `WEB/MQTT/SECRET/REBOOT_REQUIRED`.

**2. `tele_status`: valores observáveis**
Novo registry read-only para status: IP, RSSI, uptime, heap, firmware, Wi-Fi state, bateria, sensores do produto, etc. Sem NVS. Cada campo teria `id`, tipo, unidade, flags, e um callback/getter para ler o valor atual.

Minha avaliação: isso é o caminho certo se você quer um painel MQTT genérico e reutilizável.

**Por quê**
Com `tele_config` + `tele_status`, o ESP32 pode publicar nos primeiros contatos uma espécie de “manifesto” do dispositivo:

```text
v1/telecafezinho/{device_id}/meta/config
v1/telecafezinho/{device_id}/meta/status
v1/telecafezinho/{device_id}/state
v1/telecafezinho/{device_id}/availability
```

O painel MQTT lê os manifests e descobre sozinho:

- quais campos existem;
- quais são editáveis;
- tipo/range/unidade;
- quais são secretos;
- quais exigem reboot;
- quais aparecem em heartbeat;
- quais aparecem só em status técnico.

Isso evita hardcode no painel e deixa novos projetos ESP32 “plugáveis”.

**Sobre LWT e “última vez visto”**
Aqui tem uma pegadinha importante: o LWT é definido no momento da conexão MQTT. Então ele consegue publicar `offline` se o dispositivo cair, mas não consegue calcular dinamicamente “a última vez visto” no instante da queda.

A melhor arquitetura seria:

- `availability`, retido:
  - online quando conecta;
  - offline via LWT quando cai;
- `state`, retido:
  - atualizado no boot/conexão e quando algo relevante muda;
  - contém `ts`, `ip`, `rssi`, `wifi_state`, `uptime_s`, etc.;
- `heartbeat`, talvez não retido ou retido dependendo do painel:
  - enviado periodicamente;
  - contém `ts`;
- o painel calcula `last_seen` pelo timestamp mais recente recebido em `state` ou `heartbeat`.

Se quiser que o próprio broker/painel saiba mesmo após reboot do painel, eu tenderia a reter também um tópico simples:

```text
.../{device_id}/last_seen
```

Atualizado a cada heartbeat com payload curto:

```json
{"ts":"2026-06-10T12:34:56Z","uptime_s":1234}
```

Assim o LWT diz “offline”, e `last_seen` retido diz quando foi a última mensagem viva.

**Opções de desenho**
Opção A: manter como está e só melhorar payloads MQTT.
Mais simples, menor mudança, mas painel continua dependente de campos fixos.

Opção B: criar `tele_status` registry e publicar manifests MQTT.
Minha recomendação. É modular, reaproveitável, e combina com o `tele_config`.

Opção C: criar um “tele_device_registry” único para config + status.
Pode parecer elegante, mas mistura persistência/validação de configuração com leitura dinâmica de estado. Eu evitaria por enquanto.

Minha recomendação: seguir com B. Primeiro `tele_status` pequeno, só com campos comuns: firmware, uptime, heap, wifi_state, ip, rssi, time_synchronized, heartbeat_interval_s. Depois conectamos campos de domínio do TeleCafezinho.

Uma pergunta para guiar o desenho: esse painel MQTT genérico você imagina como um painel próprio seu, ou quer compatibilidade com algum ecossistema existente tipo Home Assistant/MQTT discovery?

> esse painel que menciono é ferramenta minha para administrar os equipamentos em campo, ele fica em /workspaces/telecafezinho/tools/mqtt_desktop
>
> e gostaria que evoluisse para se comunicar com o sistema generico que estamos construindo

<details><summary>4 previous messages</summary>

> Perfeito, então o alvo não é Home Assistant/discovery externo, é uma ferramenta operacional sua. Vou olhar o `tools/mqtt_desktop` para avaliar como ele pode virar consumidor desse contrato genérico sem forçar o firmware a carregar decisões de UI.
>
> <details><summary>Explored 1 file, 1 search, 1 list</summary>
>
> - Listed files in `mqtt_desktop`
> - Read `./tools/mqtt_desktop/README.md`
> - Searched for `*.py` in `mqtt_desktop`
>
> </details>
>
> O diretório existe e parece ser uma aplicação Python única, ainda com artefatos do projeto anterior. Vou ler o script e o README para separar o que é “motor MQTT reaproveitável” do que é tela/campo antigo.
>
> <details><summary>Explored 4 files</summary>
>
> - Read `./tools/mqtt_desktop/config.example.json`
> - Read `./tools/mqtt_desktop/esp32_mqtt_desktop.py`
> - Read `./tools/mqtt_desktop/README_MQTT_DESKTOP_MVP.md`
> - Read `./tools/mqtt_desktop/requirements.txt`
>
> </details>
</details>

Ótimo, isso muda a avaliação: o painel não precisa seguir convenção externa; ele pode virar o “console de campo” oficial dos seus projetos.

Minha leitura do `tools/mqtt_desktop`: ele já tem muita coisa valiosa, mas ainda está preso ao projeto ACR por três eixos:

- `BASE_TOPIC = "v1/acr"` hardcoded;
- UI e settings com campos específicos antigos, como `trigger_mode`, `upload_prefix`, etc.;
- descoberta baseada só em tópicos fixos: `status`, `state`, `heartbeat`, `event`, `cmd/out`.

Eu evoluiria em duas camadas.

**Contrato Genérico Do Firmware**
O ESP32 publica manifests retidos:

```text
{base}/{device_id}/meta/config
{base}/{device_id}/meta/status
{base}/{device_id}/availability
{base}/{device_id}/state
{base}/{device_id}/heartbeat
```

`meta/config` viria do `tele_config`: campos editáveis, tipo, range, default, valor efetivo, origem `default|nvs`, flags `secret`, `reboot_required`.

`meta/status` viria de um novo `tele_status`: campos somente leitura, tipo, unidade, grupo, flags `heartbeat`, `state`, `technical`, `sensitive`.

`availability` seria retido e usado com LWT:

```json
{"status":"offline","reason":"lwt","session_id":"..."}
```

E quando online:

```json
{"status":"online","reason":"mqtt_connected","ts":"...","session_id":"..."}
```

Para “última vez visto”, eu não colocaria toda a responsabilidade no LWT. Melhor o desktop calcular pelo último `heartbeat/state/event/cmd_out`, e o firmware também publicar um tópico retido simples:

```text
{base}/{device_id}/last_seen
```

com:

```json
{"ts":"2026-06-10T12:34:56Z","uptime_s":1234}
```

**Evolução Do Desktop**
O `mqtt_desktop` deixaria de ter formulários fixos por projeto e passaria a renderizar:

- lista de devices por wildcard configurável;
- cards/tabela de status baseados em `meta/status`;
- formulário de settings baseado em `meta/config`;
- comandos comuns fixos: `ping`, `get_state`, `get_settings`, `get_technical_status`, `apply_and_reboot`;
- comandos de produto vindos de um futuro `meta/commands`, se quisermos.

Eu faria a migração em etapas, sem reescrever tudo:

1. Tornar `BASE_TOPIC` configurável e trocar branding ACR para TeleCafezinho/genérico.
2. Adicionar suporte a `meta/config`, mantendo fallback para `get_settings`.
3. Adicionar `tele_status` no firmware e suporte a `meta/status` no desktop.
4. Depois limpar telas antigas específicas do ACR.

A decisão central agora é esta: você prefere que o desktop continue sendo uma app única “genérica com renderização dinâmica”, ou quer manter perfis por família de projeto, tipo `TeleCafezinho`, `ACR`, `outro`, cada um podendo customizar telas além do contrato genérico?

> acho que vale a pena reestruturar os topicos que ja existem - não precisa pensar em retrocompatibilidade com ACR,
> vamos tornar base topic configuravel  e o branding para algo mais generico, algo como "Jabur Consulting MQTT control center"

<details><summary>2 previous messages</summary>

> Beleza, sem retrocompatibilidade com ACR fica bem mais limpo. Vou só mapear as referências hardcoded no desktop para propor uma mudança enxuta e não transformar isso numa reforma maior que o necessário.
>
> <details><summary>Explored 2 searches</summary>
>
> - Searched for `ACR|acr|BASE_TOPIC|v1/acr|title\(|config` in `esp32_mqtt_desktop.py`
> - Searched for `TOPICS_TO_SUBSCRIBE|publish_command|clear_retained|cmd/in|cmd/out|status|heartbeat|state|event` in `esp32_mqtt_desktop.py`
>
> </details>
</details>

Perfeito. Eu faria uma primeira fatia bem objetiva, sem ainda entrar no `tele_status/meta`:

**Design Proposto**
1. **Branding do desktop**
   - título da janela: `Jabur Consulting MQTT Control Center`
   - README renomeado conceitualmente para ferramenta genérica, sem ACR
   - remover textos “ACR ESP32 MQTT Desktop MVP” onde forem puramente branding

2. **Base topic configurável**
   No desktop, adicionar em `config.json`:

   {
     "mqtt": {
       "base_topic": "v1/telecafezinho"
     }
   }

   E também um campo na UI para editar isso junto de host/porta/TLS.

3. **Tópicos existentes reestruturados**
   Eu trocaria `status` por `availability`, porque esse é o nome mais claro para LWT/presença:

   {base}/{device_id}/availability
   {base}/{device_id}/state
   {base}/{device_id}/heartbeat
   {base}/{device_id}/event
   {base}/{device_id}/cmd/in
   {base}/{device_id}/cmd/out
```raw

   `availability` seria retido e usado pelo LWT. `state` continua retido. `heartbeat` pode continuar não retido.

4. **Firmware**
   O firmware já tem `CONFIG_MQTT_TOPIC_NAMESPACE`, então a ideia é só ajustar o default e renomear internamente o tópico de `status` para `availability`.

5. **Desktop**
   O parser de tópico deixa de assumir `v1/acr`. Ele passa a:
   - ler `base_topic`;
   - assinar `{base}/+/availability`, `{base}/+/state`, etc.;
   - publicar comandos em `{base}/{device_id}/cmd/in`;
   - limpar retained nos tópicos novos.

Eu recomendo fazer exatamente essa fatia primeiro. Ela limpa a base sem misturar ainda renderização dinâmica de manifests, e prepara o terreno para `tele_config`/`tele_status`.



```


---
Aqui começa o plano que sintetizei. Antes de implementar vamos fazer uma ultima avaliação


Você está trabalhando em um projeto ESP32/ESP-IDF com arquitetura modular, atualmente baseado no projeto TeleCafezinho, e também em uma ferramenta desktop Python localizada em:

`/workspaces/telecafezinho/tools/mqtt_desktop`

O objetivo é evoluir o sistema MQTT para um contrato genérico e reutilizável entre firmware ESP32 e uma aplicação desktop chamada:

`Jabur Consulting MQTT Control Center`

A ferramenta desktop deve ser uma aplicação única, genérica e totalmente dinâmica, sem telas específicas por família de produto. No futuro, extensões opcionais por produto poderão existir, mas devem ser descritas pelo próprio dispositivo via MQTT, sem contaminar o núcleo genérico da aplicação.

Não é necessário manter retrocompatibilidade com o projeto ACR.

---

# Objetivo geral

Implementar uma arquitetura MQTT genérica composta por:

1. base topic configurável;
2. identificação estável de dispositivo;
3. tópicos padronizados;
4. presença via `availability` e LWT;
5. `state`, `heartbeat` e `last_seen`;
6. registry de configuração `tele_config`;
7. novo registry de status `tele_status`;
8. manifests paginados para `meta/config`, `meta/status` e futuramente `meta/commands`;
9. comandos genéricos via `cmd/in` e `cmd/out`;
10. suporte a comandos grandes por transação multipartes;
11. controle de concorrência;
12. desktop dinâmico renderizado a partir dos manifests.

---

# Convenções de tópicos

O `base_topic` deve ser armazenado completo, por exemplo:

```text
v1/telecafezinho
```

A árvore MQTT deve seguir o formato:

```text
{base_topic}/{device_id}/availability
{base_topic}/{device_id}/state
{base_topic}/{device_id}/heartbeat
{base_topic}/{device_id}/last_seen
{base_topic}/{device_id}/event
{base_topic}/{device_id}/cmd/in
{base_topic}/{device_id}/cmd/out
{base_topic}/{device_id}/meta/config/index
{base_topic}/{device_id}/meta/config/{part}
{base_topic}/{device_id}/meta/status/index
{base_topic}/{device_id}/meta/status/{part}
{base_topic}/{device_id}/meta/commands/index
{base_topic}/{device_id}/meta/commands/{part}
```

Substituir qualquer uso antigo de `status` por `availability`.

---

# Identificação do dispositivo

O `device_id` deve continuar sendo formado por:

```text
{id_configurado}-{sufixo_mac}
```

Exemplos:

```text
TeleCafezinho-30D3B8
skips_11-5112D0
```

O ID configurado deve ser sanitizado para uso em tópico MQTT.

Não permitir caracteres problemáticos como:

```text
/
+
#
espaços
caracteres de controle
```

---

# Base topic configurável

No desktop, remover qualquer `BASE_TOPIC` hardcoded, especialmente referências a:

```text
v1/acr
```

Adicionar o campo no arquivo de configuração:

```json
{
  "mqtt": {
    "host": "broker.exemplo.com",
    "port": 1883,
    "base_topic": "v1/telecafezinho"
  }
}
```

Também permitir edição do `base_topic` na interface, junto das configurações do broker.

---

# Branding do desktop

Renomear o branding visual para:

```text
Jabur Consulting MQTT Control Center
```

Remover referências visuais e textuais ao ACR, quando forem apenas branding antigo.

O desktop deve ser uma ferramenta operacional genérica para equipamentos em campo.

---

# Availability e LWT

O tópico:

```text
{base_topic}/{device_id}/availability
```

deve ser retido.

Ao conectar, o dispositivo deve publicar:

```json
{
  "status": "online",
  "reason": "mqtt_connected",
  "ts": "2026-06-10T12:34:56Z",
  "session_id": "..."
}
```

Se o relógio ainda não estiver sincronizado:

```json
{
  "status": "online",
  "reason": "mqtt_connected",
  "ts": null,
  "session_id": "...",
  "time_synchronized": false
}
```

O LWT deve publicar:

```json
{
  "status": "offline",
  "reason": "lwt",
  "session_id": "..."
}
```

Observação importante: o LWT não deve tentar informar dinamicamente a última vez em que o dispositivo foi visto, porque o payload do LWT é definido antes da queda.

---

# Heartbeat e last_seen

O heartbeat padrão deve ser:

```text
60 segundos
```

Deve ser configurável, com limites:

```text
mínimo: 10 segundos
máximo: 3600 segundos
```

A alteração do intervalo de heartbeat não deve exigir reboot.

O tópico:

```text
{base_topic}/{device_id}/heartbeat
```

não deve ser retido.

Payload:

```json
{
  "ts": "2026-06-10T12:34:56Z",
  "uptime_s": 1234,
  "time_synchronized": true
}
```

Se o relógio não estiver sincronizado:

```json
{
  "ts": null,
  "uptime_s": 1234,
  "time_synchronized": false
}
```

O tópico:

```text
{base_topic}/{device_id}/last_seen
```

deve ser retido e atualizado junto ao heartbeat.

Payload igual ou equivalente ao heartbeat mínimo:

```json
{
  "ts": null,
  "uptime_s": 1234,
  "time_synchronized": false
}
```

---

# State

O tópico:

```text
{base_topic}/{device_id}/state
```

deve ser retido.

Ele representa o estado consolidado do equipamento.

Publicar `state`:

1. ao conectar ao broker;
2. quando algum campo relevante mudar;
3. em resposta a `get_state`.

Evitar duplicação excessiva com `heartbeat`.

Regra:

* `state`: estado consolidado;
* `heartbeat`: mensagem curta de vida;
* `last_seen`: versão mínima retida do heartbeat.

---

# tele_config

Manter ou evoluir o registry `tele_config` para representar campos configuráveis.

Cada campo deve descrever, quando aplicável:

```text
id
label
description
type
default
valor efetivo
origem: default | nvs
min
max
unit
options
secret
reboot_required
group
flags
```

Campos `secret` nunca devem publicar valor nem máscara.

Exemplo de manifesto para segredo:

```json
{
  "id": "mqtt_password",
  "secret": true,
  "configured": true
}
```

Nunca publicar:

* valor real;
* string mascarada;
* hash;
* qualquer derivado do segredo.

Ao alterar configurações, se um campo secreto não for enviado, ele deve permanecer inalterado.

Para apagar explicitamente um segredo, usar operação explícita de `clear`, nunca string vazia implícita.

---

# tele_status

Criar um novo registry read-only chamado `tele_status`.

Ele deve ser separado do `tele_config`.

O `tele_status` não deve usar NVS.

Cada campo deve conter:

```text
id
label
description
type
unit
group
flags
getter/callback
```

Campos comuns iniciais recomendados:

```text
firmware_version
uptime_s
free_heap
wifi_state
ip
rssi
time_synchronized
heartbeat_interval_s
```

Flags iniciais possíveis:

```text
STATE
HEARTBEAT
TECHNICAL
SENSITIVE
```

O firmware deve conseguir gerar `meta/status` a partir desse registry.

---

# Manifests paginados

Os manifests podem ultrapassar o tamanho seguro de payload MQTT/JSON.

Portanto, publicar `meta/config`, `meta/status` e `meta/commands` em formato paginado.

Não quebrar JSON arbitrariamente por bytes.

Cada parte deve ser um JSON válido e independente.

Cada campo ou comando deve ficar inteiro dentro de uma parte.

## Índice

Exemplo:

```text
{base_topic}/{device_id}/meta/config/index
```

Payload:

```json
{
  "schema_version": 1,
  "registry_revision": 7,
  "parts": 3,
  "total_fields": 24,
  "content_hash": "..."
}
```

## Partes

Exemplo:

```text
{base_topic}/{device_id}/meta/config/0
{base_topic}/{device_id}/meta/config/1
{base_topic}/{device_id}/meta/config/2
```

Payload:

```json
{
  "schema_version": 1,
  "registry_revision": 7,
  "part": 0,
  "parts": 3,
  "fields": [
    {
      "id": "device_label",
      "type": "string",
      "group": "identity"
    },
    {
      "id": "heartbeat_interval_s",
      "type": "integer",
      "min": 10,
      "max": 3600,
      "unit": "s"
    }
  ]
}
```

O mesmo padrão deve valer para:

```text
meta/status/index
meta/status/{part}

meta/commands/index
meta/commands/{part}
```

## Regras de publicação

Para atualizar um manifest:

1. publicar todas as partes da nova revisão;
2. publicar o novo `index` por último;
3. o desktop só deve montar o registry quando possuir todas as partes da mesma `registry_revision`;
4. o desktop nunca deve misturar partes de revisões diferentes;
5. se a nova revisão tiver menos partes que a anterior, limpar as partes antigas excedentes.

Os tópicos de manifests devem ser retidos.

---

# Limite de tamanho dos payloads

Não inventar um número fixo sem verificar o código.

Antes de definir o tamanho máximo das páginas, inspecionar:

```text
buffer de recepção MQTT
buffer de transmissão MQTT
maximum packet size da biblioteca MQTT
capacidade do JSON document
limite do broker, se configurado
heap disponível no ESP32
```

Depois disso, escolher um tamanho seguro com folga.

Exemplo conceitual:

```text
se o limite técnico for 4 KiB, usar páginas de até aproximadamente 2 KiB
```

Mas o número real deve vir da inspeção do projeto.

---

# Comandos MQTT

Comandos pequenos usam uma única mensagem.

Tópicos:

```text
{base_topic}/{device_id}/cmd/in
{base_topic}/{device_id}/cmd/out
```

Esses tópicos não devem ser retidos.

## Envelope de entrada

```json
{
  "request_id": "desktop-1749562496-001",
  "command": "apply_settings",
  "params": {
    "values": {
      "heartbeat_interval_s": 120
    }
  }
}
```

Usar `command`, não `cmd`, nos payloads.

## Envelope de resposta com sucesso

```json
{
  "request_id": "desktop-1749562496-001",
  "command": "apply_settings",
  "success": true,
  "result": {
    "persisted": [
      "heartbeat_interval_s"
    ],
    "activated": [
      "heartbeat_interval_s"
    ],
    "pending_reboot": [],
    "reboot_required": false
  }
}
```

## Envelope de resposta com erro

```json
{
  "request_id": "desktop-1749562496-001",
  "command": "apply_settings",
  "success": false,
  "error": {
    "code": "validation_error",
    "message": "A configuração não foi aplicada.",
    "fields": {
      "heartbeat_interval_s": {
        "code": "out_of_range",
        "message": "O valor deve estar entre 10 e 3600."
      }
    }
  }
}
```

---

# Comandos genéricos iniciais

Implementar ou preparar suporte para:

```text
ping
get_state
get_settings
get_technical_status
apply_settings
reboot
```

`apply_and_reboot` não precisa existir como comando real no firmware. Preferir tratar no desktop como operação composta:

1. enviar `apply_settings`;
2. aguardar sucesso;
3. enviar `reboot`.

---

# Semântica de apply_settings

`apply_settings` deve permitir enviar um ou vários campos.

Não é necessário enviar a configuração completa.

A transação é integral apenas sobre os campos enviados naquele comando.

Regra:

```text
Campos não enviados permanecem inalterados.
Todos os campos enviados são validados antes de qualquer persistência.
Se qualquer campo enviado for inválido, nenhum campo daquele comando é aplicado.
```

Exemplo válido com um campo:

```json
{
  "request_id": "desktop-001",
  "command": "apply_settings",
  "params": {
    "values": {
      "heartbeat_interval_s": 120
    }
  }
}
```

Exemplo válido com vários campos:

```json
{
  "request_id": "desktop-002",
  "command": "apply_settings",
  "params": {
    "values": {
      "heartbeat_interval_s": 120,
      "device_label": "Recepção",
      "mqtt_port": 8883
    }
  }
}
```

Se qualquer campo falhar, nenhum dos campos enviados deve ser persistido.

Implementação recomendada:

1. receber o comando;
2. montar configuração candidata em RAM;
3. validar campo por campo;
4. validar relações entre campos;
5. persistir somente após validação completa;
6. aplicar em runtime apenas os campos que não exigem reboot;
7. responder com `persisted`, `activated`, `pending_reboot` e `reboot_required`.

---

# Configurações grandes: transação multipartes

Quando o payload for grande demais para uma única mensagem segura, usar transação multipartes.

## settings_begin

```json
{
  "request_id": "desktop-010",
  "command": "settings_begin",
  "params": {
    "transaction_id": "cfg-20260610-001",
    "client_id": "control-center-notebook-mauricio",
    "base_revision": 42,
    "expected_parts": 3,
    "expires_in_s": 60
  }
}
```

Resposta:

```json
{
  "request_id": "desktop-010",
  "command": "settings_begin",
  "success": true,
  "result": {
    "transaction_id": "cfg-20260610-001",
    "accepted": true,
    "current_revision": 42,
    "expires_in_s": 60
  }
}
```

## settings_part

Cada parte deve conter campos completos, não pedaços arbitrários de JSON.

```json
{
  "request_id": "desktop-011",
  "command": "settings_part",
  "params": {
    "transaction_id": "cfg-20260610-001",
    "client_id": "control-center-notebook-mauricio",
    "part": 0,
    "values": {
      "heartbeat_interval_s": 120,
      "device_label": "Recepção"
    }
  }
}
```

Regras:

* repetir uma parte idêntica deve ser aceito;
* repetir uma parte com mesmo número e conteúdo diferente deve ser rejeitado;
* partes fora da faixa devem ser rejeitadas;
* partes de outro `client_id` devem ser rejeitadas;
* partes após timeout devem ser rejeitadas.

## settings_commit

```json
{
  "request_id": "desktop-014",
  "command": "settings_commit",
  "params": {
    "transaction_id": "cfg-20260610-001",
    "client_id": "control-center-notebook-mauricio"
  }
}
```

No commit, o firmware deve:

1. confirmar que todas as partes foram recebidas;
2. montar configuração candidata;
3. validar campos individualmente;
4. validar relações cruzadas;
5. persistir;
6. aplicar campos de efeito imediato;
7. responder.

Se qualquer campo falhar, nada deve ser aplicado.

## settings_abort

```json
{
  "request_id": "desktop-015",
  "command": "settings_abort",
  "params": {
    "transaction_id": "cfg-20260610-001",
    "client_id": "control-center-notebook-mauricio"
  }
}
```

Também deve haver cancelamento automático por timeout.

---

# Concorrência

Permitir apenas uma transação de configuração aberta por dispositivo.

Durante uma transação aberta:

* `ping` deve responder imediatamente;
* `get_state` pode continuar funcionando;
* heartbeats continuam funcionando;
* publicação de status continua funcionando;
* nova transação de configuração deve ser rejeitada;
* `apply_settings` fora da transação deve ser rejeitado;
* comandos perigosos ou que alterem estado persistente devem ser rejeitados ou tratados com cuidado.

Erro de ocupado:

```json
{
  "request_id": "desktop-020",
  "command": "settings_begin",
  "success": false,
  "error": {
    "code": "transaction_busy",
    "message": "Já existe uma transação de configuração em andamento.",
    "transaction_id": "cfg-20260610-001",
    "expires_in_s": 41
  }
}
```

---

# Revisão de configuração

Criar ou expor um contador:

```text
config_revision
```

Ele deve ser incrementado a cada alteração persistida com sucesso.

O desktop deve usar `base_revision` ao iniciar uma transação multipartes.

Se a configuração mudar antes do commit, rejeitar:

```json
{
  "request_id": "desktop-014",
  "command": "settings_commit",
  "success": false,
  "error": {
    "code": "revision_conflict",
    "message": "A configuração foi alterada desde o início da operação.",
    "expected_revision": 42,
    "current_revision": 43
  }
}
```

---

# Registry de comandos

Criar suporte a `meta/commands`, ainda que a primeira versão tenha poucos comandos.

Exemplo:

```json
{
  "schema_version": 1,
  "registry_revision": 3,
  "part": 0,
  "parts": 1,
  "commands": [
    {
      "id": "ping",
      "label": "Testar comunicação",
      "description": "Solicita uma resposta imediata do equipamento.",
      "group": "system",
      "danger_level": "none",
      "confirmation_required": false,
      "params": []
    },
    {
      "id": "reboot",
      "label": "Reiniciar equipamento",
      "description": "Reinicia o equipamento.",
      "group": "system",
      "danger_level": "warning",
      "confirmation_required": true,
      "params": []
    }
  ]
}
```

Tipos iniciais de parâmetros:

```text
string
integer
number
boolean
enum
```

Propriedades iniciais:

```text
id
label
description
type
required
default
min
max
unit
options
secret
```

Níveis de risco:

```text
none
warning
danger
```

Comportamento sugerido no desktop:

* `none`: execução direta;
* `warning`: confirmação simples;
* `danger`: confirmação explícita com nome do dispositivo.

---

# Limpeza de retained

O desktop pode permitir limpeza manual dos seguintes tópicos:

```text
state
last_seen
meta/config
meta/status
```

Regras:

* nunca limpar automaticamente;
* exigir dispositivo selecionado;
* mostrar os tópicos que serão apagados;
* exigir confirmação adicional;
* evitar operação ampla em `{base_topic}/#`.

A limpeza de `meta/commands` pode ser considerada depois, mas não é prioridade.

---

# Desktop dinâmico

O desktop deve:

1. conectar ao broker usando `base_topic` configurável;
2. assinar os tópicos padronizados;
3. descobrir dispositivos por mensagens recebidas;
4. exibir estado online/offline por `availability`;
5. calcular última atividade por `last_seen`, `heartbeat`, `state`, `event` e `cmd/out`;
6. montar formulários de configuração a partir de `meta/config`;
7. montar painéis de status a partir de `meta/status`;
8. montar comandos disponíveis a partir de `meta/commands`;
9. respeitar campos secretos;
10. respeitar campos que exigem reboot;
11. validar campos no desktop antes de enviar, mas sempre considerar o firmware como autoridade final;
12. lidar com manifests paginados;
13. não misturar partes de revisões diferentes.

---

# Ordem recomendada de implementação

Implementar em etapas pequenas.

## Etapa 1 — Limpeza e base genérica

* Remover branding ACR.
* Renomear para `Jabur Consulting MQTT Control Center`.
* Tornar `base_topic` configurável.
* Ajustar parser e subscriptions para usar `{base_topic}`.
* Reestruturar tópicos atuais.
* Trocar `status` por `availability`.

## Etapa 2 — Presença e vida

* Implementar `availability` retido.
* Ajustar LWT.
* Implementar `heartbeat`.
* Implementar `last_seen` retido.
* Tratar relógio não sincronizado com `ts: null`.

## Etapa 3 — Comandos básicos

* Implementar envelope `command`.
* Implementar `ping`.
* Implementar `get_state`.
* Implementar `apply_settings`.
* Implementar resposta padronizada em `cmd/out`.
* Garantir que `cmd/in` e `cmd/out` não sejam retidos.

## Etapa 4 — Transação lógica de configuração

* `apply_settings` integral sobre os campos enviados.
* Configuração candidata em RAM.
* Validação individual.
* Validação cruzada.
* Persistência só após validação completa.
* Resposta com `persisted`, `activated`, `pending_reboot`, `reboot_required`.

## Etapa 5 — Multipartes

* Implementar `settings_begin`.
* Implementar `settings_part`.
* Implementar `settings_commit`.
* Implementar `settings_abort`.
* Implementar timeout.
* Implementar bloqueio de concorrência.
* Permitir `ping` imediato durante transação.
* Usar `config_revision`.

## Etapa 6 — Registries e manifests

* Finalizar ou ajustar `tele_config`.
* Criar `tele_status`.
* Publicar `meta/config` paginado.
* Publicar `meta/status` paginado.
* Publicar `meta/commands` paginado.
* Implementar montagem dinâmica no desktop.

## Etapa 7 — Limpeza e documentação

* Atualizar README.
* Atualizar exemplos de configuração.
* Documentar contrato MQTT.
* Adicionar testes manuais ou automatizados para os principais fluxos.
* Remover resquícios de campos específicos antigos.

---

# Critérios de aceite

A implementação deve ser considerada correta quando:

1. O desktop conecta ao broker com `base_topic` configurável.
2. Não há dependência de `v1/acr`.
3. O título da aplicação é `Jabur Consulting MQTT Control Center`.
4. O dispositivo publica `availability` retido.
5. O LWT publica `offline`.
6. O dispositivo publica `heartbeat`.
7. O dispositivo publica `last_seen` retido.
8. `state` é retido e atualizado na conexão ou em mudança relevante.
9. `apply_settings` aceita um ou vários campos.
10. Campos não enviados permanecem inalterados.
11. Se um campo enviado for inválido, nenhum campo do comando é aplicado.
12. Segredos nunca são publicados.
13. `heartbeat_interval_s` aceita valores entre 10 e 3600 e não exige reboot.
14. `cmd/in` e `cmd/out` não são retidos.
15. `ping` responde imediatamente mesmo durante transação de configuração.
16. Uma segunda transação de configuração é rejeitada enquanto outra está ativa.
17. Manifests paginados usam `index` e partes com `registry_revision`.
18. O desktop não mistura partes de revisions diferentes.
19. O desktop renderiza campos de configuração e status a partir dos manifests, não de hardcode.
20. A limpeza de retained é manual, explícita e limitada ao dispositivo selecionado.

---

# Cuidados importantes

Não transformar a primeira entrega em uma reescrita total se não for necessário.

Priorizar uma sequência incremental.

Sempre que houver dúvida entre simplicidade e abstração excessiva, implementar o menor núcleo genérico que mantenha o contrato correto.

Não criar telas específicas por produto dentro do núcleo do desktop.

Não publicar segredos.

Não usar comandos retidos.

Não executar comandos antigos após reconexão.

Não misturar schemas de revisões diferentes.

Não aplicar configurações parcialmente quando o comando deveria ser transacional.
