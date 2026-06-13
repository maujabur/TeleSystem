# Roadmap atual

## Objetivo

Este arquivo substitui o `todo.txt` antigo e concentra apenas proximos passos
que ainda fazem sentido para o TeleCafezinho depois da limpeza e extracao dos
componentes.

## Proximos passos imediatos

1. Validar em hardware real:
   - boot;
   - provisionamento Wi-Fi;
   - portal captive;
   - troca STA/APSTA;
   - LED de status;
   - VBAT/POWER_GOOD, se o hardware final usar bateria.
2. Validar perfil release:
   - `/api/logs` indisponivel quando desabilitado;
   - SSID/IP ocultos quando configurado;
   - erros HTTP genericos;
   - MQTT sem dados sensiveis desnecessarios.
3. Definir o primeiro modulo de dominio TeleCafezinho:
   - sensores e atuadores reais;
   - comandos MQTT de produto;
   - endpoints HTTP de produto;
   - estados visuais do LED relacionados ao produto.

## Pendencias tecnicas

- Evoluir `components/tele_config` com adaptadores genericos para MQTT e web,
  conforme [tele_config.md](tele_config.md).
- Persistir `mqtt.heartbeat_interval_s` via `tele_config`, caso o ajuste remoto
  por MQTT deva sobreviver a reboot.
- Decidir se valores do namespace NVS legado `provisioning` devem ser migrados
  manualmente; migracao automatica pode transformar defaults antigos em
  overrides permanentes.
- Avaliar NVS encryption/flash encryption para credenciais Wi-Fi e MQTT antes
  de piloto maior.
- Definir autenticacao/autorizacao de comandos MQTT alem do isolamento por
  topico/broker.
- Avaliar evolucao do componente externo `manifest_file_updater` para modo
  streaming antes de usa-lo no OTA remoto de firmware.
- Evoluir `tools/mqtt_desktop` para usar os manifests `tele_config`,
  `tele_status` e `tele_commands` tambem na criacao de formularios e acoes
  mais ricas, reduzindo os campos especificos herdados do projeto anterior.

## Evolucoes futuras

- Implementar OTA remoto por manifesto HTTPS, conforme
  [plano_ota_remoto_https.md](plano_ota_remoto_https.md).
- Aplicar branding final da pagina inicial, seguindo
  [home_page_branding_manual.md](home_page_branding_manual.md).
- Criar testes de bancada documentados para portal, MQTT e OTA antes de piloto.
