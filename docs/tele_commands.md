# tele_commands

`components/tele_commands` centraliza o manifesto de comandos remotos
disponiveis no firmware. Ele e o terceiro par do contrato generico:

- `tele_config`: campos editaveis;
- `tele_status`: campos observaveis;
- `tele_commands`: comandos executaveis.

## Modelo

Cada comando registrado tem:

- `name`: nome usado no payload MQTT `cmd/in`;
- `label`: nome curto para botao;
- `description`: texto de ajuda para ferramentas e hover;
- `group`: agrupamento livre definido pelo projeto, como `system`, `status`,
  `config` ou grupos de dominio;
- flags de exposicao, como `MQTT`, `WEB`, `MUTATING`, `REBOOT_REQUIRED` e
  `INTERNAL`;
- lista opcional de argumentos, com `id`, tipo, obrigatoriedade e limites.

O componente nao executa comandos. Ele apenas registra metadados e gera JSON.
O dispatcher continua em `components/tele_mqtt`, que usa o registry para
publicar `meta/commands` e responder `commands/get`.

## Uso atual

`components/tele_mqtt` registra os comandos base:

- `ping`;
- `get_state`;
- `get_technical_status`;
- `config/get`;
- `commands/get`;
- `config/set`;
- `config/reset`;
- `set_heartbeat_interval`;
- `apply_and_reboot`.

O firmware publica `{base_topic}/{device_id}/meta/commands` como mensagem
retida ao conectar ao broker. O Control Center consome esse manifesto para
mostrar comandos agrupados, argumentos e ajuda por hover.

## Fora de escopo por enquanto

- execucao dinamica de comandos pelo registry;
- validacao visual rica e widgets especificos por tipo em todos os comandos;
- paginacao de manifesto;
- autorizacao por comando.
