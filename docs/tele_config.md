# tele_config

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

## Uso atual

`components/tele_wifi/device_config_store.c` ja usa `tele_config` para:

- `wifi.provisioning_ssid`;
- `wifi.sta_max_retry`;
- `wifi.apsta_policy`;
- `wifi.apsta_grace_period_s`.

As funcoes antigas `device_config_store_*` continuam existindo para manter
compatibilidade com o portal e com os pontos de leitura de configuracao, mas
agora elas chamam `tele_config` internamente. Fluxos de atualizacao MQTT usam o
comando generico `config/set`, que chama `tele_config_update_value()`: valida,
aplica callback opcional de runtime e persiste o override.
O comando `config/reset` remove o override e reaplica o default quando existe
callback de runtime.

## Decisoes abertas

- Permitir comandos transacionais para atualizar varios campos de uma vez
  quando isso for realmente necessario.
- Criar adaptadores web genericos para formularios simples, mantendo paginas
  especificas quando a experiencia precisar ser melhor.
- Decidir politica de migracao dos namespaces NVS antigos. Como a versao
  anterior persistia defaults automaticamente, uma migracao cega poderia
  transformar default antigo em override permanente.
