# Indice de Arquitetura

## Objetivo

Este arquivo consolida os documentos de arquitetura do firmware e sugere uma ordem de leitura para entender o sistema de ponta a ponta.

## Planejamento de melhorias

- [roadmap_melhorias_faseado.md](roadmap_melhorias_faseado.md)

## Status da documentacao no piloto

Esses documentos continuam essenciais para manter o firmware e a operacao:

- [main_raiz_estrutura_alto_nivel.md](main_raiz_estrutura_alto_nivel.md)
- [main_app_estrutura_alto_nivel.md](main_app_estrutura_alto_nivel.md)
- [main_connectivity_estrutura_alto_nivel.md](main_connectivity_estrutura_alto_nivel.md)
- [main_portal_estrutura_alto_nivel.md](main_portal_estrutura_alto_nivel.md)
- [main_audio_estrutura_alto_nivel.md](main_audio_estrutura_alto_nivel.md)
- [wifi_manager_architecture.md](wifi_manager_architecture.md)
- [status_led_system.md](status_led_system.md)

Documentos de apoio que continuam uteis, mas nao precisam ser lidos em todo fluxo:

- [audio_acr_loop_architecture.md](audio_acr_loop_architecture.md)
- [home_page_branding_manual.md](home_page_branding_manual.md)
- [plano_ota_remoto_https.md](plano_ota_remoto_https.md)
- [roadmap_melhorias_faseado.md](roadmap_melhorias_faseado.md)

Documentos temporarios ou de planejamento pontual:

- [planos/plan.md](planos/plan.md)
- [plano_reaproveitamento_proximo_projeto_led.md](plano_reaproveitamento_proximo_projeto_led.md)

Regra pratica no piloto: se o documento descreve boot, conectividade, portal, audio ou o ciclo ACR atual, ele continua necessario. Se ele apenas registra uma ideia futura ou uma variacao de UI, ele deve ser tratado como apoio e revisado antes de virar referencia principal.

## Leitura recomendada

1. Visao de composicao e boot
- [main_raiz_estrutura_alto_nivel.md](main_raiz_estrutura_alto_nivel.md)

2. Nucleo de aplicacao (ACR)
- [main_app_estrutura_alto_nivel.md](main_app_estrutura_alto_nivel.md)

3. Conectividade e provisionamento
- [main_connectivity_estrutura_alto_nivel.md](main_connectivity_estrutura_alto_nivel.md)

4. Portal web e captive portal
- [main_portal_estrutura_alto_nivel.md](main_portal_estrutura_alto_nivel.md)

5. Captura e processamento de audio
- [main_audio_estrutura_alto_nivel.md](main_audio_estrutura_alto_nivel.md)

## Mapa rapido por tema

### Boot e composicao geral
- [main_raiz_estrutura_alto_nivel.md](main_raiz_estrutura_alto_nivel.md)

### Fluxo ACR, controle e runtime
- [main_app_estrutura_alto_nivel.md](main_app_estrutura_alto_nivel.md)
- [audio_acr_loop_architecture.md](audio_acr_loop_architecture.md)

### Wi-Fi, provisionamento e LED de conectividade
- [main_connectivity_estrutura_alto_nivel.md](main_connectivity_estrutura_alto_nivel.md)
- [wifi_manager_architecture.md](wifi_manager_architecture.md)
- [status_led_system.md](status_led_system.md)

### Portal web e interface HTTP
- [main_portal_estrutura_alto_nivel.md](main_portal_estrutura_alto_nivel.md)
- [home_page_branding_manual.md](home_page_branding_manual.md)

### Audio (captura, WAV e probe)
- [main_audio_estrutura_alto_nivel.md](main_audio_estrutura_alto_nivel.md)

## Relacao entre subsistemas

- main raiz inicializa e conecta todos os modulos.
- connectivity decide modo de rede e aciona portal.
- portal expõe APIs e paginas para configuracao/observabilidade.
- app executa o ciclo ACR e usa conectividade para envio cloud.
- audio entrega PCM/WAV para o pipeline ACR.

## Sugestao de manutencao

Quando houver mudanca estrutural:

1. atualizar primeiro o documento da pasta afetada;
2. depois revisar [main_raiz_estrutura_alto_nivel.md](main_raiz_estrutura_alto_nivel.md) se houve impacto de composicao;
3. por fim ajustar este indice para manter navegacao consistente.
