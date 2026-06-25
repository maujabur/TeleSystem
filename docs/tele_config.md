# tele_config

Guia do grupo: [componentes_mqtt_config_status_commands.md](componentes_mqtt_config_status_commands.md).

## Objetivo

`components/tele_config` centraliza campos de configuracao reutilizaveis entre
projetos ESP-IDF. A regra principal e:

- o valor default vem do menuconfig/codigo;
- a NVS guarda apenas override explicito;
- se nao existir override na NVS, o valor efetivo e o default;
- ler um valor nunca persiste automaticamente o default na NVS.

Isso evita transformar uma configuracao de fabrica em estado gravado e permite
alterar defaults em firmware novo sem que dispositivos limpos fiquem presos a
um valor antigo.

## Modelo de campo

Cada campo registrado tem:

- `id`: nome estavel e legivel para web, MQTT, logs e documentacao;
- `nvs_key`: chave curta para NVS, com no maximo 15 caracteres;
- tipo: `bool`, `i32`, `u32`, `string` ou `enum`;
- default;
- limites numericos ou tamanho minimo/maximo de string;
- flags de exposicao, como `WEB`, `MQTT`, `SECRET`, `REBOOT_REQUIRED` e
  `READ_ONLY`.

O `id` nao precisa respeitar o limite da NVS. Por isso `wifi.sta_max_retry`
pode existir como API publica, enquanto a persistencia usa `w_retry`.

## Registro

Projetos registram campos chamando `tele_config_register_fields()` com uma
tabela estatica de `tele_config_field_t`. O componente nao copia strings, entao
`id`, `nvs_key`, labels de `choices` e strings default devem viver enquanto o
firmware estiver rodando.

Exemplo reduzido:

```c
static const tele_config_enum_choice_t s_wifi_policy_choices[] = {
    {.value = 0, .label = "always_on"},
    {.value = 1, .label = "auto_timeout"},
    {.value = 2, .label = "sta_only"},
};

static const tele_config_field_t s_fields[] = {
    {
        .id = "wifi.apsta_policy",
        .nvs_key = "w_apsta",
        .type = TELE_CONFIG_TYPE_ENUM,
        .default_value.i32 = 1,
        .min.i32 = 0,
        .max.i32 = 2,
        .choices = s_wifi_policy_choices,
        .choice_count = 3,
        .flags = TELE_CONFIG_FLAG_WEB |
                 TELE_CONFIG_FLAG_MQTT |
                 TELE_CONFIG_FLAG_REBOOT_REQUIRED,
    },
};
```

Campos com efeito imediato podem receber callback por
`tele_config_set_apply_handler()`. Quando `tele_config_update_value()` e
chamado, o componente valida, grava o override e tenta aplicar o valor via
callback. O resultado informa:

- `stored`: override salvo em NVS;
- `applied`: callback runtime executado com sucesso;
- `requires_reboot`: o campo declara que o efeito completo depende de reboot.

## Manifesto MQTT

`tele_mqtt` chama `tele_config_add_manifest_to_json(root,
TELE_CONFIG_FLAG_MQTT)` para publicar `{base_topic}/{device_id}/meta/config`.
O manifesto e retained e descreve apenas campos com flag `MQTT`.

Para cada campo, o JSON inclui:

- `id`, `type`, `source`, `default` e `value`;
- `min`/`max` para numericos e enum;
- `min_len`/`max_len` para strings;
- `choices` para enum, quando declaradas;
- `flags`, incluindo `web`, `mqtt`, `secret`, `read_only`,
  `runtime_apply` e `reboot_required`.

`source` indica se o valor efetivo veio de `default` ou `nvs`. Campos `secret`
nao devem expor valor real para a UI; o consumidor deve tratar o valor como
opaco.

As flags de canal seguem o mesmo contrato dos registries de status e comandos:
`MQTT` e `WEB` sao os dois primeiros bits; flags especificas de cada registry
vêm depois.

## Fluxo MQTT

O Control Center e outras ferramentas nao precisam conhecer campos de produto.
Elas leem `meta/config` e usam comandos genericos:

- `config/get`: solicita o manifesto atual;
- `config/set`: recebe `{ "id": "...", "value": ... }`;
- `config/reset`: recebe `{ "id": "..." }` e remove o override.

Depois de `config/set` ou `config/reset` bem-sucedido, `tele_mqtt` republica
`meta/config`, permitindo que a UI atualize origem, valor efetivo e flags sem
adivinhar estado interno.

## Uso no TeleSystem

`components/tele_wifi/device_config.c` registra no `tele_config` os campos:

- `wifi.provisioning_ssid`;
- `wifi.sta_max_retry`;
- `wifi.apsta_policy`;
- `wifi.apsta_grace_period_s`.

Consumidores leem valores efetivos diretamente com `tele_config_get_effective()`.
Fluxos de atualizacao por MQTT ou portal usam o comando generico `config/set`,
que chama `tele_config_update_value()`: valida, aplica callback opcional de
runtime e persiste o override. O comando `config/reset` remove o override e
reaplica o default quando existe callback de runtime.

## Fora de escopo por enquanto

- Permitir comandos transacionais para atualizar varios campos de uma vez
  quando isso for realmente necessario.
- Criar adaptadores web genericos para formularios simples, mantendo paginas
  especificas quando a experiencia precisar ser melhor.
- Migracao automatica de namespaces NVS antigos. Como defaults nao sao
  persistidos automaticamente, uma migracao cega poderia transformar default
  antigo em override permanente.
