# VU AOT experimental

O runner agora aceita `PS2X_VU_AOT=1`. O caminho gerado substitui somente os corpos de pares suportados; o loop existente continua cuidando de dependências, flags, filas, delay slots, budgets, paradas e XGKICK. Pares não suportados e imagens desconhecidas usam o interpretador.

A seleção verifica o hash da imagem VU1 quando muda a geração, ponteiro ou tamanho. Cada par também confere as duas palavras antes de alterar o estado. Os contadores `[VU AOT] compiled=... fallback=... image=...` confirmam o uso; não são FPS nem percentual de recompilação concluída.

Para compilar com capturas locais:

```sh
ps2recomp/build_vu_experimental.sh imagem-vu1.bin coverage.bin segunda-imagem-vu1.bin
```

A segunda imagem é opcional. Capturas e header contendo instruções do jogo ficam locais e ignorados. Não fazem parte do patch distribuído. Sem o header gerado, o runtime compila apenas com interpretador. O gerador do bundle exige imagens VU1 completas de 16 KiB.

Para testar com o backend GPU já instalado, por tempo limitado:

```sh
ps2recomp/run_vu_experimental.sh 120 /tmp/black_vu_experimental.log
```

O teste termina automaticamente. Sem `PS2X_VU_AOT` o runner usa o interpretador. O experimental não é ativado por padrão e não implica correção gráfica ou ganho de desempenho comprovado.

`compare_update_logs.py experimental.log baseline.log` compara os mesmos contadores de Game::update em duas execuções limitadas. Uma comparação isolada não substitui repetições; updates/s não são o FPS da janela.

Primeiro A/B limitado com duas imagens (versão de 403 pares, anterior à ampliação LQI/SQI/LQD/SQD): 180 updates comuns (1201..1381) levaram 36,1 s no AOT e 38,1 s no interpretador, aproximadamente 4,99 contra 4,72 updates/s. Diferença de 5,5% em uma única passagem, ainda sem confirmação por repetições. As execuções chegaram ao Level_00 e foram encerradas automaticamente; o teste não confirma correção dos gráficos.

A versão ampliada inclui LQI/SQI/LQD/SQD e emite 466 dos 667 pares observados em cada uma das duas variantes (590 corpos lower e 509 upper). Foi compilada e executada por 90 s até a gameplay do Level_00, com fallback ativo e encerramento automático. Não houve novo A/B desta ampliação; o resultado de 5,5% acima pertence à versão anterior.

A expansão ACC usa ADDA/SUBA/MULA/MADDA/MSUBA e OPMULA com os helpers e a fila reais. A versão atual emite 554 pares por variante (623 upper e 590 lower), foi compilada e testada por 90 s na gameplay. O A/B inicial de 5,5% continua pertencendo à versão antiga de 403 pares; não há medição comparativa nova desta versão.

Com conversões, ABS e CLIP, todos os 667 PCs upper observados nas duas capturas são emitidos. A versão atual tem 590 pares completos por variante e foi compilada/testada na gameplay por 90 s. Restam 77 PCs lower nessas capturas, além da VU0 e de outras imagens. O número não representa percentual global de conclusão da VU.

Com MTIR/MFIR e flags lower, a versão atual suporta 642 pares por variante, incluindo filas FCSET/FSSET reais. Foi compilada e testada por 90 s na gameplay do Level_00. Restam 25 PCs lower nas duas capturas, além da VU0 e de outras imagens. Ainda não há A/B novo desta ampliação.

Com ILWR/ISWR, o experimental atual suporta 643 dos 667 pares observados por variante. Passou nos testes de memória e filas e numa execução de 90 s até a gameplay do Level_00, encerrada automaticamente. Restam 24 PCs lower nessas capturas; Q/EFU, VU0 e outras imagens continuam pendentes. Não houve novo A/B de desempenho.

Com DIV/SQRT/RSQRT/WAITQ, o experimental atual suporta 655 dos 667 pares observados por variante. As filas Q e esperas FastVU/accurate foram comparadas com produção, com limites IEEE e regressões de memória. Build e execução de90s até gameplay passaram e o runner encerrou automaticamente. Os12PCs restantes nessas capturas são XTOP/XITOP/XGKICK; VU0 e outras imagens permanecem pendentes. Não houve novo A/B de desempenho.

Com XTOP/XITOP, o experimental atual suporta665dos667pares observados por variante. Fixture512encodings e limitesTOP/ITOP passaram com scheduler real; build e run90s chegaram gameplay e encerraram automaticamente. Somente2PCsXGKICK restantes nessas capturas; não equivale aVU completa e não há ganho novo medido. XGKICK aguarda validação de transferência PATH1, bytes/ciclos e ordem LSU.

XGKICK agora emite chamada ao startXgkick real. O experimental cobre667dos667pares observados em ambas capturas; outrosmicroprogramas/VU0 continuamfallback. Testes dedicados compararam bytes e ciclosPATH1, formatosGIF/tagsEOP/wrap/ordemLSU, com consumidor capturado; build e run90s chegaram gameplay com consumidor original e encerraram automaticamente. Não valida rasterização ou ganhoFPS, nem equivale àVUinteira recompilada.

Benchmark atual667pares,4runsABBA: interpretador média5,12updates/s eAOT4,91updates/s, diferença−4,1% nasjanelasrelativas. Semganhoobservado; amostra pequena e recortescomcontadoresiguaisvariaram, portanto não éregressãofixaestatisticamenteconfirmada. Veja VU_PERFORMANCE_2026-10-05.md. O antigo+5,5% eraumaúnicapassagem deversão403pares e não descreveoversãoatual.

Pares agora usam despachoPC único paraupper+lower, com mesmasverificações/sombra/queues. 170880comparaçõespassaram e4runsABBA terminaram. MédiaAOT5,14/I4,73updates/s, mas variaçãoI/janelas impede afirmar ganho8,8%; primeiropar alinhado empatou5,04/I5,03. VejaVU_FUSED_DISPATCH_2026-10-05.md. Opt-incontinua; helpers/scheduler ainda existem.

O helper deordem/sombra agorateminlineforçado (--inline-mode helper), comcorposdecididoscompilador. Runnerhelper1310→0símbolos, build/test90s atégameplay passaram. Benchmarkdeterminísticoisolado,6repetições porversão/path:FastVU+4,8%,accurate+3,4%,estado+memóriachecksumsiguais. Não énovoA/Bgameplay nemganhoFPS. VejaVU_HELPER_INLINE_2026-10-05.md. Forçartodoscorpos foi rejeitado; opt-inmantido.

PrimeiraVU0incluída:77d305d95d59af9a,162pares observados(4KiB). PS2X_VU_AOT ativaVU0/VU1paraimagensembutidas;VU0outrasimagens/macroainda interpretadas. AmbasVU1mantêm667pares cada. Buildcom3imagens:

```sh
ps2recomp/build_vu_experimental.sh recomp/diagnostics/vu-coverage-level00/vu1-c2835de1d8dfddf0.bin recomp/diagnostics/vu-coverage-level00/coverage.bin recomp/diagnostics/vu-coverage-level00/vu1-7271631fc285cb81.bin recomp/diagnostics/vu-coverage-level00/vu0-77d305d95d59af9a.bin
```

90368comparações passaram;identityselecionaunit/hash eVU0usa4KiB/PCmask0xfff. Build/run90s atégameplaypassaram com[VU0 AOT] ativo eautofechamento. ConversãoindependentedemediçãoFPS; nãohouveA/Bnovonestaetapa.

As trêsimagensVU0capturadas agora estãoincluídas:162+95+36=293paresobservadosdistribuídospelastrêsvariantes. Bundle5com duasVU1anteriores. Build:

```sh
ps2recomp/build_vu_experimental.sh \
  recomp/diagnostics/vu-coverage-level00/vu1-c2835de1d8dfddf0.bin \
  recomp/diagnostics/vu-coverage-level00/coverage.bin \
  recomp/diagnostics/vu-coverage-level00/vu1-7271631fc285cb81.bin \
  recomp/diagnostics/vu-coverage-level00/vu0-77d305d95d59af9a.bin \
  recomp/diagnostics/vu-coverage-level00/vu0-757121c0db138fe6.bin \
  recomp/diagnostics/vu-coverage-level00/vu0-686f3730c337d24c.bin
```

88384comparaçõesnovaspassaram,build e90satégameplay passaram. LogsVU0 incluíramostrêshashes eúltimocontador34603008compiled/fallback0;fechouautomaticamente. Zero fallbacknessaamostranão significaVU0ISA/macro/outrosníveis concluídos. NãohouvebenchmarknovodeFPS; focoémais código convertido.

### Blocos VU0: contrato inicial de budget e retomada

O emissor cria candidatos de blocos sequenciais limitados a oito pares e interrompidos em instruções de controle. O runtime despacha imagens VU0 e VU1 correspondentes por `executeBlock`; cada par ainda passa pelo scheduler existente, que cobra ciclos, enfileira resultados, aplica o delay slot e atualiza o PC.

`python3 ps2recomp/tests/vu_block_aot_test.py` compila uma imagem sintética VU0 e retoma por fatias de 1 a 8 pares. IBEQ taken/untaken respeita PC 8 como delay slot. No caminho untaken, uma SQ enfileirada em PC 24 é publicada antes do payload PATH1 de XGKICK em PC 32; o evento ocorre uma vez apesar dos cortes de budget. Uma regressão separada com decoder e filas reais passou 40.000 pares VU0 nos modos rápido e preciso. `vu_pair_aot_test.py --xgkick` passou 8192 transferências com parser PATH1 de produção, tags encadeadas/EOP, wrap circular, formatos GIF e ordem LSU; o consumidor foi capturado, sem validar rasterização.

Smoke gameplay com `PS2X_PAD_SCRIPT` entrou na missão (`cur=0x004bcf78`) e terminou automaticamente aos 150 s. As três imagens VU0 capturadas apareceram nos logs. A última amostra registrou 90.177.536 pares compilados e zero fallback VU0; esse contador conta pares observados pelo caminho AOT, não instruções únicas nem percentual global. Não houve guest fault, opcode reservado, exhaustion ou assertion nos logs. Isso confirma execução do caminho integrado no Level_00, não correção visual completa. Sem A/B nesta etapa e sem ganho de FPS afirmado.

### Expansão para as imagens VU1 capturadas no Level 00

O catálogo local contém 33 hashes VU1 e 3 hashes VU0. O bundle antes incluía só dois hashes VU1. O gerador agora aceita `--vu1 arquivo.bin` repetido, mantendo os argumentos posicionais antigos e os caminhos `--vu0`:

```sh
ps2recomp/build_vu_experimental.sh vu1-principal.bin coverage.bin vu1-secundaria.bin vu0-a.bin vu0-b.bin vu0-c.bin --vu1 vu1-extra-a.bin --vu1 vu1-extra-b.bin
```

Incluí os 31 hashes VU1 restantes no bundle local. Entre as 33 variantes, há 11.387 pares observados; 11.359 têm corpos upper+lower completos e 28 PCs lower permanecem em fallback. Isso soma 10.025 pares recompilados a mais que as duas variantes anteriores, contando cada par-PC por imagem, não instruções únicas. O header gerado ficou com 20 MiB; o runner compilou com 86 MiB.

No smoke temporizado de 90 s em Level_00, o runtime registrou atividade em 17 hashes VU1 distintos e nos três hashes VU0, com o backend paralelo ativo. Não apareceu guest fault, instrução reservada, exhaustion ou assertion. O teste confirma seleção das imagens e execução do caminho AOT durante a cena; não valida a correção visual nem mede ganho de FPS. Log: `/tmp/black_vu-all33-test.log`.

MFP, identificado nos 28 fallbacks, passou a ser emitido como cópia de P para VF com máscara via `applyDest`; o scheduler/latência original permanece no controle. Fixtures e três capturas passaram, cada captura com 40.000 pares sequenciados FastVU/accurate. O smoke de 30 s carregou Level_00 e o scanout paralelo, mas não imprimiu contadores VU; portanto não confirma a execução de MFP na gameplay. Log: `/tmp/black_vu-mfp-test.log`.

### ESQRT, ESIN e ERCPR nas capturas VU1

As 16 posições lower restantes eram três EFUs. O emissor agora gera ESQRT, ESIN e ERCPR para os sites capturados. ESQRT/ERCPR usam o caminho real `queueP`, preservando latência e espera da fila P; ESIN delega à normalização polinomial do interpretador antes de enfileirar P com sua latência de 29 ciclos. O scheduler e decoder de produção continuam decidindo hazards e commits.

As fixtures compararam 64 estados por corpo com as implementações do interpretador. Uma sequência de três pares EFU passou 40.000 pares nos modos FastVU e accurate, comparando pipeline P, latências, stalls, filas e estado. O bundle de 33 imagens VU1 + 3 VU0 compilou e linkou com Ninja/LTO; as 11.387 posições VU1 observadas têm corpo upper+lower emitido, além de 293 posições VU0 observadas já cobertas. Isso é cobertura estática por imagem/captura, não percentual da ISA, código dinâmico executado ou conclusão da recompilação.

Smoke de gameplay de 90 s está registrado em `/tmp/black_vu-efu-test.log`: houve contadores AOT em 9 hashes VU1 e 2 VU0, todos com fallback zero nos contadores amostrados. O log não associa o contador ao opcode/PC individual, então não confirma que os sites ESQRT/ESIN/ERCPR foram executados na gameplay. Não houve guest-fault, reserved detail, exhaustion ou assertion. O processo fechou automaticamente. Isso não comprova correção visual ou jogabilidade completa; não foi medido FPS.

### Blocos sequenciais também no VU1

Agora o emissor cria entradas de bloco para ambas as unidades: até oito pares consecutivos, terminando em branch ou instrução com controle/halt. O dispatcher de hash/unidade e o loop runtime VU1 usam `executeBlock`. Cada issue ainda passa pelo mesmo `processPair` e scheduler; guardas de PC/opcodes, latência, hazards, budget, delay slot e retomada permanecem ativos. Assim, o bloco junta o despacho estático dos corpos; ainda não é um bloco nativo que elimina o scheduler por par e não se afirma ganho de desempenho.

`vu_block_aot_test.py` passou para fixtures geradas como VU0 e VU1, com cortes de budget de 1 a 8, branch taken/untaken, delay slot, SQ antes do XGKICK e retomada. `vu_pair_aot_test.py --scheduler --real-helpers` passou 40.000 pares contra decoder/scheduler de produção. Bundle 33 VU1 +3 VU0 regenerado; 11.387 VU1 e293 VU0 pares observados têm block-entry candidate. Build Ninja/LTO passou. Smoke de gameplay de60s (`/tmp/black_vu1-block-test.log`) registrou atividade em14 hashes VU1 e3 VU0, todos com fallback zero nos contadores amostrados; sem guest-fault/reserved detail/exhaustion/assertion. Confirma execução integrada em Level_00, sem comprovar render correto nem desempenho.

Experimentei trocar o callback de corpo por chamada template estática para cada `PairBody`. As fixtures e os 40 mil pares passaram, mas o build completo entrou em ThinLTO por mais de5min e usou cerca de2,5GiB sem concluir; interrompi e removi essa versão. Bundle e runner foram reconstruídos com o dispatcher de callback compacto; portanto, a tentativa não ficou no runtime. Smoke final auto-fechado em `/tmp/black_vu1-static-call-test.log`: atividade AOT em13 hashes VU1 e3 VU0, fallback zero nos contadores amostrados, sem guest-fault/reserved detail/exhaustion/assertion. Nenhum FPS é inferido.
