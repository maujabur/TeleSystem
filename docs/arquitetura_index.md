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

## Planejamento e historico

- [plano_reaproveitamento_proximo_projeto_led.md](plano_reaproveitamento_proximo_projeto_led.md)
- [plano_mqtt_dispositivos_distribuidos.md](plano_mqtt_dispositivos_distribuidos.md)
- [home_page_branding_manual.md](home_page_branding_manual.md)

## Relacao entre subsistemas atuais

- `main/main.c` inicializa NVS, VBAT, OTA, portal, conectividade e MQTT.
- `main/connectivity` concentra Wi-Fi, configuracao de dispositivo, NTP,
  MQTT de presenca e LED de status.
- `components/tele_mqtt` contem o cliente MQTT reutilizavel, sem depender do
  dominio TeleCafezinho.
- `main/portal` serve as paginas embarcadas e APIs base de status,
  conectividade, logs, Wi-Fi, OTA e restart.
- `main/app` mantem utilitarios gerais de firmware, bateria, power-good e OTA.

## Regra de manutencao

Quando uma nova feature de dominio entrar, documente primeiro a interface publica
do modulo e depois atualize este indice. Evite documentar planos longos como se
fossem arquitetura atual.
