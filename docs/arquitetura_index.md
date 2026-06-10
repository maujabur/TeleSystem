# Indice de Arquitetura

## Objetivo

Este arquivo aponta para os documentos ainda relevantes depois da limpeza do
TeleCafezinho. Documentos do projeto anterior foram removidos para evitar que o
codigo atual seja guiado por rotas, estados ou modulos que nao existem mais.

## Leitura recomendada

1. [main_raiz_estrutura_alto_nivel.md](main_raiz_estrutura_alto_nivel.md)
2. [main_connectivity_estrutura_alto_nivel.md](main_connectivity_estrutura_alto_nivel.md)
3. [main_portal_estrutura_alto_nivel.md](main_portal_estrutura_alto_nivel.md)
4. [manual_mqtt_operacao.md](manual_mqtt_operacao.md)
5. [status_led_system.md](status_led_system.md)
6. [plano_ota_remoto_https.md](plano_ota_remoto_https.md)
7. [roadmap_atual.md](roadmap_atual.md)

## Guias complementares

- [home_page_branding_manual.md](home_page_branding_manual.md)

## Relacao entre subsistemas atuais

- `main/main.c` inicializa NVS, VBAT, OTA, portal, conectividade e MQTT.
- `main/connectivity` orquestra conectividade e rotas de configuracao.
- `components/tele_wifi` contem Wi-Fi, provisionamento, credenciais, botao de
  boot e NTP como base reutilizavel.
- `components/tele_mqtt` contem o cliente MQTT reutilizavel, sem depender do
  dominio TeleCafezinho.
- `components/tele_presence` adapta a presenca MQTT para os dados atuais do
  firmware.
- `components/status_led` contem o driver/stub de LED de status reutilizavel.
- `components/tele_portal` serve as paginas embarcadas e APIs base de status,
  conectividade, logs, Wi-Fi, OTA e restart.
- `components/tele_system` contem versao de firmware, OTA, bateria e
  power-good.

## Regra de manutencao

Quando uma nova feature de dominio entrar, documente primeiro a interface
publica do modulo e depois atualize este indice. Planos longos devem ficar no
roadmap ou em documento explicitamente marcado como futuro.
