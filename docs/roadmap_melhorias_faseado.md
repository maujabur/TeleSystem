# Roadmap de Melhorias Faseado

## Objetivo

Transformar as sugestoes consolidadas em um plano executavel por fases, equilibrando impacto tecnico, risco e esforco.

## Status Consolidado (Abril 2026)

- **FASE 1 (P0) - COMPLETO**: Estabilidade de estado e conectividade — todos 4 itens concluidos com validacao em campo.
- **FASE 2 (P1) - COMPLETO** (exceto fila de LED adiada): Robustez e observabilidade — telemetria Wi-Fi, politica APSTA, hardening de LED para remocao.
- **FASE 3 (P1/P2) - EM ANDAMENTO**: Seguranca (baseline aplicado, NVS adiado) + Audio (ganho ajustavel e histerese concluidos; click/dropout pendente) + Manutencao (nao iniciado).

## Fase 1 (P0) - Estabilidade de estado e conectividade

Objetivo da fase:

- reduzir inconsistencias de estado;
- diminuir comportamento redundante;
- tornar reconexao mais previsivel em campo.

Itens:

1. Formalizar transicoes de estado no wifi_manager
- Status: Concluido.
- adicionar helper de validacao de transicao;
- centralizar mudancas de estado em um unico ponto;
- registrar transicoes invalidas em log.

2. Evitar sync redundante em connectivity_controller
- Status: Concluido.
- aplicar short-circuit quando estado Wi-Fi nao mudou;
- evitar chamadas repetidas de update de portal e LED.

3. Parametrizar retry de STA
- Status: Concluido (Kconfig + NVS + endpoint tecnico, com default preservado).
- mover valor fixo para configuracao (Kconfig e, opcionalmente, NVS);
- manter default atual para preservar comportamento inicial.

4. Snapshot de status Wi-Fi com sincronizacao mais robusta
- Status: Concluido.
- garantir leitura consistente para consumidores do status;
- reduzir risco de estado parcial em cenarios concorrentes.

Criterio de saida:

- transicoes Wi-Fi previsiveis em logs;
- sem regressao de provisionamento AP/STA;
- reducao de chamadas redundantes em caminhos de sync.

## Fase 2 (P1) - Robustez operacional e observabilidade

Objetivo da fase:

- aumentar diagnosabilidade em campo;
- reduzir retrabalho de suporte.

Itens:

1. Telemetria de saude operacional
- Status: Concluido para conectividade (reconexao e transicao invalida); silencio descartado ja exposto no status ACR.
- contador de quedas e reconexoes Wi-Fi;
- contador de transicoes invalidas ignoradas;
- taxa de silencio descartado no pipeline de captura.

2. Politica de APSTA versus STA-only
- Status: Concluido (policy configuravel via NVS/API com modo always_on, auto_timeout e sta_only).
- documentar e implementar regra explicita de permanencia em AP apos conectar STA;
- definir comportamento de desativacao de portal/AP em estado conectado.
- idle extension aplicado: atividade de portal renova a janela APSTA em modo auto_timeout.

3. Evolucao do status_led para fila de comandos
- Status: Adiado (LED marcado como debug-only nesta fase; priorizado hardening para remocao futura).
- substituir estado compartilhado simples por fila de atualizacoes;
- manter compatibilidade de API externa.

Observacao aplicada nesta fase:

- hardening de build para remocao: quando STATUS_LED_ENABLED desativado, o firmware compila com implementacao stub sem backend RMT.

Criterio de saida:

- metricas disponiveis para debug em runtime;
- comportamento AP/STA reproduzivel e documentado.

## Fase 3 (P1/P2) - Seguranca e maturidade de produto

Objetivo da fase:

- melhorar seguranca de credenciais;
- consolidar comportamento de produto em campo.

Itens:

1. Endurecimento de segredos
- avaliar NVS encryption e/ou flash encryption para token ACR;
- revisar exposicao de dados sensiveis via APIs e logs.
- Status parcial: baseline de hardening aplicado (identificadores de rede ocultos por default em APIs tecnicas, erros HTTP internos genericos por default e buffer de logs filtrando DEBUG/VERBOSE; endpoint /api/logs pode ser desativado via Kconfig para release).

2. Fluxo tecnico de limpeza de credenciais/chaves
- endpoint de manutencao para limpar chaves ACR e voltar a defaults;
- restricoes de uso para evitar operacao indevida.

3. Audio: qualidade e configurabilidade
- Status parcial: ganho digital ajustavel concluido com integracao UI/API/NVS; heuristica de silencio com histerese RMS concluida e configuravel; investigacao de click/dropout permanece pendente.
- fechar investigacao de click/dropout com criterio de reproducao.

Criterio de saida:

- estrategia de seguranca definida para producao;
- menos variabilidade de captura em campo.

## Dependencias e ordem recomendada

1. Concluir Fase 1 antes de mexer em politicas de produto dependentes de estado Wi-Fi.
2. Implementar telemetria basica no inicio da Fase 2 para medir impacto das mudancas.
3. Endurecimento de seguranca deve considerar restricoes de hardware e estrategia de OTA.

## Riscos

- Mudancas em estado Wi-Fi podem impactar provisionamento em dispositivos ja em campo.
- Ajustes de audio podem alterar sensibilidade do trigger ACR.
- Endurecimento de seguranca sem plano de suporte pode dificultar manutencao remota.

## Definicao de pronto por fase

- codigo aplicado;
- validacao funcional minima em AP, STA e ciclo ACR;
- documentacao atualizada nos arquivos de arquitetura afetados;
- item marcado no todo.txt com status claro.

## Itens Adiados e Justificativa (Fase Piloto)

### Migracao para fila no LED (Fase 2)
- **Motivo do adiamento**: STATUS_LED_ENABLED ja usa stub em build-time para remocao; migracao para fila e uma otimizacao arquitetural em contexto de debug-only.
- **Status**: Disponivel para fase pos-piloto.

### Criptografia de NVS (Fase 3)
- **Motivo do adiamento**: Escala piloto de 20 unidades justifica estrategia de rotacao de token antes de hardening criptografico; fluxo de OTA e suporte de campo ficam mais simples.
- **Status**: Alvo de upgrade de seguranca para pos-piloto; depende de infraestrutura de OTA.

### Melhorias de audio (Fase 3)
- Ganho digital ajustavel: concluido via UI/API/NVS e aplicado no pipeline de captura.
- Heuristica de silencio (histerese): concluida com histerese RMS configuravel para estabilizar classificacao de active_ms.
- Investigacao de click/dropout: pendente de cenario reproduzivel e ajuste de buffers.
- **Motivo**: item remanescente tem prioridade menor que a baseline de conectividade/seguranca para MVP e pode permanecer adiado, salvo evidencia de campo que justifique reabertura.

## Proximas Acoes Executaveis

1. **Validar build release em hardware ESP32-S3**: verificar se endpoint /logs retorna 404, SSID/IP ocultos e mensagens de erro genericas.
2. **[P2/Fase 3] Endpoint de limpeza ACR**: endpoint tecnico de manutencao para reset de credenciais.
3. **Validacao de producao**: teste em campo com build release no hardware piloto.
4. **Click/dropout**: Reabrir apenas se houver reproducao confiavel em campo/bancada.