# Plano de OTA Remoto por HTTPS

## Objetivo

Este documento registra um plano futuro para permitir que o firmware consulte uma URL remota, descubra se existe uma versao mais nova, baixe o `.bin` por HTTPS e aplique OTA automaticamente ou sob comando remoto.

Este plano nao esta implementado ainda. A intencao e deixar o desenho tecnico pronto para quando o piloto precisar de atualizacao remota coordenada, sem voltar a depender de certificados hardcoded por servico.

## Contexto: validacao TLS atual

O firmware passou a usar o bundle publico de CAs do ESP-IDF para validar endpoints HTTPS. Tecnicamente, isso e feito preenchendo o campo `crt_bundle_attach` da configuracao HTTP:

```c
esp_http_client_config_t http_config = {
    .url = url,
    .crt_bundle_attach = esp_crt_bundle_attach,
};
```

Esse mecanismo usa o conjunto de autoridades certificadoras embutido pelo ESP-IDF/mbedTLS, em vez de um PEM fixo mantido pelo projeto. A validacao TLS continua existindo: o dispositivo verifica a cadeia apresentada pelo servidor, valida datas, assinatura, hostname/SNI e ancora de confianca dentro do bundle. A diferenca e que a ancora de confianca nao e mais um certificado especifico do servico, e sim uma CA publica reconhecida pelo bundle.

Essa mudanca evita acoplamento a um PEM fixo de provedor. Se um servico HTTPS
valido trocar a cadeia intermediaria, o firmware continua validando pelo bundle
publico do ESP-IDF:

```text
service.example.com
-> CA intermediaria publica
-> CA raiz publica
```

Com o PEM fixo, o handshake TLS falhava quando a cadeia do provedor mudava. Com `esp_crt_bundle_attach`, o firmware passa a aceitar cadeias publicas validas presentes no bundle, como Amazon Trust Services, Let's Encrypt, Google Trust Services, DigiCert e outras CAs comuns incluidas na configuracao do ESP-IDF.

O mesmo padrao ja e usado no OTA por URL existente em `firmware_ota_start()`. Portanto, baixar firmware de uma URL HTTPS publica se tornou uma possibilidade pratica sem introduzir um novo certificado hardcoded no firmware.

## O que o firmware ja tem

O firmware ja possui:

- particoes OTA;
- upload manual de `.bin` via pagina `/ota`;
- funcao `firmware_ota_start(url)` para OTA por URL;
- uso de `esp_https_ota()`;
- validacao HTTPS via `esp_crt_bundle_attach`;
- status OTA exposto por `/api/ota/status`;
- versao atual em `APP_VERSION_STRING`, publicada em `/api/status`, paginas tecnicas e MQTT.

O que falta e a camada de descoberta, politica e seguranca operacional em torno do update remoto.

## Modelo recomendado: manifesto

Em vez de o dispositivo tentar descobrir um `.bin` diretamente, o desenho recomendado e consultar um manifesto HTTPS pequeno.

Exemplo:

```json
{
  "schema": 1,
  "artifact_type": "firmware",
  "channel": "pilot",
  "version": "0.6.10",
  "build_id": "2026-06-04T23:10:00Z-0.6.10",
  "url": "https://updates.example.com/telecafezinho/0.1.1/TeleCafezinho.bin",
  "sha256": "hexadecimal_sha256_do_binario",
  "size": 1286144,
  "min_version": "0.6.8",
  "critical": false,
  "notes": "Corrige validacao OTA e melhora logs"
}
```

O firmware baixa o manifesto, valida o JSON, compara a versao, verifica restricoes, baixa o `.bin`, calcula/verifica SHA-256 e so entao chama o fluxo OTA.

## Avaliacao de componentes externos

O repositorio `maujabur/mozilla_ca_spiffs_updater` contem componentes ESP-IDF
publicos que podem servir de base ou referencia:

- `manifest_file_updater`: componente generico que baixa um manifesto HTTPS,
  valida `schema`, `artifact_type`, `channel`, `version`, `url`, `sha256` e
  `size`, baixa o artefato, confere tamanho/SHA-256 e chama um callback
  `apply()` com o arquivo verificado.
- `ca_manager`: componente especifico para armazenar, validar e ativar bundle
  de CAs do ESP-IDF em SPIFFS.
- `ca_manifest_updater`: adaptador especifico que combina
  `manifest_file_updater` + `ca_manager` para atualizar `artifact_type =
  "ca_bundle"`.

Para o TeleCafezinho, a parte mais reaproveitavel hoje e o contrato de
manifesto e a validacao generica de metadados. O componente
`manifest_file_updater` e uma boa referencia, mas no desenho atual ele baixa o
artefato inteiro para um arquivo temporario antes de aplicar.

Isso e adequado para artefatos pequenos/medios, como `bundle_ca.bin`, mas nao e
ideal para firmware OTA: o `partitions.csv` atual nao tem SPIFFS/FAT grande o
suficiente para armazenar um `.bin` inteiro alem dos dois slots OTA. Criar essa
area temporaria consumiria espaco que hoje pertence ao firmware.

Direcao recomendada para firmware:

1. Manter o formato de manifesto compativel com `manifest_file_updater`,
   usando `artifact_type = "firmware"`.
2. Implementar em `components/tele_system` um fluxo streaming, por exemplo
   `firmware_ota_start_manifest(url)`.
3. Baixar o manifesto pequeno em RAM.
4. Baixar o `.bin` por HTTPS em blocos, escrevendo diretamente na particao OTA
   com `esp_ota_write`.
5. Calcular SHA-256 durante o download.
6. Chamar `esp_ota_set_boot_partition()` somente se tamanho e SHA-256 baterem.

Direcao recomendada para evoluir o outro repositorio:

- adicionar ao `manifest_file_updater` uma API de streaming, sem arquivo
  temporario obrigatorio;
- manter a API atual baseada em arquivo para CA bundle e outros artefatos;
- expor callback incremental, por exemplo `begin/write/finish/abort`, ou um
  callback que receba blocos verificados parcialmente enquanto o componente
  calcula SHA-256 global;
- permitir que consumidores como OTA escrevam direto no destino final
  transacional, sem exigir filesystem intermediario.

Exemplo de dependencia se o componente generico for usado diretamente no futuro:

```yaml
dependencies:
  manifest_file_updater:
    git: https://github.com/maujabur/mozilla_ca_spiffs_updater.git
    path: components/manifest_file_updater
    version: lib-v0.1.0
```

Enquanto o componente nao tiver modo streaming, a recomendacao e nao adiciona-lo
ao firmware apenas para OTA de firmware. Use-o como referencia de contrato e
validacao, ou evolua o componente no repositorio de origem antes de integrar.

## Comparacao de versao

A string humana atual de firmware e util para debug, mas ruim para automacao:

```text
0.3.5 TeleCafezinho status manifest
```

Para OTA remoto, o ideal e separar:

- `APP_VERSION_SEMVER`: exemplo `0.6.9`;
- `APP_VERSION_LABEL`: exemplo `base cleanup`;
- `APP_BUILD_ID`: timestamp, hash curto ou numero de build.

O manifesto deve comparar uma versao normalizada, nao texto livre. A UI pode continuar exibindo uma string amigavel composta.

## Fluxo proposto

1. Dispositivo inicia normalmente.
2. Modulo OTA remoto carrega configuracao:
   - URL do manifesto;
   - canal (`pilot`, `stable`, `dev`);
   - auto-check habilitado ou nao;
   - auto-apply habilitado ou nao.
3. Em evento manual, MQTT ou timer, o dispositivo baixa o manifesto por HTTPS.
4. Valida:
   - HTTPS com CA bundle;
   - JSON bem formado;
   - `schema` suportado;
   - `version` maior que a atual;
   - `min_version` satisfeita;
   - URL HTTPS valida;
   - tamanho plausivel;
   - campo `sha256` presente.
5. Checa condicoes locais:
   - Wi-Fi conectado e estavel;
   - nenhum fluxo critico de produto em andamento;
   - bateria/power-good OK;
   - heap suficiente;
   - nenhuma OTA em andamento;
   - janela operacional permitida.
6. Baixa o `.bin` por HTTPS.
7. Verifica SHA-256 do binario baixado.
8. Aplica OTA.
9. Reinicia.
10. No boot seguinte, confirma versao e saude basica.

## Acionamento recomendado por fases

### Fase 1: check manual por MQTT

Adicionar comando remoto:

```json
{
  "cmd": "ota_check",
  "args": {
    "manifest_url": "https://updates.example.com/telecafezinho/pilot/manifest.json"
  }
}
```

Resposta esperada:

```json
{
  "ok": true,
  "current_version": "0.6.9",
  "available": true,
  "target_version": "0.6.10",
  "critical": false
}
```

Essa fase nao aplica update automaticamente. Ela so prova descoberta, TLS, JSON e comparacao.

### Fase 2: apply manual por MQTT

Adicionar comando:

```json
{
  "cmd": "ota_apply",
  "args": {
    "manifest_url": "https://updates.example.com/telecafezinho/pilot/manifest.json"
  }
}
```

O dispositivo deve recusar se:

- existe fluxo critico de produto em andamento;
- Wi-Fi esta instavel;
- bateria esta baixa;
- versao alvo nao e maior;
- hash ausente ou invalido;
- URL nao e HTTPS;
- OTA ja esta em andamento.

### Fase 3: auto-check periodico

Adicionar uma tarefa leve que consulta o manifesto a cada N horas, com jitter para evitar todos os dispositivos consultarem ao mesmo tempo.

Configuracoes possiveis:

- `ota_remote_enabled`;
- `ota_manifest_url`;
- `ota_check_interval_s`;
- `ota_auto_apply_enabled`;
- `ota_channel`.

No piloto, a recomendacao e habilitar auto-check, mas manter auto-apply desligado.

### Fase 4: auto-apply controlado

Permitir auto-apply apenas quando:

- manifesto tem `critical=true`, ou
- dispositivo pertence a um grupo de rollout, ou
- configuracao remota autoriza.

O rollout pode usar hash do `device_id` ou sufixo do MAC para dividir em grupos.

## Seguranca

HTTPS com CA bundle protege contra conexao com servidores que nao apresentam cadeia publica valida para o hostname. Isso e necessario, mas nao suficiente para update remoto robusto.

Riscos restantes:

- bucket/servidor comprometido;
- manifesto alterado por alguem com acesso ao servidor;
- erro operacional publicando binario errado;
- downgrade acidental;
- troca indevida de canal (`dev` para `pilot`);
- update aplicado em momento ruim.

Mitigacoes recomendadas:

- exigir SHA-256 no manifesto;
- recusar downgrade;
- recusar URL sem HTTPS;
- manter allowlist de host ou prefixo de URL;
- separar `pilot`, `stable` e `dev`;
- logar versao atual, alvo, URL e hash;
- opcional futuro: assinar manifesto com chave privada offline e verificar assinatura no firmware.

A verificacao de assinatura do manifesto e mais forte do que depender apenas do HTTPS, porque protege mesmo se o servidor de arquivos for alterado indevidamente. Para o piloto, SHA-256 no manifesto + HTTPS + controle de acesso ao servidor provavelmente e suficiente. Para escala maior, assinatura do manifesto vira recomendada.

## Armazenamento do manifesto

Opcoes viaveis:

- GitHub Releases;
- S3 + CloudFront;
- servidor proprio com TLS publico;
- storage estatico com URL versionada.

Preferencias:

- URLs imutaveis para binarios versionados;
- manifesto pequeno e atualizavel;
- cache control conservador para o manifesto;
- checksums publicados junto dos artefatos.

## Interacao com produto e MQTT

OTA nao deve competir com fluxos criticos de produto. O update remoto deve:

- consultar o estado do modulo de produto, quando existir;
- nao iniciar durante atuacao ou comunicacao critica;
- pausar o loop automatico ou marcar estado de manutencao;
- publicar evento MQTT antes de iniciar;
- publicar falha/sucesso quando possivel;
- reiniciar apenas apos gravacao bem sucedida.

Durante OTA, e aceitavel suspender MQTT/portal de forma controlada, mas a
decisao deve ser explicita. Se o update for acionado via MQTT, e normal perder a
conexao durante download/restart; a resposta final pode ficar para o boot
seguinte via `status`/`event`.

## API/Comandos futuros

HTTP local possivel:

- `GET /api/ota/remote/status`;
- `POST /api/ota/remote/check`;
- `POST /api/ota/remote/apply`.

MQTT possivel:

- `ota_check`;
- `ota_apply`;
- `ota_cancel` se ainda estiver antes da gravacao;
- `ota_config_set` para URL/canal/auto-check.

Eventos MQTT sugeridos:

- `ota_check_started`;
- `ota_update_available`;
- `ota_no_update`;
- `ota_download_started`;
- `ota_download_failed`;
- `ota_hash_failed`;
- `ota_apply_started`;
- `ota_apply_success_restarting`;
- `ota_apply_failed`.

## Campos de status recomendados

```json
{
  "ota_remote": {
    "enabled": true,
    "auto_apply_enabled": false,
    "manifest_url": "https://updates.example.com/telecafezinho/pilot/manifest.json",
    "last_check_ms": 123456,
    "last_result": "update_available",
    "current_version": "0.6.9",
    "target_version": "0.6.10",
    "in_progress": false,
    "last_error": ""
  }
}
```

## Pros

- reduz necessidade de acesso fisico ou portal local;
- permite corrigir bugs de campo rapidamente;
- aproveita a infraestrutura OTA ja existente;
- evita certificados hardcoded por usar CA bundle;
- pode ser acionado por MQTT no piloto;
- permite rollout gradual;
- torna o parque mais facil de manter.

## Contras

- aumenta superficie de risco de seguranca;
- exige disciplina de versionamento;
- exige infraestrutura de publicacao de binarios;
- precisa lidar com update interrompido, energia ruim e rede ruim;
- pode quebrar dispositivos remotamente se uma versao ruim for publicada;
- exige telemetria clara para saber quem atualizou e quem falhou.

## Decisao recomendada para o piloto

Nao implementar auto-update silencioso agora.

Implementar primeiro, quando necessario:

1. manifesto HTTPS;
2. comando MQTT `ota_check`;
3. comando MQTT `ota_apply`;
4. verificacao SHA-256;
5. recusa por estado inseguro;
6. logs/eventos claros.

Depois de validado, adicionar auto-check periodico. Auto-apply deve ficar para quando houver confianca no fluxo de rollback/saude pos-boot e na infraestrutura de publicacao.
