# Black (SLUS_213.76): estado do projeto

Ler no início da sessão e atualizar a cada marco.

## Decomp byte-matching (raiz do repo)

- ROM: `build/SLUS_213.76.rom: OK` (sha1 idêntico) é a condição de aceitação de toda mudança.
- Progresso (2026-10-01): TOTAL 6.3563% (jogo 4.5870%, 2521/9580 funções; middleware 22.6808%, 815/1236; sdk 0%).
- Todos os 64 segmentos de `cod/` são `c` no yaml. Funções ainda em asm via INCLUDE_ASM.
- Pipeline automático em `tools/auto/` (saídas em `build/auto/`):
  leafgen → symgen → m2cgen → applyloop → callfix → ninja → romfix.
  Quase-acertos: `nearmiss.py` → `perm_batch.py` (decomp-permuter) → `*.ok.c`.

## Recomp (PS2Recomp, `ps2recomp/`)

### Otimização, primeira cena 3D e bloqueio de IRQ — 2026-10-02

- Run manual `recomp/window-fixed-boot.log` entrou na missão em g=374.7. Até g=382.7, 60 updates gastavam 37–49 segundos (~1.2–1.6 updates/s). Perfil `recomp/diagnostics/gameplay-sample-oct02.txt` mostrou rasterização/texturas como principal custo. Depois parou em polling DMA/VIF na função 0x2B2638; amostra `gameplay-stall-oct02.txt`. Houve mensagens VU1 com sentinela 0xFFFFFFF8 e scratch exhaustion de 24 bytes. A causa do esgotamento ainda não foi demonstrada.
- A sentinela 0xFFFFFFF8 vem de `progressXgkick` recusando GIF FLG=3, não do decoder de instruções VU. PS2tek e PCSX2 tratam FLG=3 como IMAGE2. Corrigidos parser XGKICK, frontend GS, reconhecimento de IMAGE no arbiter e fast paths de upload DMA. Teste `gif_image2_test.py` passou. Isso não prova sozinho que todo pacote emitido pelo jogo está correto.
- Triângulos grandes agora podem dividir linhas entre workers do pool existente. Guardas conservadoras mantêm serial VRAM wrap, textura sobre destinos e alias framebuffer/Z. CT32/24/16/16S e Z32/24/16/16S podem paralelizar: os stores CT16/Z16 são de 16 bits independentes; as páginas de 16 bits têm altura 64. Sem alteração na matemática por pixel. Workers herdam/restauram o modo de arredondamento do caller (o VU usa FE_TOWARDZERO). `gs_triangle_test.py` passou nos quatro modos de arredondamento e em 13 formatos de textura, com snapshots de VRAM idênticos: serial 275 ms, 1 worker 137 ms, 4 workers 60 ms (~4.6x neste benchmark, não FPS da missão). Também comparado com backend pré-alteração.
- Builds `recomp/gameplay-performance-build.log`, `parallel-rounding-build.log` e `parallel-16bit-build.log` passaram. Regressões de dispatch e window init passaram. Patch persistido. Run intermediária `recomp/performance-image2-mission.log` foi encerrada em g=356.7 para usar o binário final com CT16 e propagação do arredondamento. Run final `recomp/performance-final-mission.log` em andamento, com detalhes GS no update 10741, trace DMA depois de g=380, input após g=390 e analógicos após g=410. Gameplay e ganho real ainda sem confirmação. Não existe percentual confiável restante para um recomp jogável.


Decomp está pausada. O runner atual sobe o menu e chega ao briefing de Veblensk (Level_00). Gameplay controlável ainda não foi confirmado. Não existe uma métrica validada de percentual restante para um recomp jogável; 6.3563% mede somente a decomp byte-matching.

- Run final `recomp/performance-final-mission.log` chegou à cena 3D com HUD e objetivo em g≈370, registrada em `recomp/diagnostics/optimized-first-world.png`. A imagem tem corrupção severa de texturas/cores. O usuário confirmou melhora grande antes da gameplay, mas gameplay muito lenta e renderização incorreta. Não confundir isso com um recomp jogável.
- Briefing/entrada na missão continuam em ~1.2–1.4 updates/s (60 updates / 45–50 segundos). `optimized-mission-sample.txt` mostra VU e triângulos pequenos consumindo tempo; benefício sintético dos triângulos grandes não se traduziu em ganho suficiente neste trecho.
- Run deixou de produzir updates após g=372.7, embora relógio/pad continuassem até g=416+; depois houve `runtime-scratch` exhaustion. Sem mensagens VU reservadas nesta run, mas o bloqueio permaneceu. Teste terminou por limite de 1800 segundos.
- Captura LLDB real do bloqueio: PC=0x2B28E8, ISR na thread EE 2, espera `(GIF_STAT & 0x1F000000) != 0`. GIF_STAT armazenado=0, MSKPATH3 ativo, fila PATH3 retida vazia. GIF TADR=0x1D6DE90 após um REFE de QWC=0 começando em 0x1D6DE80 (descritor em 0x1D24400). Isso NÃO prova que basta preservar FQC de filas mascaradas: nessa captura não havia payload retido. A origem da transferência vazia e a arbitragem temporal continuam em investigação; não forçar FQC ou pular o polling.
- Captura do scheduler: 242628 invocations pendentes, duas threads com ISR ativa; arena com 8144 blocos usados de 32 bytes e um de 1536. Arquivos `live-context.bin`, `live-rdram.bin`, `live-mmio.json`, `live-vram.bin`, `live-vu1-*`, `live-*-capture.log` e layouts locais usados pelo LLDB permitem continuar sem perder a evidência. Dumps são gitignored, não distribuir os assets do jogo.
- Corrigida acumulação de IRQs pendentes: INTC/DMAC são causas latched; `dispatchIrq` agora atribui identidade (controlador/cause/handler) e `queueInvocation` mantém uma entrega pendente por identidade. Handler ativo ainda pode ter uma reentrega pendente; pacotes SIF não são descartados. `irq_pending_test.py`, dispatch e IMAGE2 passaram. Build `irq-pending-build.log` passou; run `irq-coalesced-mission.log` chegou a g=242.5 sem fault; encerrada para testar também o despacho de ISR. Não resolve por si só a fila GIF vazia nem a corrupção visual.

### Retomada de 2026-10-01

- Evidência persistente: `recomp/resume-trace.log` e imagem `recomp/diagnostics/veblensk-briefing.png`. O teste passou pelo Start, terminou o FMV em g=344.81, carregou Level_00 (loader 20→28 em g=352.02) e continuou até g=366.7 / update 10981. Encerrado manualmente; não foi um crash. Nenhum erro de opcode reservado VU1 ou arena scratch apareceu nesse intervalo. Isso não demonstra que as falhas históricas foram corrigidas.
- Script usado: `9:up:0.3,9.5:cross:0.3,24:start:0.3,40:cross:0.5,45:start:0.5,198:cross:0.3,203:cross:0.3`. A tela mostra `VEBLENSK CITY STREET` / `07:36HRS`; ainda não há personagem controlável nem cena 3D validada.
- Depois do carregamento, 60 updates levaram aproximadamente 53 segundos de host (~1.1 updates/s). Amostras nativas em `recomp/diagnostics/briefing-sample*.txt` mostram o EE executando e muito tempo em rasterização CPU chamada por VIF/VU1/XGKICK. Lentidão observada, não prova de loop preso. Backend threaded existente (`PS2X_GS_THREAD=1`) ainda não foi medido nesta retomada.
- Instrumentação persistente em `ps2recomp/overrides/black_debug.cpp`: hooks FE:SetPage/PageCallback, `BLACK_FE_TRACE=1` para página/flags reais, registradores em chamadas e retornos, e `BLACK_TRACE_AFTER=<segundos guest>` para adiar `BLACK_TRACE_RANGE`. Compilação `--skip-recomp` concluída; logs `recomp/resume-build-diagnostics.log`. Não foram editados arquivos gerados nem aplicada correção especulativa de runtime.
- Smoke test do novo binário: `recomp/resume-diagnostics-smoke.log`, 40 segundos de host, passou pelo Start e chegou a g=34.3 sem crash. Novos registros FE e de retorno foram emitidos. O orçamento de 80 entradas iniciado em g=23 se esgotou antes do Start; usar início em g=24 ou aumentar o orçamento no próximo teste específico.
- Próximo teste de missão: usar os novos diagnósticos com o script acima e botões após g≈367 para verificar a saída do briefing; comparar desempenho com o GS threaded antes de mudar o padrão. Se o VU1 falhar, coletar kick/prev/VI/vizinhos já instrumentados. Se o Start falhar, rastrear `384e70:384ed8:80` com `BLACK_TRACE_AFTER=24` e conferir preservação de s0/ra/stack.

### Segunda rodada: entrada analógica e teste GS threaded

- `recomp/threaded-mission.log`: `PS2X_GS_THREAD=1` passou pelo Start, mas parou antes da missão, após g=192.5. Leituras inválidas em `0x218DA8`, `0x218DB0`, `0x218DC4` usam `s2=1` (endereço resultante `0xFFFFDF69`); depois o scheduler recebe PC/RA=1 e torna a thread principal dormant. O relógio/pad continuam avançando, portanto log de botões isolado não comprova execução do jogo. A causa da corrupção ainda não foi provada; não atribuir automaticamente ao GS threaded.
- Intro com GS threaded: aproximadamente 60 updates / 3.1 segundos (~19 updates/s), contra ~29 no smoke serial. Não chegou ao briefing para comparar esse trecho. O modo padrão continua serial. Amostra nativa persistida em `recomp/diagnostics/threaded-sample.txt`.
- Corrigida uma limitação independente de entrada: o teclado antes deixava os dois sticks centralizados. W/A/S/D agora também geram o stick esquerdo; I/J/K/L geram o direito. Gamepad conectado mantém prioridade sobre teclado. Script ganhou nomes `ls_*` e `rs_*`, com cancelamento de opostos e restauração da entrada live ao soltar. Alteração persistida no patch do runtime.
- Build completo do runner passou (`recomp/analog-build.log`). Teste independente do backend passou: `ps2recomp/tests/pad_input_test.cpp`, instruções em `ps2recomp/tests/README.md`. Gameplay continua sem confirmação; percentual restante para jogável não mensurável.
- A primeira execução após build caiu em `rlLoadTexture → rlglInit → InitWindow`, antes de iniciar o guest (`recomp/analog-smoke.log`, código 139). Retry abriu normalmente (`recomp/analog-smoke-retry.log`), passou pelo Start, emitiu sticks=128,1/255,128 em g=30 e voltou ao centro em g=31; chegou a g=36.3 sem guest-fault/missing-target e foi encerrado pelo limite de 40 segundos de host. Não foi aplicada correção especulativa da falha nativa.
- Próxima investigação da corrupção: registrar `s2`, SP e os slots salvos ao chamar/retornar de `0x241048` dentro de `0x218CB0`, perto de g=192. O assembly de `0x241048` salva/restaura s2 no stack; é preciso distinguir escrita no stack de retomada incorreta antes de corrigir o runtime.

### Travamento reportado no teste manual

- Launcher `ps2recomp/JogarBlack.command`, GS serial, sem script de pad. Log preservado em `recomp/diagnostics/manual-freeze.log` (original `recomp/manual-test.log`). Último update: g=288.6, update 8641, cena frontend; após carregar MC2_D.IRX ocorreu `guest-branch:missing-target`, PC=RA=`0x01FFFC40`, SP=`0x01FFFC10`. O alvo fica na pilha, fora da região de código. Processo encerrado depois de salvar o diagnóstico.
- Últimas entradas: `0x286990 → 0x285CC8 → 0x1D7100 → 0x367C30 → 0x2526AC`. `0x367C30` é o wrapper do syscall -0x77 (`sceSifSetDma`); `0x2526AC` é continuação de `0x252680`, cuja epíloga recupera RA do stack. A proximidade do DMA não prova que ele causou a corrupção. Falha também ocorre no GS serial; GS threaded não é condição necessária.
- Correção ainda não identificada. Próxima coleta deve observar o RA salvo e SP em `0x252680`/`0x252328`, incluindo retornos e retomadas, antes de aplicar qualquer mudança. Gameplay permanece sem confirmação.

### Correção da propagação de pausas no dispatcher

- Causa demonstrada: `dispatchGuestBranch` interpretava PC igual ao endereço de retorno (ou ao entry-PC do callee) como conclusão da chamada, mesmo quando uma chamada recursiva mais profunda havia pausado e deixado sua pilha aberta. O chamador continuava com SP/registradores do descendente, podendo recuperar RA de dados em vez do slot correto.
- Correção em `ps2_runtime.cpp`: contador thread-local de transferências, capturado antes de chamar código guest; qualquer pausa de checkpoint ou transferência que exige retorno ao scheduler se propaga por todos os frames nativos. A comparação de PC só decide retorno quando nenhum descendente transferiu a execução. Sem alterar código gerado nem ignorar chamadas do jogo. Persistido em `ps2recomp/patches/0001-black-runtime-fixes.patch`.
- Teste `ps2recomp/tests/dispatch_yield_test.py` compila os corpos reais dos dois métodos com scheduler determinístico. Antes: falhavam pausa recursiva no retorno do ancestral e pausa no entry-PC. Depois: PASS, incluindo retorno normal, preservação de PC/SP e stub HLE. Logs `recomp/dispatch-test-before.log` e `recomp/dispatch-test-after.log`. Build passou (`recomp/return-fix-build.log`).
- Confirmação no jogo: `recomp/return-fix-validation.log` registra colisões evitadas em `0x240ED8 → 0x2407F8` e `0x2410F0 → 0x23E3D0`, com vários níveis de SP ainda abertos. A segunda cadeia também aparece nos travamentos anteriores. Eventos diagnosticados por `PS2X_TRACE_CALL_STACK=1`.
- Integração: passou pelo Start e MainMenu, FMV terminou em g=346.15, Level_00 chegou ao estado 28 em g=353.35; execução continuou até g=356.7 / update 10681 sem guest-fault, missing-target, erro reservado VU1 ou scratch exhaustion. Imagem `recomp/diagnostics/return-fix-level-intro.png` mostra a apresentação `4 DAYS EARLIER`. Teste encerrado manualmente para reabrir sem script; não é confirmação de gameplay controlável nem garantia de ausência de outros defeitos.
- Renderização continua muito lenta após load: 60 updates em ~46.5 segundos (~1.3 updates/s). Percentual restante para jogável continua sem métrica validada; decomp permanece 6.3563%.
- Padrão aprendido: pausa recursiva com PC igual ao entry/retorno de um ancestral causa falso retorno e leitura dos slots da pilha errados; corrigir propagando explicitamente a transferência até o scheduler, antes de interpretar igualdade de PC como conclusão.
- Reabertura após a validação falhou três vezes antes do guest, na criação da janela (código 139, aviso `GLFW: Failed to determine Monitor to center Window`), logs `recomp/manual-fixed-test.log`, `recomp/manual-fixed-retry.log`, `recomp/manual-fixed-ready.log`. O launcher atualizado foi aberto via LaunchServices/Terminal, mas não havia processo do runner ativo na última verificação. Não afirmar que o jogo ficou aberto. É a falha nativa de inicialização gráfica já vista antes, distinta da corrupção de retorno corrigida.

### 2026-10-02: falha de inicialização gráfica

- Causa encontrada na dependência Raylib 5.5: `InitWindow` ignorava o retorno de `InitPlatform`. Quando GLFW não conseguia determinar um monitor, encerrava a plataforma e retornava -1; mesmo assim `InitWindow` chamava `rlglInit`, provocando acesso a funções OpenGL sem contexto. Os relatórios anteriores apontavam `rlLoadTexture → rlglInit → InitWindow`.
- Corrigido o fluxo da Raylib para retornar antes de inicializar GPU se a plataforma falhar. Patch persistente `ps2recomp/dependencies/raylib-window-init.diff`, aplicado de modo idempotente/não interativo por `build.sh` após configurar as dependências. Runtime também verifica `IsWindowReady()` e retorna falha antes de áudio/debug UI. Parte do runtime regenerada no patch 0001.
- Teste `ps2recomp/tests/window_init_test.py`: trecho real de inicialização com plataforma simulada; falha não chama GPU nem marca GPU pronta, sucesso mantém inicialização normal. PASS. Reverse dry-run confirmou patch aplicado, sintaxe de build.sh passou. Build completo de verificação concluiu com sucesso (`recomp/window-fix-build-retry.log`). Primeiro build compilou/linkou, mas editar o script enquanto ele rodava causou erro ao ler sua cauda; retry estável passou.
- A indisponibilidade do monitor é externa à emulação: esta correção impede a queda nativa e informa erro; não inventa um monitor ou contexto. Em 2026-10-02 a inicialização normal funcionou antes e depois da alteração.
- Runner corrigido aberto sem script ou limite de tempo, PID 45903 na última verificação, log `recomp/window-fixed-boot.log`, execução até g=74.4 / update 2221 sem guest-fault/missing-target no intervalo. Launcher continua `ps2recomp/JogarBlack.command`. Gameplay ainda não confirmado, lentidão da rasterização CPU permanece; percentual para jogável continua sem métrica validada.
- Padrão aprendido: ignorar falha de InitPlatform causa inicialização OpenGL sem contexto e SIGSEGV; retornar antes de rlglInit e verificar janela pronta no consumidor.

### Histórico anterior (logs temporários não disponíveis na retomada)

- Última run boa: `/tmp/b27.log`. Script `PS2X_PAD_SCRIPT=9:up:0.3,9.5:cross:0.3,24:start:0.3,198:cross:0.3,203:cross:0.3`. Intro/FMV termina em g≈346 (`FE:fmvDoneCb`), o loader sai do estado 1 e abre `Levels\Level_00\...`.
- Depois do load (loaderState 28, mode volta a 0) o VU1 para em micro-PC `0x2830` com lower `0xfffffff8` (opcode reservado). Em seguida `[runtime-scratch] arena exhausted`. Diagnóstico extra (kick/prev/VI/vizinhos) está em `ps2_vu1_core.cpp` e ainda não foi exercido até o nível: a run `/tmp/b29.log` morreu antes.
- Freeze de `/tmp/b29.log` (guest encerrou, janela parada): no Start (g≈24) o walker `0x241128` chama o método `0x384e70`. Os dois primeiros slots do descritor rodaram (último retorno em `0x384ea8`, no meio passou `sceSifSetDma`). No terceiro slot (`jalr` em `0x384ebc`) `$s0` já era `0x2411f0`, que é o endereço de volta do `jalr` em `0x2411e8` (`move $s0,$s2`), não um objeto. `lw` de `+4` leu o opcode `0x5600ffe2` e o ponteiro de função saiu 0. A run `/tmp/b27.log` passou pelo mesmo Start.

## Padrões aprendidos ("X causa Y, corrigir com Z")

- Ps2EeAs insere nops em loops curtos e entre c.cond/bc1f → o ROM desloca; emitir branches como `.word` (ee_cc.py).
- Padding DIV / `vXXX ACC` / `psravw` mal codificados pelo Ps2EeAs → emitir como `.word`.
- Retail com padding extra após certas funções (func_00138328, func_0027C0B8) → C desloca a TU; manter em asm (try_match mostra `DESL`).
- Segmentos com `break 7` → compilar sem `-mno-check-zero-division` (CHECKED_DIV em configure.py).
- m2c não conhece `max.s`/`min.s` do EE → patch `tools/patches/m2c-ee-minmax.patch`.
- Permuter com stdout em pipe nunca retornava após kill (workers herdam o pipe) → log em arquivo + `start_new_session` + `killpg`.
- m2c emite `p += 4` num `s32 *` (bytes) → binário anda 16; `ptrfix` em m2cgen.py escala por sizeof.
- m2c emite `*(X + &G)` com X em bytes e G tipado → `addrfix` reescreve para `*(T *)((X) + (s8 *)&G)`.
- O permuter às vezes troca o tipo de um extern (ex.: `char` em vez de `s32`, `void` em vez de `s32`) → `permute.py` copia o extern dele com alias novo (`D_X_P<tipo>`, `_kP<tipo>_func_X`).
- O score 0 do permuter pode ser falso (ele normaliza alvos de branch) → `applyloop`/try_match decide.
- 414 "funções" `addiu $29,$29,+N; nop` são fragmentos mortos entre funções; perm_batch ignora. o padrão de chamada virtual, mas quebra outras funções → não usar globalmente.

### Despacho durante IRQ — 2026-10-02

- Captura anterior mostrou dois handlers Interrupt ativos em threads distintas. `transferIfRequested` evita troca dentro do ISR, mas `checkpointDue`, `applyPendingPreemption` e a cauda de `run` podiam trocá-lo mesmo assim. Corrigidos os três pontos para adiar troca de thread enquanto a invocação Interrupt estiver ativa. Eventos/timers continuam sendo processados; a solicitação de troca é preservada até o retorno do handler.
- `irq_preemption_test.py` extrai checkpoint e preempção de produção: PASS para ISR, retorno, deadlines, slice comum e callback SIF. Build `irq-preemption-build.log` passou; run `irq-serialized-mission.log` iniciada. Efeito sobre gameplay e renderização ainda não validado.

- Encontrado outro parser IMAGE2 incompleto: `pendingGifImageQwc` no VIF1 reconhecia somente FLG=2, portanto uma imagem FLG=3 partida entre comandos DIRECT perdia a continuação e seu payload podia ser interpretado como GIFtag. Corrigido reconhecimento de FLG=3; teste `vif_image2_test.py` passou, baseline reproduziu a falha. Build `vif-image2-build.log` passou. Ainda sem prova de que esta seja a origem das paletas corrompidas de Black; a run ativa testa o binário anterior, com as duas correções de IRQ.

- Run `irq-serialized-mission.log` passou do bloqueio anterior g=372.7 e chegou a g=380.7 sem erro de opcode VU nem esgotamento de scratch no intervalo. Gameplay ainda ~1.2 updates/s. Capturas `world-palettes/` compararam 16 CLUTs carregadas com a VRAM no instante do draw: todas idênticas, incluindo cores erradas. Investigar uploads e passes de conversão anteriores; capturas `world-uploads/` incluem dados de origem e comandos. Não atribuir ao lookup da CLUT sem nova evidência.
- Skip de vídeo em implementação por pedido do usuário: Tab/Select chama a rotina original `Video::stop` (0x109550) quando `Video::update` (0x109918) está no estado 28. A rotina solicita parada e deixa teardown/callback/transição para o fluxo original. Módulo `black_cutscene_skip.cpp`, opt-in `BLACK_CUTSCENE_SKIP=1`, habilitado no launcher. Build inicial falhou por assinatura do registrador/unity globals; ajustado, retry passou (`cutscene-skip-build-retry.log`). Run `cutscene-skip-validation.log` iniciada com Select em g=40/50/70/80/90/100 e sem Start depois de g=24. Ainda não validado no jogo; cenas dentro da engine não abrangidas por esse vídeo-player.

- Runner com IRQs chegou a g=384.7 sem o freeze anterior. A imagem `irq-serialized-world.png` mostra PAUSE MENU sobre a cena 3D, com RESTART MISSION selecionado; portanto o trecho não valida gameplay ativa nem resposta do personagem. Entradas analógicas scripted foram recebidas em g=369/371, mas não demonstram movimentação na fase. Corrupção visual permanece.

- Skip validado em `cutscene-skip-validation.log`: `[black-video] skip solicitado` seguido de `FE:fmvDoneCb` (g=33.90 no relógio de Game::update), abertura de Level_00 e estado de loader final em g=42.4 / host 58.3s. Usa binário com coalescência, serialização de IRQ e continuação VIF IMAGE2. Não chama diretamente callback de conclusão nem pula carregamento. Tab/Select habilitado no launcher; instruções `ps2recomp/CONTROLES.md`. Cenas dentro da engine ainda sem skip validado.

- Perfil com skip `skip-mission-sample.txt`: cálculo de dependências VI consome amostras relevantes do VU. `calculatePairReadyCycle` e `markPairWrites` agora visitam somente os bits VI ativos, preservando VI0 hardwired. `vu_dependency_test.py` compara 65.536 máscaras com varredura completa e valida arrays de VF/VI/ACC: PASS. Benchmark sparse 12.39 ms contra 27.25 ms no traversal; não mede ganho de FPS. Build `vu-dependency-build.log` em andamento.

- VU dependency build passou. Run com skip chegou ao Level_00 mas voltou a esgotar arena scratch (24 bytes); encerrada manualmente. Skip validado, gameplay continua bloqueada.
- DrawSprite ordenava X/Y da tela sem ordenar UV/ST correspondentes. Logs reais incluem sprites invertidos em passes sobre texturas. gs_sprite_test.py com backend real e gradiente conhecido reproduziu falha em X invertido; correção troca endpoints por eixo e passou oito casos FST/ST e X/Y. Build sprite-orientation-build.log em andamento; efeito nas paletas ainda não validado.

- Build sprite-orientation passou; regressão de 42 snapshots de triângulos em todos os modos de arredondamento passou. Run sprite-orientation-mission.log iniciada com skip, sem Start de pausa na fase.

- Integração sprite-orientation-mission.log: skip novamente seguido de FE:fmvDoneCb em g=31.3 e Level_00. Captura sprite-orientation-world.png em g=44.4 mostra cena 3D/HUD/objetivo ativos, sem PAUSE MENU. Cores ainda fortemente corrompidas; 60 updates levam ~46s (~1.3 updates/s), portanto orientação de sprites não resolve a causa principal nem há ganho global demonstrado para VU. Nenhum esgotamento até esse ponto; teste anterior o apresentou mais tarde. Recomp jogável ainda sem percentual confiável.

### Metal e cores — 2026-10-02

- Pedido: GPU e cores em conjunto. Backend Metal compute implementado como caminho do GS existente, opt-in PS2X_GS_METAL=1. Compartilha VRAM page-aligned sem cópia; ordena dispatches com barreira, sincroniza antes de CPU upload/CLUT/feedback/presentation. Formatos de framebuffer/depth de 32/24 bits e texturas CT32/24/16/16S/T8/T4/T8H/T4HL/T4HH; formatos/passes não suportados ou com feedback continuam na CPU. Não transforma o VU em GPU.
- gs_sprite_test.py passou com GPU real Apple M1 Pro e CPU. gs_metal_test.py passou 71 comparações de 4 MB completas (70 dispatches GPU, uma compatibilidade CPU), formatos, filtros, CLUT, wrap, alpha, blend, Z, fog, ST e feedback. Benchmark ordered em execução. Build metal-raster-build.log em andamento.
- COLCLAMP ignorado no blend CPU: sempre saturava. Teste produziu 255 onde COLCLAMP=0 exige 144 (400 AND 255). Corrigido RGB pós-blend para saturar ou mascarar conforme registro, incluindo negativos. GPU implementa regra. Run colclamp-mission.log iniciada com binário só desta correção; até g=48.4 apresenta intro 4 DAYS EARLIER, não valida cores da cena.

### Pesquisa e correção de sincronização GPU — 2026-10-02

- Perfil da primeira execução Metal encontrou 215/360 amostras de GameThread esperando GPU em LoadClut. CLUT agora carregada por compute, buffer interno 512 ushort, mesma fila/barreiras dos draws; CPU copia só após Sync necessário. Lotes 4096 dispatches. Build metal-gpu-clut-build.log passou; 71 comparações completas e ordered batch passaram (170 dispatches reais). Benchmark sintético não mede FPS.
- M1 Pro/MoltenVK 1.4.2 passou requisitos Vulkan básicos do paraLLEl-GS, incluindo BDA/subgroups/32 KiB shared. Ainda falta build e teste de shaders/GSInterface. Pesquisa e próximos gates documentados em GPU_BACKEND_PLAN.md.
- Run metal-gpu-clut-mission.log ativa; resultado de gameplay ainda pendente. Gravador encerrou 12000 draws ainda no frontend: gatilho por endereço não identifica fase, precisa transição explícita e CLUT inicial para replay fiel. Não usar esse capture como prova da fase.
- Cores e gameplay continuam não resolvidas; nenhum percentual confiável de recomp jogável.

- Integração Metal CLUT encerrada manualmente: g36.4/host57.5s no loader; novo perfil mostra 679/1456 amostras esperando GPU dentro de Submit por alternância com fallback CPU. Paletas deixaram de ser o único ponto de espera. Não habilitar caminho experimental por padrão. Regressão de triângulos/rounding passou.
- paraLLEl-GS standalone compilou no M1 Pro após patch Granite temporização macOS, preservado em dependencies/granite-macos-timer.diff. Smoke real GSInterface/MoltenVK passou upload/readback 4096 bytes e rasterização de sprite, pixel esperado 0x80402010. Arquivo tests/parallel_gs_smoke.cpp. Ainda não conectado ao Black; não há prova de cores/gameplay corretas. Logs parallel-gs-build.log / parallel-gs-smoke.log em diagnostics ignorado.

### Integração paraLLEl-GS — 2026-10-02

- Ponte GPU opcional implementada: PS2X_GS_PARALLEL=1, módulo dinâmico versionado em ps2recomp/gpu/parallel_gs_module.cpp. Runtime recebe GIF original antes de decode diagnóstico; árbitro preserva PATH1/2/3. Upload nativo/registros HLE, clear por sprite GPU e download FIFO roteados para GSInterface. Submit/LoadClut CPU não rasterizam nesse modo. Módulo é compilado separado via gpu/build.sh; fontes e binários dependentes em recomp/gpu ignorado.
- Apresentação nesta etapa ainda lê VRAM e converte pixels na CPU via helper; não usa scanout GPU direto. SIGNAL/FINISH/LABEL publicados pelo frontend legado ainda precisam de validação em streams fragmentados. VU interpretado permanece.
- Primeira integração caiu em GSParallelBackend::Flush durante GS::GS → reset antes de Initialize. Relatório confirmou causa. Corrigidos Flush/Sync/Reset pré-init; teste do frontend real cobre sequência e passou. Builds parallel-lifecycle/parallel-readback passaram.
- Segunda run iniciou GPU e desenhou >170 mil primitives, mas travou em compute_page_rect dentro de init_transfer (perfil 1403/1403 amostras do GameThread). Fórmula do upstream usa modulo 32 em limites de blocos e pode subtrair min maior que max, criando largura/altura enormes. Repro válida: CT32, BP não alinhado à página, SSAX48/SSAY24, RRW224/RRH240. Baseline interrompido por timeout; patch conservative min(raw block extent,31) termina. Patch dependencies/parallel-gs-page-rect.diff persistido e aplicado por gpu/build.sh.
- Teste adicional verificou 53.760 pixels após upload e cópia local multi-page, comparando download completo com padrão original: PASS. Também passam caminhos separados/intercalados, sprite, clear, native upload/FIFO e frontend real. Granite registra cada thread externa no slot 0: chamadas são serializadas por mutex do backend; evita erro de thread não registrada.
- Terceira run parallel-third-mission.log iniciada. Resultado de fase/cores/FPS ainda pendente. Launcher experimental gpu/TestarGPU.command, launcher padrão não alterado.
- Status jogável ainda sem percentual confiável; não declarar GPU scanout ou gameplay resolvidos por testes isolados.

- Terceira run chegou ao Level_00 ativo, mode0, g52.4/upd1561/host177.9s. Skip/loader completos; passou do travamento compute_page_rect anterior em g18.3. Rasterização GPU comprovada por contadores de milhões de primitives, sem fallback CPU. Últimos blocos de 60 updates: 31.2s,30.1s,22.6s,31.7s; ~1.9–2.7 updates/s. Baseline CPU separado ~46s/60; melhora observada, comparação não controla todos os fatores e não equivale FPS.
- Imagem parallel-third-world.png / parallel-third-world-latest.png ainda severamente corrompida com faixas e cores; backend GPU não resolveu cores. Perfil parallel-third-world-sample.txt: 1411/1907 amostras em VU1::run sob VIF e 1506/1907 no caminho de transferências/ISR; maior custo agora na interpretação VU. Scanout ainda é CPU/readback. Não declarar recomp jogável.

### VU sparse commit e limites GIF GPU — 2026-10-02

- commitReadyPipelines varria 16 VF + 8 VI + 8 ACC a cada ciclo. Máscaras de ocupação agora mantidas em queue/reset/commit permitem visitar somente os slots ativos, em ordem crescente idêntica à varredura. Queue allocation, latências e política de sequências WAW preservadas. Teste vu_sparse_commit_test.py compara 150 mil operações aleatórias contra implementação anterior, incluindo capacity failures, lanes, estado final e reset. PASS; microbenchmark sparse ~28ms contra62ms, não FPS. Build vu-sparse-commit-build.log passou. Regressão VI dependencies passou.
- Captura de transferência reservada gravou PATH2, BITBLT=3f4600003dd00000 (valores com aparência de ST), TRXREG=4400000000000001, SPSM61. Arquivo diagnostics/parallel-bad-transfer-expanded.bin. Diagnóstico não prova por si só que VU gerou dados ruins: decoder podia ler além do transporte.
- Achado bug em GSInterface::gif_transfer do paraLLEl-GS: optimized_draw_handler calculava nloops com size/nreg, ignorando offset i. Prefixos/múltiplas tags + payload dividido causam overread. Também podia ficar em laço sem progresso quando pacote disponível menor que nreg. Corrigido cálculo para (size-i)/nreg e só usa otimização quando há grupo completo; grupos parciais usam handler genérico. Patch dependencies/parallel-gs-gif-bounds.diff aplicado por gpu/build.sh.
- Repro: tag NOP + A+D de cinco loops dividido após três, seguido de guard qwords fora do tamanho enviado. Baseline desenhou 0x80aabbcc (guard) onde esperado0x80554433; patch desenhou esperado. Split/interleaved paths, upload/FIFO e cópia multipage continuam PASS. Teste módulo estendido.
- Auditoria inicial fragmentava pacotes em quads e travou o handler otimizado antigo (nloops0). Encerrada; auditoria agora observa o transporte sem mudar os blocos enviados. Run parallel-gif-bounds-mission.log usa VU sparse e limites GIF corrigidos; efeito no jogo ainda pendente.

- Run integrada parallel-gif-bounds-mission.log chegou a Level_00 ativo g110.5/host133.0 e g114.5/host183.6. Imagem parallel-gif-bounds-world.png mostra cena 3D/HUD reconhecíveis, mas paletas/texturas continuam fortemente corrompidas; layout ruidoso antigo melhorou, sem correção visual completa demonstrada. 60 updates:26.5s e24.1s, benchmark não controlado e poucos blocos; não atribuir ganho global certo apenas ao sparse commit.
- SPSM61/62 ainda apareceu após patch de bounds. Portanto overread é bug comprovado corrigido, mas não explica sozinho os comandos reservados nem toda corrupção. Auditoria preservada para seguir origem anterior do BITBLT com aparência de ST. Não afirmar que VU gerou payload ruim sem prova.
- Após g114.5 voltou runtime-scratch arena exhausted (24 bytes). Execução encerrada manualmente. Freeze/arena permanece bloqueador separado. Recomp não jogável; porcentagem sem métrica confiável.
- Revisão adicional encontrou limitação do clear GPU: ponte clear não transmite FBMSK/FBA completos do GSContext. CPU anterior respeitava esses campos. Corrigir e adicionar testes de máscaras/alpha antes de considerar fidelidade visual completa.

### Clear GPU e diagnóstico da fila — 2026-10-02

- Ponte clear atualizada para ABI 2: transmite FBMSK e FBA do GSContext. Sprite de clear aplica máscara completa e alpha forçado; versão incompatível falha explicitamente. Testes GPU passaram preservação alternada de canais RGB/alpha, máscara integral e FBA, além de GIF parcial/FIFO/cópia multipágina. Frontend real e build runtime passaram.
- PS2X_CALLBACK_QUEUE_TRACE registra crescimento em potências de dois, tipo/tag/PC do produtor e PC ativo. Testes IRQ pending/preemption passaram; SIF continua preservando cada pacote.
- Run parallel-clear-callback-mission.log reproduziu arena esgotada. Fila cresceu de64 até4096, com4 IRQ e4092 SIF; produtor kind7/tag6/PC286990, callback ativo PC2b28e8. Disassembly confirma espera por GIF_STAT10003020 AND1f000000: aguarda FQC não zero. Não é IPU. Isso localiza o bloqueio gráfico que causa acúmulo, sem provar ainda a causa do FQC ausente. Não aumentar arena nem descartar SIF.
- Usuário informou que imagem já corrompe ao confirmar Enter em Yes no aviso do memory card. Próxima reprodução deve capturar antes/depois desse ponto; não limitar investigação à fase. Run atual encerrada ao travar. Clear sozinho não demonstra cores ou gameplay corrigidas; imagem capturada ficou quase uniforme. Recomp jogável ainda sem percentual confiável.

- Teste manual: usuário confirmou corrupção imediata após Yes do memory card, GPU ABI2. Captura memory-card-yes-corruption.png mostra ruído colorido generalizado na introdução. Não apenas paleta da fase.
- Teste frontend ampliado: upload GPU CT32, BP2240/BW2, padrão128x96; todos12288 pixels lidos pelo decoder CPU iguais ao padrão. Não confirma scanout completo nem formatos de textura, mas exclui endereçamento CT32 básico nesse caso.
- PS2X_PARALLEL_STREAM_CAPTURE grava eventos GIF/reg/image/clear/write/reset e VRAM inicial, limite128MiB, diagnósticos ignorados. Regressão módulo com captura passou. Launcher TestarGPU ativa captura+auditoria+fila para reprodução manual do Yes. Jogo reaberto; captura parallel-memory-card-stream.bin, log parallel-gpu-test.log. Correção gráfica ainda pendente.

### Continuação IMAGE e foco na gameplay — 2026-10-02

- Causa comprovada: feedVif1DirectPayload insere GIFtag auxiliar de continuação IMAGE para decoder legado. Backend GPU tem estado persistente e consumia esse tag como pixels, deslocando comandos seguintes. Metadado imageContinuation agora acompanha payload VIF→árbitro→GS; raw GPU exclui16bytes auxiliares, decoder legado mantém pacote. Sem heurística nem descarte de imagem.
- Regressão frontend GPU: transporte antigo falha x4 (00008bff vs801234f1); correção compara12288pixels com padrão distinto em várias páginas e valida desenho seguinte PATH2. Teste VIF IMAGE/IMAGE2 e DIRECTHL passou. Build completo passou, runtime patch regenerado.
- Run parallel-image-continuation-manual.log entrou na fase. Usuário confirmou etapa após memory card correta e pediu foco gameplay. Captura parallel-image-continuation-gameplay.png tem HUD/arma/cenário mais reconhecíveis, mas texturas continuam coloridas/corrompidas. Não afirmar correção de todas cores. Sem percentual jogável confiável.
- Perfil de5s parallel-gameplay-profile.txt: GameThread3012amostras,2213 em caminho VIF/VU; commitReadyPipelines aparece372+225 amostras, executado no início de cada iteração e novamente em advanceOneCycle no mesmo ciclo. Movido commit de entrada para antes do laço (mantendo guarda budget/stop); advanceOneCycle ainda publica LSU antes de XGKICK. Teste150k compara commits simples vs referência dupla, passou; máscaras VI65536passaram. Build vu-single-commit-build.log em andamento; ganho de gameplay não medido ainda.
- Captura completa antiga128MiB preservada em parallel-memory-card-before-fix.bin; limite foi atingido cedo por uploads FMV, não representa toda confirmação. Launcher passou a deixar captura completa opt-in; auditoria e fila continuam ativas.

- Gameplay após IMAGE fix ainda esgotou arena, mesmos SIF tag6 bloqueados emGIF_STAT. Correção adicional: GS::downloadFifoQwc consulta bytes reais pendentes do backend, limitado16QW e arredondamento de últimoQW; memória GIF_STAT usa callback quandoBUSDIR1, reportandoFQC e direção. Sem contador inventado nem descarte callbacks. Teste gif_download_fifo_test.py cobrevazio/disponível/saturado/consumido/upload/mode; frontend GPU download16bytes→1QW passou. Build gif-download-fifo-build.log em andamento.
- Revisão aponta próximo risco concreto: VIF1 DMA direction0 ainda enfileira PendingTransfer para processVIF1Data como upload. Verificar transferênciaGS→EE real após desbloqueioFQC; não tratar pixels retornados como VIFcodes. Ainda não alterado.

- Integração gif-download-fifo-mission.log reproduziu freeze; correção downloadFQC não resolve espera deste jogo. Perfil~24s/60updates continua lento, sem ganho global claro. Diagnóstico gif-fifo-wait-detail.log: BUSDIR0, GIF_STAT00000006 (PATH3masked+IMT), downloadAvailable0, VIF1CHCR70000045/QWC0, callbackPC2b28e8. Portanto espera é upload mascarado, não download; não inventar dadosGS→EE.
- readGIF_STAT agora calcula FQC da fila real m_path3MaskedFifo quandoPATH3mascarado, cap16QW. Contador temporário limpo por advanceEeTimers não pode ocultar pacotes aceitos e retidos. Teste produção cobre64bytes→4QW,512bytes→saturação16, fila drenada→0, mais regrasdownload/upload. Build gif-masked-fifo-build.log em andamento. Ainda falta integração para confirmar causa da espera.
- Teste VU150k ampliado comqueueStore/FSSET/CLIP/FCSET e memóriaLSU; commits simples vsduplos equivalentes. Sem alegar FPS de microbenchmark.

- Build gif-masked-fifo passou. Run gif-masked-fifo-mission.log avançou Level_00 de g46.4 atég62.4/host134.2 sem callbackqueue/arenaexhausted. Antes bloqueavaFQC e fila crescia até4096. FQC de pacotesPATH3aceitos/mascarados não deve sumir comtickdeEE: readstatus deve refletir fila real, pois guest esperaFIFOocupado antes de liberarMSKPATH3.
- Amostras60updates nesta execução:8.0s,9.9s,8.4s,3.2s,6.8s,12.0s,12.3s; antes~24s. Execuções/câmera não controladas, portanto não alegar ganho global isolado nem FPS fixo. VUcontinuaCPU; flags/latências preservadas. Screenshot gif-masked-fifo-gameplay.png mostra arma/HUD/carros/prédios comcores mais naturais, mas geometria/texturas ainda incompletas, algum ruído. Não jogável completo nempercentual confiável.
- Jogo deixado aberto PID89132, run de3600s. Pergunta assíncrona enviada para validar WASD/IJKL (aguardando). Perfil gif-masked-fifo-profile.txt capturado para próxima otimização.

- Usuário confirmou IJKL gira câmera; WASD deslocamento não perceptível. Portanto câmera responde, movimento ainda não validado; não dizer que jogador anda. Teclado enviaanalógicoesquerdoemdata6/7 e direitoem4/5, mas falta conferir resposta do personagem/colisão sobbaixa taxa deupdates.

### Apresentação — tentativa revertida em 2026-10-02

- Na gameplay a janela ficou num quadro só (mesmos pixels por vários minutos) enquanto o jogo seguia desenhando na página 70 e o DISPFB continuava na página 0. Forçar a apresentação a seguir qualquer desenho grande fora do DISPFB trocou a tela do jogo inteiro, inclusive o carregamento (`srcFbp=70`). Revertido. Não repetir: o composto que o CRT escaneia não é o alvo cru do draw.

### Apresentação — não trocar o DISPFB por qualquer draw grande

- Tentativa de mostrar a página desenhada (FBP 70) no lugar do DISPFB (FBP 0) porque a janela da gameplay ficava no primeiro quadro. Isso passou a valer no jogo inteiro: o carregamento e as telas anteriores passaram a exibir o buffer interno (`srcFbp=70`). Revertido. A tela volta a seguir o DISPFB. A imagem parada da gameplay continua sem correção; não repetir esse atalho.

### Gráficos da gameplay — 2026-10-02

- Usuário pediu explicitamente ignorar WASD e priorizar gráficos. Trace pad-input acrescentado mas desabilitado por padrão; não mexer em movimento nesta etapa.
- VIF1 UNPACK divergências confirmadas contra PCSX2 upstream Vif_Unpack.cpp: V2 deve repetirXY emZW; V4-5 expandeRGB5bits<<3/alpha1bit<<7; STMOD3 atualizaROW; WL0representa256. Runtime preservavaZWantigos, não expandiaV4-5, ignoravaROWmode3 e tratavaWL0como1.
- Teste vif_unpack_test.py executa ramoUNPACK real: versão antiga65544divergências; correção passouV2 signed/unsigned8/16/32, todas65536coresV4-5 inclusiveSTMOD2ignoradoparaesseformato, ROWmode3 eWL0fill. V3 W/CL0/truncamento ainda não corrigidos, exigem validação de alinhamento/continuidade; não aproximar comportamento.
- Build vif-unpack-build.log em andamento, ainda sem validação visual da fase. Não alegar gráficos completos a partir dos testes.

### Gameplay: isolamento da regressão visual UNPACK
- Screenshot do usuário confirma HUD legível e cenário coberto por faixas/ruído; testes sintéticos não validaram o resultado visual.
- Build vif-unpack-isolation-build.log passou. Seletor diagnóstico PS2X_VIF_UNPACK_FIX_MASK: V2=1, V4-5=2, ROWmode3=4, WL0=8, V3-8W=16; default runtime31. Teste V3-8 acrescentado, passou junto dos anteriores; alinhamento ainda exige verificação integrada.
- A/B com mesma entrada automática na missão: mask27 mostrou prédios/chão, mas texturas e cores severamente corrompidas (vif-mask27-world.png); mask31 voltou a cobrir cena com faixas verdes e roxas (vif-mask31-world.png). ROWmode3 agrava corrupção nesta implementação, não prova de que regra de hardware esteja errada.
- Launcher TestarGPU.command usa temporariamente mask27 (override por ambiente). Isso é isolamento/mitigação, não correção completa. Próximo diagnóstico: interação ROW/máscaras/V3 e continuidade UNPACK entre pacotes. WASD fora do escopo atual a pedido do usuário.

### UNPACK V3 W — 2026-10-02

- O bit 16 do seletor zerava W em todo UNPACK V3, menos no primeiro vetor com alinhamento ímpar. O modo 3 copia esse W para ROW, e o ROW zerado pintava as faixas verdes/roxas da máscara 31.
- Hardware (PCSX2 `UNPACK_V4` no slot V3): W é o elemento seguinte da fonte. Se esse elemento começa um bloco de 16 bytes, W é 0. Vale para V3-32, V3-16 e V3-8. O teste `vif_unpack_test.py` cobre o V3-8 nos quatro alinhamentos e um V3-32 de dois vetores. Build do runner passou.
- Usuário confirmou em 2026-10-02: os artefatos no fim do carregamento, depois da dificuldade, sumiram. As texturas da gameplay e a lentidão continuam.

### Texturas de 4 bits e custo do raster — 2026-10-02

- Dump ao vivo (`g_gsTextureDumpRequest`) na missão: o céu é PSMT8H 1024×256 em tbp 6720 e a leitura em ordem é uma imagem de nuvens. As paredes são PSMT4 (0x14), 128×128, tbw 2, por exemplo tbp 11680. A leitura em ordem desses índices já sai em blocos; a paleta em si é um degradê, não lixo. Reinterpretar o índice como linear ou aplicar o swizzle de novo piora a correlação com o vizinho, então não é swizzle duplo.
- Esses draws usam TEX1 com MMAG linear e MMIN 4 (mip linear, nível mais próximo). O runtime não escolhe mip: amostra sempre o nível 0, com quatro leituras por pixel. Isso alia a parede em faixas e concentra o tempo em `SampleTexture`.
- Amostra de 3s do PID 15782: a GameThread fica em `0x2B2638` → VIF1 → XGKICK → `DrawTriangle`. Não há threads do raster paralelo; os triângulos não passam do corte de 8192 pixels. A thread da janela dorme em `EndDrawing`.

### Seleção de mip — 2026-10-02

- O sampler agora calcula o LOD do TEX1 (`log2(1/|Q|) * (1<<L) + K`, K em s7.4). LOD ≤ 0 usa o nível 0 e o filtro de magnificação. LOD > 0 com MMIN ≥ 2 escolhe o nível mais próximo, limitado por MXL, e MMIN 4/5 filtram o texel. MMIN 3 e 5 ainda não misturam dois níveis. Endereço e buffer vêm de MIPTBP1 (níveis 1–3) e MIPTBP2 (4–6). Slot 0/0 fica no nível 0.
- TEX0 com MTBA ligado preenche só o MIPTBP1, textura tratada como quadrado em TW, formato Z ignorado, TW fora de 5..10 ignorado. Draws com MMIN ≥ 2 e MXL ≥ 1 ficam seriais para não ler um mip junto com o framebuffer.
- `gs_mip_test` passou no CPU: Q=0.5 com MMIN 4 cai no mip verde; Q=1 e K=−1 ficam no nível 0 magenta. O sprite de UV também passou. O runner foi reconstruído.
- Quadro ao vivo (upd 1561, `/tmp/black_frame_0.ppm`): o HUD está nítido, mas o mundo é quase duas cores sólidas, (90,74,16) e (74,99,24). Não são as faixas do nível 0.
- Os triângulos dessa parede têm 1–3 pixels. Q≈0,024 e o intervalo de S/Q é ~0,04, ou seja, menos de um texel por pixel na textura de 128. O LOD por `log2(1/Q)+K` caía no nível 2–3 e o triângulo virava um texel só. O nível agora segue a densidade de texels na tela, com o K do TEX1 só como viés. O teste cobre densidade 2 (mip 1), densidade 1 (base) e o triângulo curto com K=−3,125 (base, não o mip azul). A página de 4 bits continua listrada; isso só impede que ela suma.
- Dump na mesma cena: o céu PSMT8H em tbp 6720 continua uma foto de nuvens. As paredes PSMT4 128×128 têm MIPTBP preenchido pelo jogo (TBW permanece 2 em todos os níveis) e Q≈0,012–0,024. Com K=−3,125 o LOD cai em 2,2–3,2, então o desenho usa o nível 2 ou 3 (32×32 ou 16×16), não a textura cheia. O nível 0 dessas paredes, lido na hora, não é foto: uma é cinza chapado e as outras são faixas vermelhas/verdes. A paleta de uma delas alterna vermelho e verde. Escolher o mip não corrige essa imagem de 4 bits; com esse Q, a imagem cheia nem chega na tela.

### Redesenho plano da parede — 2026-10-02

- Com o LOD por densidade a sala aparece (janelas, vão, chão) e o HUD continua nítido, mas a parede vira faixas. A página PSMT4 em si não é o defeito principal: o atlas de HUD em tbp 11616 (512×256, PSMT4) lê como retículas e silhuetas, e uma parede (tbp 15872) lê como um painel com a letra P. A tabela de coluna continua a do PCSX2.
- Cada triângulo de parede é submetido duas vezes, com o mesmo XYZ, o mesmo Q e o mesmo teste de Z (GEQUAL). A primeira vez o S/Q cobre a textura (intervalo ~1 numa parede de 481×358). A segunda o S/T troca e o intervalo cai para ~0,03, com ABE desligado, então esses poucos texels substituem a textura. Por isso a faixa.
- O segundo submit é ignorado quando a posição coincide e o intervalo de UV é menor que 0,08 e menor que 35% do intervalo que ficou na tela. `gs_flat_redraw_test` cobre isso: o redesenho plano não troca o pixel, um triângulo novo ainda desenha.
- A heurística só do último triângulo falha na missão: o segundo ST chega depois de outros kicks, e strip troca a ordem dos vértices. O frontend agora guarda 64 triângulos texturizados, compara XYZ sem ordem e só ignora o smeer se ABE estiver desligado. O teste interpola um triângulo no meio e reenvia os vértices rotacionados.

### GPU Present + EE/VU — 2026-10-04

- JogarBlack usa paraLLEl-GS. Present ainda converte DISPFB (FBP 0, CT16) na CPU; não é FBP 70.
- PMODE 0x8003 lia o mesmo FBP duas vezes (CRT1/CRT2). `readDisplay` agora faz um `map_vram_read`. Convert+blend dual do mesmo FBP foi pulado. `getenv(PS2X_TRACE_CALL_STACK)` saiu do caminho quente de cada call. Espera Q/P/XGKICK no VU rápido não chama mais `calculatePairReadyCycle`.
- Medido com PAD script + CUTSCENE_SKIP, janela aberta: menus ~30 upd/s. Level_00: primeiro burst 1201→1321 em 31.8 s (~3.8 upd/s, 0.7 M→6.1 M prims). Ângulos mais leves 1441→1741 ~19–25 upd/s. Vista pesada 1741→1861 ~5.7–5.9 upd/s. Ainda não é jogável. Próximo corte grande: scanout GPU sem readback CPU a cada vsync.

### Level_00: ativação real do scanout GPU — 2026-10-04
- Baseline ABI3 level00-performance-baseline.log: milhares de Unknown video format, scanout0x0, fallbackCPU ativo. Entrada upd1201→1321=29.9s (~4upd/s), vista posterior ~6.8–7.4s/60updates. Perfil level00-performance-baseline-sample.txt preservado.
- Causa: GSParallelScanout/GSPresentationRequest não transportavam SMODE1; syscall GsSetCrt configurava sóSMODE2. SMODE1LC/CMOD zerados impediam classificação do sinal no paraLLEl-GS.
- Implementado transporteSMODE1 ABI4 + configuração real NTSC2/PAL3 no syscall conforme tabela e campos públicos do ps2sdk. Outros modos não alterados. Sem falsificar sucesso do scanout.
- Build runner passou; teste módulo real GPU passou imagem64x32 e cores, mais regressões de transferência/clear. Integração level00-smode1-performance.log em andamento; ganho de gameplay e imagem ainda precisam confirmação.

- Integração revelou que o jogo não chama SetGsCrt nesta sequência e escreve DISPLAY literal640×448/MAGH0/SMODE2=0. O boot HLE foi tornado consistente NTSC (SMODE1, interlace/frame e DISPLAY), mas guest sobrescreveDISPLAY: scanout analógico saía2560×224 e recorte quebrava menus. Não contar essa execução como melhoria.
- Acrescentado patch paraLLEl-GS internal_resolution_scanout: resolução e retângulos da imagem final vêm dos circuitos internos, sem extents/offsets do sinal analógico. EndereçosDISPFB, formatos e composiçãoPMODE mantidos; registros do guest não reescritos pelo módulo. Build aplica patchnovo independentemente dos fixesGIF/páginas.
- TesteGPU cobre circuito64×32, duploNTSC640×448 e configuração literalBlack (SMODE2=0, MAGH0, segundoDBY1/DH446): dimensões e cores passaram. Menus voltaram legíveis, scanout640×448 real. SequênciaPAD inicial perdeu sincronização; execução level00-native-performance-mission.log acrescenta confirmações após52s para chegar à fase. Ganho e gráfico da missão ainda não medidos.

- Integração final: scanout640×448 ativo, sem Unknown video format; menus legíveis em level00-native-start-mission.png. Runtime/framebufferGPU raster continuaativo. API4 e patch interno publicados no build, patch runtime regenerado. Ainda existe cópiaGPU→host ewait_idle porframe; não é apresentação semreadback.
- PADscriptconfirmou Start/Cross mas ficou emStartMission nesta sequência, portanto não há benchmarkLevel_00válido apóscorreção. Pergunta ao usuário pede entrada manual na missão; PID89970 deixadoaberto. Não alegar aumentoFPS nem%jogável. Comparação anterior ROWmode3 invalidada pela auditoria: todosUNPACKmode0 e0truncamentos nas execuções capturadas; variante de máscara não provou causa.

### 2026-10-04 — Pesquisa externa de GS/VU e recomp

Pesquisa em fontes primárias paraLLEl-GS, PCSX2 microVU/MTVU, OpenGOAL e PS2Recomp registrada em `ps2recomp/diagnostics/renderer-research-2026-10-04.md`. Prioridade técnica: benchmark real do Level_00, capturas diferenciais VU1 e execução por blocos nativos; posteriormente sincronização por fence e apresentação sem readback. microVU consultado é x86 e requer adaptação para ARM64. Execução atual permanece no frontend; nenhuma nova alegação de ganho em gameplay ou alteração executável nesta pesquisa.

### 2026-10-04 — Primeira otimização conservadora da VU superior

`execUpper` reconhece NOP da tabela especial (seletores 0x2f/0x30) antes de normalizar VF/ACC/Q/I. Mantém `m_currentUpperInstruction`; ciclos, lower e pipeline continuam no executor original. Referência anterior congelada em `ps2recomp/tests/vu_upper_reference.inc`; `vu_nop_test.py` passou 100000 NOPs com bits aleatórios e controle de instruções reservadas adjacentes. Teste cobre estado/instrução e rejeita qualquer tentativa de escrita via hooks; não é validação completa de programas VU. Microbenchmark isolado 13.01ms→12.77ms, diferença pequena, sem alegação de ganho de gameplay. Runner compilado e patch persistente regenerado.

Execução real `recomp/diagnostics/level00-vu-nop.log` com GPU, scanout640x448 e frame visível verificado (`level00-vu-nop.png`, vídeo da bandeira). Ainda frontend feState28/cur004bc220 em t52.7. Deixada aberta na sessão2427 por até3600s. Pergunta pede ao usuário pular vídeo com Tab e entrar na missão; benchmark Level_00 ainda pendente. Não é backend VU nativo e não resolve sozinho a lentidão nem corrupção gráfica.

### 2026-10-04 — Lentidão real no Level_00 e flags/stores esparsos

Usuário confirmou travadas. PID40353 em `level00-user-test.log` entrou no Level_00 cur004bcf78: primeiros intervalos 60updates/15.1s e18.1s, depois aproximadamente60/6.2–9.1s; execução continuava, não congelamento completo. Frame `level00-before-pipeline.png` mostra cenário/arma mas corrupção visual severa. Perfil `level00-user-test-sample.txt`: VU run361, commit247, upper227 amostras diretas; VU é gargalo relevante. Trechos posteriores durante compilação têm contenção de CPU e não servem como benchmark.

Adicionadas máscaras de ocupação `m_flagPending`/`m_storePending`, usadas em commit para visitar apenas slots válidos em ordem crescente, mantendo readyCycle, ordem de flags/sticky/clip e writes de memória. Todos os quatro produtores de flags e queueStore marcam slots; commit limpa e resetScheduler zera máscaras. Teste `vu_sparse_commit_test.py` expandido para FMAC flags e invariantes de ocupação, 150000 operações diferenciais passaram incluindo filas cheias/reset/FSSET/FCSET/CLIP/stores. Testes de todas65536VI masks e100000NOP também passaram. Benchmark sintético do teste compara com oracle antigo de scans completos, portanto seu fator não representa ganho desta mudança isolada nem FPS.

Runner recompilado (211targets), patch regenerado. Execução nova PID41959, sessão3881, `level00-sparse-pipeline-user-test.log`, GPU640x448. Usuário entrou na missão durante a espera: cur004bcf78 t51.8;60updates/14.2–15.3s (~3.9–4.2updates/s) na abertura/cutscene, ainda lento. Novo perfil `level00-sparse-pipeline-sample.txt`: commit190, upper233, run395 amostras diretas; cargas/câmeras diferentes impedem atribuir ganho percentual. Frame `level00-after-pipeline.png` contém texto da abertura '4 DAYS EARLI...', não comparar com cenário anterior. Jogo permanece aberto; pergunta de fluidez pendente. Corrupção e backend VU nativo continuam não resolvidos. Não alegar gameplay corrigida.

### 2026-10-04 — Hot path VI/XGKICK e teste de timing

Novo perfil real `level00-current-stall-sample.txt` confirma execução continua (não freeze): VU run403, upper277, commit198, XGKICK74 amostras diretas. Implementadas duas otimizações cpp locais: `writtenVi` seleciona o primeiro bit de `viWrite & 0xfffe` via countr_zero, preservando a leitura antiga e ignorando VI0; XGKICK copia qword em blocos contíguos com wrap circular, sem alterar um qword por dois ciclos nem ordenação de LSU/GIF.

Teste novo `vu_execution_hotpath_test.py` extrai os trechos de produção e compara com a semântica anterior: todas65536 máscarasVI e61082 cópias com buffers minúsculos, tamanhosVU reais, offsets desalinhados, wrap e último qword do pacote. AddressSanitizer/UBSan passaram. Testes150000queue/commit e65536dependency masks também passaram. Runner compilado, patch runtime regenerado.

Reaberto PID43114/sessão2391, log `level00-vu-timing-test.log`, com PS2X_VU_ACCURATE=1 apenas nesta execução: investigação da corrupção visual por latência VF/VI/ACC. Ainda sem confirmação de ganho gráfico ou FPS. Launcher padrão não foi alterado. Em t50.7 continuava frontend, scanoutGPU640x448 ativo; entrada manual em Level_00 pendente. Pergunta pede comparar prédios/chão/arma. Não declarar native VU implementado nem corrupção resolvida.

### 2026-10-04 — Resultado de timing VU e correção de arbitragem GIF

Teste PS2X_VU_ACCURATE entrou no Level_00: t123.7 upd3361→t142.5 upd3421→t161.0 upd3481 (~3.2updates/s). Captura `level00-vu-timing-latest.png` mostra cenário/HUD e texturas severamente corrompidas. Portanto atrasosVF/VI/ACC não eliminam o problema; não concluir que não contribuem nem comparar câmeras diferentes como teste causal completo. Não habilitar modo lento no launcher padrão. Processo43114 encerrado após teste.

Encontrado defeito demonstrável em `GifArbiter::drain`: stable_sort com exceção PATH3 IMAGE vs PATH2 DIRECTHL viola strict weak ordering (PATH2 normal equivalente a DIRECTHL pelo pathId, mas comparação distinta com IMAGE). Substituído por seleção entre cabeças das três FIFOs, preservando ordem por caminho e prioridadePATH1. DIRECTHL cede ao IMAGE na cabeçaPATH3; payload mais atrás não salta sobre configuração anterior. Cópia/move de fila protege callbacks que enfileiram; capacidade reaproveitada, caminho1packet sem ordenação. submit rejeita path inválido. Trace `PS2X_GIF_ARBITER_TRACE` registra drains mistos em potências de2. Teste C++ `gif_arbiter_order_test.cpp` com ASan/UBSan passou5040permutações, FIFO, priorityPATH1, regressõesDIRECTHL/IMAGE, continuação e enqueue reentrante. Isso não prova que o defeito causava a corrupção do Black.

Runner recompilado e patch runtime regenerado. Execução nova PID44108/sessão85124 em `level00-gif-order-test.log`, sem PS2X_VU_ACCURATE e com tracearbitragem. Em t46.7 frontend cur004bc220, GPUscanout640x448 ativo, nenhumdrainmisto registrado até então. Entrada manual na missão pendente, pergunta enviada. Performance e imagem após esta correção ainda sem confirmação. TestesVU hotpath61082copies/todas65536VImasks passaram nesta etapa. Não alegar backendVU nativo ou recompjogável concluído.

### 2026-10-04 — Paletas GPU, fence de apresentação e snapshot real

Execução pós-arbitragem entrou no Level_00, mas `level00-gif-order-current.png` continua com corrupção severa de texturas/HUD. Nenhum `[gif-arbiter]` misto registrado no log inspecionado; correção de ordenação é válida por teste, mas não demonstrada como causa desse frame.

Novo teste realGPU `parallel_gs_texture_test.cpp` e target `black-parallel-texture-test`: 4096pixels por caso PSMCT32/PSMT8/PSMT4, CLUT32 CSM1 eCSM2, seis casos passaram em MoltenVK/M1Pro. Teste inicial tinha sobreposição de páginas do próprio fixture (CLUT256×1 ocupa4páginas); corrigido TBPpara256 antes de tirar conclusão do renderer. Inspector de stream agora conta CPSM/CSM: captura20851eventos usa16798T8 e17008T4 todosCPSM32/CSM1, além24T8H; nenhumPSMtransfer reservado. Isso não valida mipmaps/cache/reutilização ou uploads reais do jogo.

Scanout substitui device.wait_idle por Fence específico da cópia, preservando COPY→HOST barrier e vida do buffer. Teste módulo ampliado para32quadros de cores diferentes e pixels em topo/meio/final da área válida; passou. Ultima linha do segundo circuito é ausente pela configuração447linhas, por isso não é usada como oracle de cor cheia. LeituraGPU→CPU ainda existe; não alegar zero-copy nem ganhoFPS comprovado.

Snapshotdiagnóstico único `PS2X_PARALLEL_SNAPSHOT_REQUEST`: criar arquivo no caminho configurado capturaVRAM4MiB+JSON TEX0/TEX1/FRAME/CLAMP dos2contextos eTEXCLUT/TEXA; remove trigger só ao terminar. Validado comfixture VRAMprimeiropixel80554433. Não é save state completo, não contém cacheCLUT interno. Adicionado decodificador offline `ps2recomp/diagnostics/gpu_texture_snapshot.cpp`, CSM1/CPSM32 apenas; lêcaptura real semalterarjogo.

Execução novaPID46594/sessão91247 `level00-fence-texture-test.log`, comsequence PAD9up/9.5cross/20select/22cross/24cross/26cross/30select/40select; entrouLevel_00 duranteexecução, sem afirmar que não houveinputhumano. GPU640x448 ativo. Snapshotreal `level00-gpu-texture-snapshot`capturado readback2963: TEX0=2005bfe59d40ade0 TBP11744 TBW2 PSMT4 128×64 CBP11775 CPSM32 CSM1 CSA0 CLD1. Palette16entradas naVRAM todas402bb0dc epreviewuniformeamarelo (`level00-current-texture.png`). Pode ser textura/efeito uniforme ou paleta já reutilizada depoisdo draw; snapshotfimdequadro não capturaCLUT interno e não prova uploaderrado. Próxima investigação: conteúdo de paleta no momento do TEX0/CLD e origem dosuploads. Corrupção gráfica e VUnativa seguem pendentes. Modulebuild/test/README atualizados; jogo permaneceaberto.

### 2026-10-04 — NEON na VU e correção de permutações MMI

Testes GPU de textura ampliados para uploads CT16/CT16S 224×240, BP1025/BW8/DSAX48/DSAY24 atravessando páginas: roundtrip completo passou, junto aos seis casos de texturas/paletas. Inspector da captura real contou 8944 uploads CT16, 4802 CT32 e1 CT24, sem transferências inválidas ou tamanhos parciais de HWREG. Isso valida casos cobertos, não elimina todos os defeitos de upload/CLUT.

Normalização dos três vetores VF/VF/ACC em execUpper agora usa NEON ARM64; Q/I e fallback scalar preservados. Teste extrai código real, quatro milhões de valores bit a bit incluindo signed zero/subnormals/Inf/NaN e alias in-place: passou. Microbenchmark scalar13.90ms vsNEON3.90ms, checksum igual; fator3.6x é somente normalização, não FPS. NOP regression passou. Perfil pré-mudança `level00-before-neon-sample.txt`.

Encontradas permutações incorretas PEXEH/PEXEW/PROT3W no runtime e expressões inline do gerador, comparadas ao PCSX2 MMI.cpp. Corrigidas para halfwords[2,1,0,3,6,5,4,7], words[2,1,0,3] e[1,2,0,3]. Gerador usa os helpers testados. Disassembly ELF contém quatroPROT3W em32c218/334650/339d40/33b3bc; nesses quatro usos as operações seguintes consomem só64bits inferiores, iguais no comportamento antigo, portanto não atribuir corrupção à descoberta sem evidência. PEXEH/PEXEW não encontrados nesseELF. Teste100001inputs ASan/UBSan passou. Patch único agora inclui ps2xRecomp/src/lib, skill atualizada; build.sh recompila translator antesde regenerar. Fullbuild `mmi-correction-build.log` em andamento. PID46594 encerrado para rebuild; validação real pós-mudanças pendente. Gameplay/corrupção ainda não resolvidas e percentual restante não mensurável.

Validação pós-mudanças: fullbuild terminou com207targets, analyzer/recompiler executados; runnerPID49471 sessão53007, `level00-neon-mmi-test.log`, backendGPU640×448. EntrouLevel_00 cur004bcf78 t≈69.8/g44.4. Captura `level00-neon-mmi-current.png` ainda mostra corrupção severa apesar de cenário/HUD; MMI não resolveu gráficos. Últimos trêsintervalos60updates/13.3,13.3,13.4s (~4.5updates/s), contra pré-run14.4–14.8s em outro momento/câmera, insuficiente para ganho causal percentual. Perfil pósNEON `level00-after-neon-sample.txt`: run415,commit211,upper148 amostrasdiretas, pré run350/commit212/upper243; distribuição muda masVUcontinua gargalo. Nenhum guest-fault/missing-target/scratch exhaustion/reservedVU encontradoaté t159.2/g58.4. Jogo permanece aberto para teste. Alteração acidental de PEXCH revertida ao código anterior (não existeuso noELF), translator recompilado e patch regenerado; PEXCH/PEXCW/PREVH continuam fora da correção desta rodada. Próximo foco: captura da paleta no momento do carregamentoCLUT e diminuir custo de decode/dispatch daVU; desempenho e gráficos ainda não atendem jogável.

### 2026-10-04 — Inserção direta nas filas VU e diagnóstico de paletas corrigido

Perfil real anterior mantinha run415/commit211/upper148 amostras diretas. Inserção VF/VI/ACC/store/flags agora seleciona menor slot livre via countr_zero da máscara de ocupação, mantendo capacidade, ordem de slots, ciclos e erros. Capturado oracle anterior `vu_queue_reference.inc`, teste diferencial150miloperações passou (inclusivequeuefull/reset/WAW/flags/stores), runner recompilado cpp apenas e patch regenerado. Teste real `level00-queue-mask-test.log` PID50811 chegouLevel_00; intervalos tardios60/13.3–13.4s (~4.5updates/s), semelhante ao anterior, sem ganho real significativo demonstrado. Processo encerrado para carregar novo diagnósticoGPU.

Teste texturaGPU ampliado: CT16/CT16S reinterpretados como PSMT8/PSMT4, comparando4096pixels com oracle independente das tabelas de swizzleCPU.14casosrender +2roundtrips16bit passaram. TEX0 agora enviado viaGIF A+D no fixture. Isso não elimina defeitos em mipmaps/reuso deCLUT/dadosreais.

IMPORTANTE: a hipótese de paleta uniformemente402bb0dc da captura anterior foi invalidada. O decodificador `gpu_texture_snapshot.cpp` não chamava InitLookupTables, repetindo a leitura do primeiro endereço de cada página. Corrigido; palette anterior agora varia. É erro do diagnósticooffline, não correção dos gráficos do jogo. Novo teste compara4096RGB/indices com fixture realGPU, garantindo inicialização correta.

Instrumentação one-shot `PS2X_PARALLEL_SNAPSHOT_ON_TEX0=1` captura no primeiro TEX0 indexed que muda e carregaCLUT após trigger, antes de draws seguintes. Enquantotriggerexiste apenas, rawGIF é dividido porqword até captura; resto permanecebatched. Semtrigger não altera render normal. JSON registra stage=tex0. Fixture capturado e16entradas iniciais passaram após correção do decodificador. Não captura cacheCLUTinterno mas capturaVRAMno ponto do carregamento. Nova execução `level00-tex0-test.log` sessão28706 comGPU e caminho `level00-tex0-snapshot`, trigger será criado apósLevel_00. Corrupção e lentidão continuam; sem percentual validado para jogável.

Validação completa do diagnóstico: `gpu_texture_snapshot_test.py` passou4096RGB +4096índices, stage=tex0 e remoçãodo trigger. Run novaPID51239 entrouLevel_00 t67.5/g56.4. Trigger capturouVRAMantesdosdraws readback3225 (`level00-tex0-snapshot.*`), TEX0ctx0=20061fe59940b0e0: T4 64×64 TBP12512 CBP12543. Paleta varia (ex6666a858/8666a866/66668886), textura extraída contém ruído dourado/verde. Não concluir causa sem comparar dados de upload e selecionar precisamente contexto alterado. ctx1tambémindexed TEX0=2005cc0621312d60. Snapshot não registraqualcontextodisparou, próximo refinamento deve registrar isso. Até g62.4 execução continua eframe640×448; sem prova de gameplay corrigida. Run permanece aberta. Aprendizado: tabelas de swizzle não inicializadas causam falso diagnóstico de paleta uniforme; inicializar antes de lerVRAM e verificarfixture realGPU. FilasVU: varredura de slots livres substituída por máscara mantendo menor índice, equivalência passou mas ganho da missão ainda limitado.

### 2026-10-04 — Captura do CLUT interno e origem dos uploads

Adicionado `audit_texture_uploads.py`: decodifica transporte capturado, mostra uploads/TEX0 para CBP/TBP selecionados e extrai transfers completas até primeiroTEX0matching. `trace_palette_writes.cpp` usa swizzleCPU para rastrear bits/bytes do blocoCLUT atingidos por uploads. Na captura histórica CBP12543 recebeu paletas normais alpha7f, CT16 BP12512 64×64 e posteriorCT32 BP12512 128×128 atingem essa região. Isso identifica reutilização/overlap, não prova comando errado: a VRAM pode ser reutilizada depoisdoCLUTcache. Outro banco15135 termina com palette7f212121 no primeiroTEX0matching histórico; o momento difere da captura atual, não comparar como causa demonstrada.

TesteGPU de preservação CLUT: carregarTEX0/CLD1, sobrescreverVRAMda paleta, desenharcomCLD0. PSMT8 ePSMT4 passaram4096pixels, juntoa14casosanteriores +2roundtripsCT16. Portanto backendpreserva cache nos casos cobertos.

Patch dependência `parallel-gs-clut-snapshot.diff` adiciona acessores somenteleitura de bufferCLUT/instância; build.sh aplica apóspatchscanout. Snapshot one-shot agora exporta `.clut.bin`1024bytes e JSON trigger_context/clut_instance. CopyGPU→host combarreiraALL_COMMANDS→COPY→HOST eFence. O cacheCT32 é armazenado em2planos256halfwords(RG eBA), não256uint32 intercalados; decoderopcional5ºargumento reconstrói cores. TesteGPUfixture verifica256corescache, contexto0 e4096RGB/indices nosdoisdecoders: passou. Buildmodule7targets passou, patchreversecheck passou. Não alterada a renderização normal semtrigger.

Run atualPID53912/sessão60179 `level00-cached-clut-test.log` entrouLevel_00 t47.5/g44.4. Snapshotrealreadback2654,trigger_context0,clut_instance892,TEX0=200763e5dd40bae0 (T4 128×128 TBP15072 CBP15135). Cacheinterno palette[0..15] exatamenteigual aosvaloresVRAMestranhos (0653a9b3/0644cabe/182d71f2...), preview ainda ruído colorido. Confirma que neste instante o cache carregado contém dados incorretos para uma paleta visual coerente; não é só VRAMreutilizada depoisda cópia. Não prova ainda qualupload ou escrita guest causouisso. Jogo continua aberto, semcorreção gráfica/performance nova nesta rodada. Próximo foco: correlacionar transferências dessa mesmaexecução antesdoTEX0 e localizarúltimaescrita, evitando comparações de momentos/bancosdiferentes. Sempercentual validado parajogável.

### 2026-10-04 — Correlação exata entre uploads e CLUT da mesma execução

Snapshot/stream agora registram capture_current_event, capture_events, capture_bytes e trigger_packet_bytes; cut_stream_at_snapshot.py rejeita registro incompleto e corta o último pacote no TEX0 exato. Captura encerra ao salvar snapshot. Limite configurável 16–1024 MiB, disparo automático por readbacks e filtro PSM opcional. Fixture GPU PSMT8/PSMT4 passou4096RGB/indices e metadados; CLI de auditoria passou fixture de ordinal/evento/payload. README documenta protocolo. Diagnóstico pesado não serve para medirFPS.

V1/V2 atingiram limite antes do ponto-alvo, portanto não são evidência correlacionada. V3 capturou menu PSMT8, CPU reconstruiu exatamente cacheGPU, validando rastreador. V4 (`level00-correlated-test-v4.log`, antigoPID55960) entrouLevel_00; stream iniciareadback2450, snapshotPSMT4readback2500 ctx0CLUT510 TEX0=2005afe55d40ad60 TBP11616 CBP11647. 82196eventos/445502688bytes, evento final confirmado capturado, prefixo48bytes. Último upload que afeta os256bytes daCLUT é#53276 CT32 BP11616 BW1 64×64/16384bytes. Os16valores reconstruídosCPU batem exatamente comCLUTinternoGPU, incluindo df577064/7a59574d/33bb8d47/33d64589. Demonstra origem nosbytes/comandos recebidos nesseinstante, não identifica ainda qualregra de guest/DMA/sincronização está errada. Não inferir que CT32sobrepaleta é sempre inválido: reutilização VRAM é legal e precisa analisar sequência.

Auditoria com --trace-after localiza#53276 emevento82195 PATH3 offset4524672 (completo), depoisTEX0 emevento82196 PATH2 offset32. O grandeeventoPATH3 tem4691712bytes/4045GIFtags/14EOP e nenhumGIFparcial; payloadexato se repete33vezes na janela. O banco recebe primeiro paleta válida, depois uploads que a sobrepõem antes doTEX0 selecionado. Suspeita de sincronização/ordem ou referências guest; hipótese de defeito de swizzle/cor/cacheGPU não demonstrada.

Adicionado PS2X_DMA_CHAIN_MAP (somente quando configurado) ao runtime: registra uma vez tags originais/address/offset de primeira cadeiaGIF>4MiB. Patch único regenerado; build4targets passou. Nova execuçãoPID57288/sessão76614 `level00-dma-chain-test.log`, semcaptureGIFpesado, entrouLevel_00 t38.7/g32.4. Mapa inicial4792368bytes/1429tags terminaEND7 e não atinge4096limite. Repetição de umendereço13vezes correspondeCALL/RET de subcadeia; não prova cicloinfinito. Este mapa é da primeira cadeia grande da novaexecução, não exatamente doevento82195 anterior; não correlacionar offsets entreelas. Frame `level00-dma-chain-current.png` continua com cenário/HUD e severa corrupção. Intervalo t87.8→101.2 contém60updates/13.4s (~4.5updates/s); semganhoFPS nesta rodada. Runner permaneceaberto; gráficos/lentidão não corrigidos e percentual atéjogável segue não mensurável. Próxima investigação: DMA/VIF/GIF concorrentes e referências de textura quando osuploads reutilizam os mesmosbancos, usando endereços dos tags e rastreio de chamadas guest.

### 2026-10-04 — Contrato de envio/consulta DMA do SDK

Comparado ELForiginal sceDmaSend29c4f8/sceDmaSendN29c560/sceDmaSync29c5e8 (`dma-sdk-original-disassembly.txt`) comHLE. SDK faz `(CHCR & 0xfffffff3) | 0x105/0x101`, preservandoTTE/TIE/ASP/tag/configuração; HLE forçavaTIE1 e descartavaoutrasflags. Corrigido helperdmaSendChcr. SDKSendN gravaQWCdireto (hardware16bits); removidaheurística que tratava argumentos>65535comobytes. Syncmodo1 agora consultaCHCR.STR diretamente, removidafilaartificial deumpollocupado; demais modos processamtransferências síncronas disponíveis. Isso nãoimplementa concorrência/stallDMA e não deve ser vendido como correçãocompleta derender.

Testedma_sdk_contract_test.py extraihelpersreais eSync, compara65536configurações cominstruçõesdoELF, limitesQWC/TIE/TTE/ASP/tagbits, repetidasconsultasSTRativo/inativo, modobloqueante e runtimenulo: passouASan/UBSan. Patchúnicoregenerado. Primeirobuild22targets passou; segundobuild apósSync emandamento, testeLevel_00 pósalteração pendente. Nenhumganhoperformance/correçãográfica afirmadoantesdorun.

Validação pósSDK: segundobuild22targets concluiu; runnerPID59073/sessão89843 `level00-dma-sdk-contract-test.log` entrouLevel_00 t67.8/g62.4. LogSDK agora0x101GIFnormal/0x105VIF1chain, emvezde0x181/0x185, comoELF. Run anteriorPID57288 encerrado t≈27 da novaexecução para evitarinstâncias simultâneas; não usarintervalos iniciaiscomoFPS. Semguest-fault/missing-target/exhaustion inspecionados. Frame `level00-dma-sdk-current.png` ainda tem corrupção forte; intervalos60updates/14.9e13.7s no início da missão, insuficientes paraganhoperformance e semelhantes ao padrão lentoanterior. CorreçãoSDK válida, masnão resolvepaleta/geometriacompleta. Jogo atualpermaneceaberto. Próximaprioridade continua sincronizaçãoCPU-DMA-VIF eendereçamento/seleçãodosuploadsreais; não percentualjogável mensurável.

### 2026-10-04 — Prioridade explícita: desempenho do Level_00

Usuário pede priorizar desempenho, deixando cores/diagnósticoDMA como secundários. BaselinePID59073 atualizado:60updates/12.8–12.9s emmissãoestável (~4.65updates/s), perfil5s `level00-before-result-neon-sample.txt`: run655, commitReadyPipelines392, execUpper267, execLower186, normalizeFmacResult150, calculateFmacProductSticky149 amostrasdiretas. MainThreadaguardaEndDrawing, threadEE/VU concentra trabalho. Não confundiresperado60apresentaçõesdaGUIcom60updatesjogo.

Otimizado normalizeFmacResult noARM64 comNEON: quatroresultadosZ/S/U/O normalizadosporbits, mesmasflags, flushsubnormais/signedzero/clampInfNaN, mantémresultadosdelanesinativas. ModoPS2X_VU_EXACT_FMAC e outrasarquiteturas mantêmcaminhoscalar. Testevu_result_neon_test.py compara4milhõesvalores/flagscomscalaroriginal, todas16máscaras, IEEEedges e lanesinativas: passouASan/UBSan. NOPregression100mil passou. Patchúnicoregenerado; build4targets emandamento. Runnerbaselineencerrado para lançamento apósbuild. GanhoFPS ainda não validado; GPU/corrupçãopermanecemforaescopodestaotimização.

Pósotimização: build4targets passou, runnerPID60179/sessão72828 `level00-result-neon-test.log` entrouLevel_00 t41.7/g38.4. Perfil5s `level00-after-result-neon-sample.txt`: normalizeFmacResult104 versus150amostrasdiretaspré; run670/commit389/upper244/lower196/sticky140. A distribuição mudou, masnãointerpretarcomo30%FPS. Intervalos60updates/12.0,9.1,14.5,13.0s no início, ainda variáveis/limitados, semganhoFPScausal conclusivo. Frame `level00-result-neon-current.png` salvo, gráficoscontinuamcorrompidos. Nenhumguest-fault/missing-target/exhaustion encontrado noslogsinspecionados. Jogoaberto para teste. Próximo foco: run/commitReadyPipelines (maiorescustosCPU), mantendoequivalência esem desativarpipelines/flags parafingirvelocidade. Prioridadedesempenho permanece.

### 2026-10-04 — XGKICK reutiliza buffer sem limpar 64 KiB por envio

Continuada prioridadedesempenho. Duranteinspeçãodecommit/ciclo, encontrado startXgkick zerando todaestrutura incluindoarraypacket64KiB a cadaenvio. progressXgkick sobrescreve cadaqword antesdeleitura/emissão efinish sóenvia totalBytes copiados; limpeza daporção não usada édesnecessária. Agora reiniciaexplicitamente todososcontroles source/total/copied/tagEnd/EOP/credit/issue/active, mantendoarrayentreenvios. resetScheduler continuareinicializandotudo. Nenhumciclo/tamanho/byteválidoalterado.

Referênciaoriginalvu_xgkick_start_reference.inc, testevu_xgkick_reuse_test.py compila três métodosreais eoraclecomresetcompleto; 10milenvios commemóriacircular, tagsmúltiplosPACKED/REGLIST/IMAGE, payloadaleatório ebufferreutilizadocomlixo: bytes/ciclos/controles iguais, ASan/UBSan passou. vu_sparse_commit_test.py150miloperaçõespassou. Build4targets passou, patchúnicoregenerado. Baselineanterior finais60updates/13.2–13.3s. Novorun `level00-xgkick-reuse-test.log` sessão95566 lançadoparavalidação; nãoháganhoFPSdemonstradoaté medirmissão. Prioridadecorescontinuasecundária.

RunXGKICKPID61403 chegouLevel_00 t52/g42.4. Intervalosiniciais60updates/13.8,11.8,8.9,14.6,13.0s variam e não demonstramganhoFPScausal. Perfilpós `level00-after-xgkick-reuse-sample.txt`:run686/commit422/upper217/lower170/sticky159. A confirmaçãodasfilas permanecemaiscaraqueexecUpper. Frameatualsalvo, coresnãoresolvidas.

Segundaotimização: commitReadyPipelines aposaplicarentradas só invalidaentry/store/write/fdiv, sem zerarpayload/timestamps deentradas járetiradas; máscaras continuamlimpas. Enqueue inicializaentradasreutilizadas, todasleituras deexecução verificamvalid/máscaras. Testescomparadosaooracleoriginal expandem150miloperações paraQ/P alémVF/VI/ACC/flags/stores; equivalência deregistros/memória/capacidade/WAW/validadepassou. PID61403encerradopararebuild4targets `vu-retire-build.log`, novaexecução pósambasotimizaçõespendente. Patchregenerado. Não alegarganhodeFPS atémedir.

Validaçãofinalduasotimizações: build4targetsconcluiu, runnerPID62146/sessão48383 `level00-retire-test.log` entrouLevel_00 t51.9/g48.4. Finais60updates/13.1e12.9s (~4.6updates/s), comparávelao13.2–13.3s anterior, semganhosignificativodemonstrado. Perfil `level00-after-retire-sample.txt`:commit398/upper267/lower155/sticky140/normalize98; dependedejanela, não%FPS. Nenhumguest-fault/missing-target/exhaustion/reserved noslogsinspecionados. Framesalvo `level00-retire-current.png`; cenáriopermanecedescolorido/corrompido, semafirmaçãodecorreçãográfica. PID62146segueaberto. Otimizaçõesreduzemescritasredundantes masnão resolvemo gargaloestruturalrun/commitdointerpretadorVU; próximo foco deve ser reduzirtrabalho porinstrução/ciclo (ou execuçãoVU compilada) comoraclediferencial. Prioridadedesempenho; sempercentualatéjogável mensurável.

### 2026-10-04 — Continua otimização offline; não deixar jogo aberto

Usuário pedeencerrar/deixarnãoaberto parateste e iniciarmaisotimizações. PID62146encerrado. pipelinesPending substituicinco varredurasdearrays porORdas máscarasocupação, preservandoFDIV/XGKICK/EFU. Testediferencial150miloperaçõesagora compara tambémconsultaPending emcadaoperação, inclusiveQ/P/reset/capacidade: passou.

calculateFmacProductSticky hoista seleção de broadcastcomponente/Q/I paraforado laço delanes, normalizaoperandobroadcastuma vez emvezde4vezes e mantémswizzlescross/operandosporcomponente. Classificação/precisãoaritmética/flagsnãoalteradas. Referênciaoriginalvu_product_sticky_reference.inc, teste300milinstruçõesaleatórias, opcodes/specials/todas16máscaras/IEEEedges: flags eestadoiguais, ASan/UBSanpassou. Patchúnicoregenerado. Build4targets `vu-pending-broadcast-build.log` emandamento. GanhoFPSnão medido; alteraçãooffline não anunciadacomocorreçãodegameplay. Jogo permanecefechado conforme preferência; desempenho continua prioridadedesta investigação.

BuildpósPending/broadcast concluiu4targets semerros. TesteFMAC ampliadopara primeiros32768casos cobrirtodas2048combinaçõesopcode/special×16máscaras, seguidoaleatórios até300mil: passou. Testes/build completos; runrealpósalteração eFPS permanecemnão medidos, jogo fechado. Preferênciado usuário: não deixaraplicaçãoaberta entreiterações; continuardesenvolvendo desempenho.

### 2026-10-05 — Catálogo real da VU para recompilação antecipada

Coletoropt-in PS2X_VU_CATALOG foiadicionadoao run daVU; imagenscompletas de código porhashFNV64 eeventos24bytes unit/PC/hash/generation, hashing apenasquandomudageração. Arquivosreais emrecomp/diagnostics (ignorados). Parsercatalog_vu.py valida checksum eagrupachamadas porimagem/PC/gerações, trataeventofinalincompleto. Fixtureschecksum/gerações/PCs/tailpassaram; build4targetsconcluiu, patchregenerado. Documentação VU_CATALOG.md lista limites: imagenspodem conterváriosprogramas; contagens incluemresume e nãorepresentamtempoCPU; coberturalimitada.

Run120s `level00-vu-catalog-test.log` terminou efoiencerradoautomaticamente, nenhumrunneraberto. EntrouLevel_00, últimoregistro t112.8/g70.4. Catálogoem `recomp/diagnostics/vu-catalog-level00/catalog.json`:1678848calls,37imagens(3VU0/34VU1), tail0. VU0hash77d305d95d59af9a800468calls/588geraçõesidênticas PCs0388/0880/0550/0390; VU1c2835de1d8dfddf0439561calls/227gerações PC1758(439107calls)+0000/0038; VU17271631fc285cb81135054calls/61geraçõesmesmosPCs. Repetiçãodehash entregerações demonstra recarregamentodebytesidênticos, nãoprovaautomodificação. candidates.md compara duasimagensVU1ePCs. Há candidatos estáveis para reconhecimento/buildAOT, mas ainda não hátradutor/backendcompilado implementado nemganhoFPS; próximo passo rastrearblocos/variantesalcançados e implementarprimeirotradutor comoracleinterpretador. Percentualjogávelnãomensurável; prioridadeVU/desempenho, coressecundárias, nãodeixarjogoaberto.

### 2026-10-05 — Cobertura real e primeiro emissor AOT upper experimental

Adicionadacoberturaopt-in porPC antesdedecode emrun, `coverage.bin` registros16bytesunit/PC/hash; bitmap porimagemativa evita repetirPC atéimagemmudar. Reentradasdeimagemrepetemregistro; parserdeduplica. DiagnósticofazI/Oe não émediçãoFPS. Build4targetspassou epatchregenerado. Run100s `level00-vu-coverage-test.log` encerrouautomaticamente, semrunneraberto; chegouLevel_00 t45.4/g38.4. 1608362run/resume calls/36imagens,16bytesfinais incompletosignorados (SIGTERM/interrupçãobuffer). Candidatoc2835de1d8dfddf0:667PCsdeparespercorridos, faixascontíguas0..180,238..770,ae0..f90,1090..1a00 (fimexclusivo). Isso écoberturaPC, nãoordem/blocosCFGdefinitivos/ciclos.

Novo emit_vu_upper_aot.py traduzestaticamenteADD/SUB/MULbc/vector eNOP emC++comregistradores/operandosliterais, compartilhahelpersnormalizeOperand/applyFmacDest, defaultunsupportedfalse. Imagemidentificada porhashconstante; futura integração precisaverificarimagemantesdedespachar. Protótipo emrecomp/diagnostics, assetsfora docódigoversionado. Nãoexecutalower/controle/branches/ciclos/dependênciasparalelas; nãoestáintegradonobackendruntime, não éVUinteirarecompilada.

Testevu_upper_aot_test.py compilaexecUpperdeprodução juntoemissor, interceptorhelper compararesultadosaritméticos/argumentos/máscara/destino/estado/currentUpper. Helpersdeflags/queueinterceptados, portanto teste não validaflagsou pipelinesdoprogramainteiro. Versãofullimagem1403upperPCs×64entradaspassouASan/UBSan (incluiregiõesnãopercorridas); versãofiltradapelacoberturareal emite372de667upperPCs e295pedemfallback. Cobertura parcialupper nãoépercentualdeVUrecompilada nemganhoFPS. DocumentaçãoVU_CATALOG.md atualizada. Próximo: completarupperFMAC/specials, traduçãolower eseleçãoidentidade/execuçãoparcomtiming, antesdehabilitare medirjogo.

Testeversãofiltrada372sites×64inputs(23808casos) passouASan/UBSan `vu-upper-observed-aot-test.log`. Jogoencerrado. Próxima etapaAOTautorizada emcontinuações; desemepenhoprioritário, semganhosdeFPSafirmados.

### 2026-10-05 — Emissor AOT ampliado e validação lower

Emissor experimental agora inclui upper ADD/SUB/MUL/MADD/MSUB (vector/broadcast/Q/I), MAX/MINI e OPMSUB, além de NOP. Para imagem c2835de1d8dfddf0 nos 667 PCs observados: 509 corpos upper e 213 corpos lower suportados. Lower inclui operações VI IADD/ISUB/IADDI/IADDIU/ISUBIU/IAND/IOR, NOP e literal I; integração deverá preservar I anterior durante upper e aplicar imediato depois. Sem cargas/stores/branches/scheduler de pares ou dispatch no runtime.

Teste upper ampliado randomiza ACC/Q/I e distingue helper FMAC de applyDest: 509×64=32576 casos passaram ASan/UBSan, comparação exata de aritmética/argumentos/estado (helpers de flags interceptados). Novo vu_lower_aot_test.py extrai corpos de casos do interpretador de produção, compara estado completo em 213×256=54528 casos com limites VI, aliasing presente nas instruções observadas e literais reais; passou ASan/UBSan. Logs vu-expanded-upper-aot-test.log e vu-lower-aot-test.log. Compatibilidade com interpretador não prova correção hardware; flags/pipelines/dependências e XGKICK ainda precisam oracle completo. Nenhuma mudança runtime neste passo; nenhum FPS medido e nenhum jogo aberto. Percentual até jogável continua não mensurável. Próximo integrar corpos com regras de pares, ampliar lower memória/controle e validar antes de habilitar execução AOT.

### 2026-10-05 — Orquestração experimental de corpos de pares

Emissor gera executePairBodies somente para interseção de PCs upper/lower suportados:168/667 no candidato c283. Verifica as duas palavras capturadas antes de alterar estado, recusando PC desconhecido ou código alterado sem execução parcial. Ordem upper antes de imediato I/lower preservada; mecanismo shadow segue decoded.upperVfShadowReg do scheduler, mas atuais lower suportadas só VI/I/NOP não exercitam conflito VF. Identidade da imagem completa ainda precisa ser verificada na futura seleção do runtime. Nenhum dispatch no jogo; stalls/flags/pipelines/PC/ciclos permanecem fora deste protótipo.

Novo vu_pair_aot_test.py compara168×64=10752 pares com oracles upper e casos lower extraídos de produção, incluindo13 sites I imediato com I anterior aleatório; comparação exata estado/helper e recusa de palavras alteradas passou ASan/UBSan. Log vu-pair-aot-test.log. Helpers FMAC interceptados: não valida flags ou scheduling completo. Próximo ampliar lower MOVE/memória/branches e testar conflitos VF reais; depois conectar corpos ao scheduler preservando flags/pipelines. Jogo fechado, sem FPS novo e sem percentual confiável até jogável.

### 2026-10-05 — Lower MOVE/MR32 e conflitos VF

Emissor lower ampliado para MOVE/MR32, copiando operandos antes da escrita por máscara, inclusive VF0 conforme corpos atuais do interpretador (normalização VF0 pelo loop externo continua fora do emissor). Candidato real agora237/667lower e186/667pares; upper permanece509. Teste lower237×256=60672 casos e pares186×64=11904 passaram ASan/UBSan. Referências MOVE/MR32 extraídas dos corpos de produção, não duplicadas manualmente. Recusa sem efeitos agora cobre também PCs da captura com alguma metade não suportada, além de palavras alteradas.

Nenhum conflito VF real nos186pares suportados: não assumir validação só por captura. Novo vu_pair_fixture.py gera2048pares sintéticos, ADD upper com MOVE/MR32 lower,16máscaras de cada metade×4formas (lower lê destino upper, lower escreve destino upper, ambos, VF0). Teste2048×64=131072casos passou ASan/UBSan, verifica estado e argumentos de escrita após mecanismo shadow, garantindo upper prioritária quando destino coincide e lower lê valor anterior quando necessário. Oracles ainda interceptam helpers FMAC: flags/pipelines/timing não validados; decoder shadow no teste é derivado de regs/máscaras para esse subconjunto. Compilação fixture O2 levou cerca2min; nenhum runner aberto. Próximo lower memória/branches e oracle scheduler completo antes de ativação no jogo. Sem mediçãoFPS/percentual jogável confiável.

### 2026-10-05 — Corpos lower de memória LQ/SQ/ILW/ISW

Emissor inclui LQ/SQ/ILW/ISW com offsets11bits sign-extended, wrapping por dataSize-1, teste addr+16<=dataSize, seleção ILW de componente por prioridade máscara e extensão16bits VI. SQ/ISW chamam queueStore; não escrevem memória imediatamente. Parâmetros vuData/dataSize opcionais nos corpos lower/pares; PCs de memória recusam null/zero antes de upper para evitar execução parcial. Preservado NOP lower0/canônico8000033c sem helper (runtime retorna antes do decode); teste anterior tratava NOP canônico como MOVE máscara0, estado igual porém chamada extra; corrigido.

Captura c283:394/667lower,509upper,309PCs com ambas metades emitidas. Isso não é309pares validados com scheduling:186pares sem memória continuam comparados;123novos pares memória ainda precisam oracle conjunto com conflitos/pipelines. Testevu_memory_aot_test.py extrai corpos de produção e intercepta queueStore:157×128=20096casos reais passaram ASan/UBSan,4tamanhos8/16/4096/16384, VI limites, mem aleatória; registradores/endereço/payload/máscara iguais, sem memória escrita antes de retirement. Recusa null/zero também viaexecutePairBodies confirma ausência de upper parcial. Fixture1024pares inclui um lower0=Nop;1023mem×128=130944casos passaram (todas16máscaras×4offsets×4formas de aliases/VF0). Helpersstore não retiram pipelines, normalizeOperand stub não usado nas instruções testadas; não prova timing/flags/hardware total.

Testes regressão237lower×256 e186pares×64 passaram após ampliação, ASan/UBSan. Logs vu-memory-aot-test.log/vu-memory-fixture-test.log. Sem mudanças runtime/buildrunner, jogo fechado, nenhum FPS medido e percentual jogável não mensurável. Próximo: integrar oracle memória+pares/pipeline e adicionar LQI/SQI/LQD/SQD/branches antes de backend ativo.

### 2026-10-05 — Validação conjunta upper+lower de memória

vu_pair_aot_test.py agora inclui os309pares suportados reais, extrai LQ/SQ/ILW/ISW de produção, recebe memória/tamanho e captura queueStore. Compara estado completo, argumentos helper, endereço/payload/máscara de stores e memória não escrita antes de retirement. Oracle aplica sombra VF quando upper escreve VF usado pela store ou destino da load (e MOVE/MR32). Corrigida ordem oracle I-bit antes de teste NOP lower: literal0 com I-bit deve atualizar I, não ser omitido. 309×64=19776casos reais,4tamanhos8/16/4096/16384, passaram ASan/UBSan emvu-pair-memory-aot-test.log.

Fixture memória --paired adicionada:512pares ADD+LQ/SQ,16máscaraslower×4upper(0/1/8/15)×4aliases/VF0×2opcodes. 512×64=32768casos passaram ASan/UBSan emvu-memory-pair-fixture-test.log. Verifica prioridade upper em loads com mesmo destino e stores lendo valor anterior upper. Helpers FMAC e queueStore interceptados; retirement/flags/ciclos ainda não verificados, não backendativo. Decode de conflitos no harness derivado dos campos do subconjunto, não oracle decoder completo. Sem alteração runtime, jogo fechado, nenhum FPS novo e percentual jogável não mensurável. Próximo: pipeline/flags com helpers reais e ampliar lower incrementais/controle antes de dispatch AOT.

### 2026-10-05 — Helpers reais de FMAC/flags/stores e commit

Novo modo --real-helpers no harness de pares extrai métodos atuais de produção: applyDest/Acc, normalização NEON/escalar/exata, cálculo sticky/exato, updateFmacFlags, applyFmacDest/Acc, queueStore/Clip, resetScheduler e commitReadyPipelines. Referência execUpper e corpos lower de produção usam os mesmos helpers reais que os corpos emitidos. Compara registros, entradas flag/store pendentes (valid/readyCycle/issueCycle/mac/status/extraSticky/payload/máscara), memória e estado após commits0..4; cada caso inicia scheduler limpo, não valida sequências arbitrárias/full scheduler.

309×64=19776casos reais passaram ASan/UBSan no modo padrãoNEON e outros19776 com PS2X_VU_EXACT_FMAC=1;512×64=32768casos sintéticos ADD+LQ/SQ passaram com helpers reais. Logs vu-pair-real-helpers-test.log/vu-pair-exact-helpers-test.log/vu-pair-memory-real-helpers-test.log. Total72320comparações neste passo. Stores somente tornam memória visível via commit real; flags FMAC aposentam na fila real. Isso elimina limitação de helpers interceptados para flag/store desses testes, mas ainda faltam stalls/queueVfWrite/queueViWrite/ACC scheduling, continuidade de programas, branch/budget/XGKICK e integração runtime. Não houve alteração runner ou FPS medido, jogo fechado. Percentual jogável segue não mensurável. Próximo validar scheduler completo e ampliar instruções lower para permitir execução AOT segura em runtime.

### 2026-10-05 — Sequências com decoder, dependências e filas VF/VI reais

Usuário solicita validar. Novo --scheduler no harness compara sequências sem reset entre pares, decoder/decodeInstructionPair, addVfRead/Write, calculatePairReadyCycle, markPairWrites e queueVf/Vi/Acc extraídos de produção. Bloco de run desde viWrites até recordViWriteForBranch extraído, mantendo captura de registradores, sombra, restauração/enqueue por latência e branchbackup. A única seleção interpretado/gerado ocorre na emissão dos corpos. Estado VF0/VI0 restaurado como run; avança ciclos/commit real com XGKICK inativo. No modo preciso calcula stalls até readiness; no rápido aplica regra sem stalls de VF/VI (subconjunto não usa Q/P wait). Após cada par compara estado/memória/ciclos, readiness, latest-write, masks e payloads/ready/sequence das filas válidas flag/store/VF/VI;16ciclos finais drenam filas e compara a cada commit.

40000pares (20000rápido+20000preciso) com309PCs reais passaram ASan/UBSan; repetição FMACexato40000passou; fixture512paresADD+LQ/SQ emsequências40000passou. Total120000pares sequenciais. Logs vu-sequence-scheduler-test.log/vu-sequence-exact-scheduler-test.log/vu-sequence-memory-scheduler-test.log. Teste é oracle do interpretador atual; seleção aleatória de PCs não segue CFG/faseoriginal e não valida hardware independentemente. Ainda não cobre branch/budget/resume/E/D/T/XGKICK; ACC pipeline extraído mas sem instruções ACC emitidas. Não chamar isso de scheduler do jogo inteiro validado. Runner não alterado, nenhum jogo aberto/novoFPS; percentual jogável segue não mensurável. Próximo completar controle e validação retomadas/budget, então conectar dispatch reconhecendo código e medir run limitado.

### 2026-10-05 — Lower branch e delay slot

Emissor lower ampliado para B/BAL/JR/JALR/IBEQ/IBNE/IBLTZ/IBGTZ/IBLEZ/IBGEZ. Imediato11bits assinado, PC+8+imm*8 mascarado conforme VU0/VU1, links(PC+16)/8, JR/JALR alvo lido antes de escrever link usando readBranchVi de produção para preservar backup. Condicionais castint16; destinoVI0 protegido. Corpos só agendam branchPending/Target/Delay1; aplicação feita no loop apósdelay como runtime. Captura c283agora509/667lower,509upper,403PCs com ambos corpos (não percentualrecomp).

Harness de pares extrai casos branches originais (fecha corretamente chaves internas), usa microAddressMask/readBranchVi reais. --scheduler incorpora bloco nextPc/branchPending/branchDelay do run: compara efeitos/delay em sequências, mas PCs das instruções ainda selecionados aleatoriamente e não peloCFG real. 40000pares captura e40000FMACexato passaram; fixture170pares (160branches offsets-1024/-1/0/1023×aliasesVI0/link/source,10IADDI para backup VI) passou40000pares sequenciais. Total120000neste passo, ASan/UBSan. Logs vu-branch-scheduler-test.log/vu-branch-exact-scheduler-test.log/vu-branch-fixture-test.log.

Tests lower não memória filtrambranches para oracle separado; memoryharness recebeu métodosbranch reais para links do template, semampliarscope. Nada conectado ao runner/jogo; novoFPS não medido. Ainda falta seguir CFG com budgets/resume/parada e XGKICK antes de backend ativo. Percentual até jogável não mensurável, prioridadedesempenho. Próximo validação de programas controlados porPC/retomadas e conexão dispatch segura com reconhecimento da imagem.

### 2026-10-05 — Programa por PC, loop, E-bit e budgets/retomadas

Novo vu_program_fixture.py cria8pares controlados: IADDIU inicializaçãoVI1, IADDI decremento com ADD, IBGTZ loop/backupVI, delay slotMUL, SQ/LQ dependentes, E-bit e delay slotNOP. --program usa --scheduler com busca porPC real da unidade, orçamento1/2/3/7/16; ao atingir budget durante stall avança/commita atélimite sem emitir instrução, preservandoPC. E-bit termina apóspróximopar e drena filas. Wrapper de orçamento/E foi implementado na bancada, não extraído de run inteiro; demais decoder/emissão/filas/PC/delay helpers são produção. Não afirmar oracle run completo.

Primeira execução detectou VU1State não inicializado no construtor reduzido da bancada (PC inválido); corrigido com memset explícito no harness de pares, incluindo realhelpers. Não era bug comprovado do runtime. Validação pósfix:1000execuções do programa (2modos×5budgets×100entradasfloat),8400slices passaram ASan/UBSan;1000/8400 repetidasFMACexato passaram. Compara estado/memória/filas/ciclos a cada retomada e estado final com interpretador sem interrupções; terminaPC64 sembranch/ebit. Regressão40000pares sequenciais reais combranches passou também apósinicializaçãofix. Logs vu-program-resume-test.log/vu-program-exact-resume-test.log e vu-branch-scheduler-test.log.

Ainda não cobre D/T/XGKICK, DMA/VIF nem CFG completo de imagens reais/invalidations entrebudgets. Código gerado não conectado ao runner; sem novoFPS/percentualjogável mensurável, jogo fechado. Próximo validar paradasD/T/XGKICK e preparar dispatch opt-in verificandoidentidade com fallback antes de rodar medição limitada.

### 2026-10-05 — Dispatch VU AOT experimental integrado ao runner

Usuário autoriza criar experimental. run daVU agora inclui header opcional black_vu_aot_generated.hpp via __has_include, ativaçãoPS2X_VU_AOT, apenasVU1+memory. CachehashFNV1a porgeração/ponteiro/tamanho, seleção deimagemrecognizada, conferênciaupper/lower literal antes dequalquerescrita; substitui somente bloco de emissão dos corpos por executePairBodies. Loop original de run preserva todo scheduler/PC/delay/E/D/T/budget/XGKICK; paresnão suportados/imagemdiferente fallback completo. AotHost local delegahelpers reais e forneceestado semeditarheaderglobal. CountersVU1 compiled/fallback a cada1048576pares eprimeirohit; nãoFPS.

build_vu_experimental.sh +build_vu_aot_bundle.py geram headerlocal a partircapturas, rejeitamimagensnãoVU1/16KiB. Bundleopcional2imagens. Capturasc2835de1d8dfddf0 e7271631fc285cb81, ambas403pares suportados entre667PCs observados. Header/metadata contendo instruções estão excluídoslocalmentedo Git tools; não incluídos no patch. Semheader buildsegue interpretador. Wrapperrun_vu_experimental.sh ativaAOT+GPU por120s padrão/encerramentoautomático. Patchúnicoregenerado com runtime, semassets.

Build4targets (3mostrados+globcheck) passou para1imagem e depois2imagens. Teste programa1000execuções/8400slices passou apósadaptaçãoharness para extrair bloco interpretado semwrapperexperimental; segundaimagem passou40000pares --scheduler. Primeiro run100s level00-vu-experimental-test.log entrouLevel00,212228645hits/732538331fallbacks noúltimocounter, semfault/missingtarget/reserveddetail/exhaustion inspecionados, encerrado. Detectou727variantecomfallback no início da gameplay; motivou bundle2. Run A/B sequencial100s cada emandamento sess99907: level00-vu-aot-two-images.log e level00-vu-aot-baseline.log; medir somente quando terminados. NenhumganhoFPS afirmado; usuário prefereencerrarapósteste. Documentação VU_EXPERIMENTAL.md ecompare_update_logs.py. Percentual jogável nãomensurável; gráficosnão corrigidos por este passo.

Conclusão A/B experimental2imagens: ambasexecuções100s finalizaram, semrunneraberto. ÚltimocounterAOT241218136compiled/612322728fallback (VU1), duasvariantesefetivamenteusadas. Nos mesmosupdates1201..1381 (180updates), experimental36.1s=4.986updates/s, baseline38.1s=4.724updates/s, diferença+5.54% nestaúnicapassagem. Inicializaçãochegougameplay emmomentosdiferentes, comparação usa counterscomunsapósmodemode0/g>=34. Não éFPSjanela nemganhorepetidostatístico; nãoafirmar5.5%garantidos oujogabilidadepronta. Logs finais level00-vu-aot-two-images.log / level00-vu-aot-baseline.log e vu-aot-comparison.json. Semguestfault/missingtarget/reserveddetail/exhaustion/assertion noslogsinspecionados (não prova rendercorreto). Experimentalprontoparatestecom run_vu_experimental.sh120; nãoativadopordefault. Próximo ampliar cobertura/baixarcustodedispatche medir repetidamente; gargaloVUe corrupção gráfica ainda nãoresolvidos.

### 2026-10-05 — AOT lower incremental integrado e testado no Level_00

Análise por PCs cobertos (não frequência dinâmica): maior conjunto lower unsupported eram SQI51/LQI28mais2LQD/SQD. Emissor inclui LQI/SQI/LQD/SQD comVI int16wrap, baseuint16×16, limite/dataSize-mask, VI0protegido, ordempre/postincremento, queueStore real. Guarda null/zero antes de qualquer incremento e antes deupper em executePairBodies. Ambasvariantes agora590/667lower e466/667pares completos versus403anterior; upper509inalterado. Adição81lowerPCs desbloqueia63pares, restantes impedidos porupperunsupported.

Oraclesmemory/paired extraem casosincrementais deprodução, decoder/queueVI cuidamdelatências. 40000sequências main+40000second+40000fixture256pares passaram ASan/UBSan; memfixtures256×128=32768 e238sites reais×128=30464 passaram4tamanhos8/16/4096/16384, limitesVI/máscaras/aliasVI0/memória; total183232comparações. Logs vu-incremental-scheduler-test.log/vu-second-incremental-scheduler-test.log/vu-incremental-fixture-scheduler-test.log/vu-incremental-memory-test.log/vu-incremental-real-memory-test.log. --incrementalfixture novo; lowerarithmetic-test filtra essesopcodes paraoracle separado.

Bundle2variantesregenerado ebuildruntime3targets+globconcluiu (vu-incremental-build.log). Bundlebuilder agora registra JSON sóhash/contagens e evita reescreverheaderigual; substitui metadataantiga da primeiraversão. Run90s level00-vu-incremental-test.log entrouLevel00, gameplay t53.9/g38.4 upd1141; t68.0 upd1201 e t80.1 upd1261. Últimocounter321541041compiled/409316431fallback(VU1); encerradoautomaticamente semrunnerrestante. Nenhumguestfault/missingtarget/reserveddetail/exhaustion/assertion noslogsinspecionados; isso não prova renderingcorrect. Esta versão466não recebeu novoA/B, portanto semganhoadicionalFPScomprovado (5.54% anterior pertenceversão403eumaúnicapassagem). Experimentalatualizadopronto via run_vu_experimental.sh120, defaultinterpretador preservado. VU0/variants/upperACC/movimentosflags/EFU/XGKICK instrução ainda fallback; próximo ampliarupperACC e reduzir custo de dispatch/commit. Percentual jogável não mensurável, gráficos continuam problema aberto.

### 2026-10-05 — Upper ACC AOT integrado ao experimental

Emissor upper agora reconhece specials ADDA/SUBA/MULA/MADDA/MSUBA em vector/bc/Q/I e OPMULA. Reutiliza fórmulas normalizadas e applyFmacDestAcc real; OPMULA é crossproduto sem subtraçãoACC (diferenteOPMSUB), laneW0; correnteupperliteral preservada paraflags. HostAdapterruntime delegaapplyFmacDestAcc, únicaalteração runtime persistida empatchúnico. Ambosmain/second agora623/667upper,590lower,554pares versus466. +114upperPCs desbloqueiam88pares; restanteupperconversões/ABS/CLIP continuafallback.

Harnessupperinterceptado agora suportawriteACC paraoracle argumentos/estado,623×64=39872casospassaram. Harnesssequencial comparaACCpipelinevalid/ready/sequence/mask/valores, além doestado/readiness/latestwrite jáexistente. Main40000+FMACexato40000+second40000+fixture576pares(36forms×16máscaras)40000 passaram ASan/UBSan; total199872comparações. Logs vu-acc-scheduler-test.log/vu-acc-exact-scheduler-test.log/vu-second-acc-scheduler-test.log/vu-acc-fixture-test.log/vu-acc-upper-test.log. Buffer-missing memoryharness ganhoustubACCabort paratemplate completo; não ampliescope de flags nesseharness.

Bundle2 regenerado ebuild3targets+globpassou(vu-acc-build.log). Run90s level00-vu-acc-test.log chegougameplay: t50.2/g34.4upd1021, t64.3upd1081, t76.6upd1141, t86.9upd1201. Últimocounter454678880compiled/336996000fallbackVU1; terminouautomaticamente, semjogoaberto. Semguestfault/missingtarget/reserveddetail/exhaustion/assertion noslogsinspecionados; não évalidação de render. NenhumA/B novo/ganhoadicionalFPScomprovado. Experimentalatualizado554pares via run_vu_experimental.sh120; continuaopt-in. Restantesvariantes/VU0/instruçõesnão suportadas permanecem interpretadas, porcentagemglobalVU/jogável não mensurável. Próximoupperconversões/ABS/CLIP eflagslower/efudivs; blocosnative/dispatch/commit precisamredução estrutural paraFPSsubstancial.

### 2026-10-05 — Upper conversões/ABS/CLIP completa nos PCs observados

Emissor inclui ITOF0/4/12/15 (rawbitsint32→float/divisor), FTOI0/4/12/15 (normalizeOperand, escaladouble, saturaçãoINT32_MIN/MAX, bitsint32emVF), ABS normalizado eCLIP bitwise rawxyz/W. Conversões/ABS destinamVFft, nãofd; clipdestmask não filtra flags, como produção. AotHostruntime delegaqueueClip real. Ambasvariantes agora667/667upperobservados e590/667lower/parescompletos; isso não significaISA/VU completa, VU0/outrasimagenscontinuamfallback. +36pares vs554 anterior;44upperPCs novos.

Testsupper667×64=42688passaramrandom; fixture640PCs(10op×16máscaras×4aliases/VF0)×64=40960passou comIEEEedges adicionadosao harness (signedzero/denorm/min/max/NaN/infinito/limitesint32). Sequências main40000+second40000+fixture40000passaram ASan/UBSan, incluindo queueClip real eflagsclip/valid/writesClip comparados. Total203648comparaçõesnestaetapa. Logs vu-conversions-upper-test.log/vu-conversion-fixture-upper-test.log/vu-conversion-scheduler-test.log/vu-second-conversion-scheduler-test.log/vu-conversion-fixture-scheduler-test.log. MemóriaharnessmissingbufferganhouqueueClipabort para linktemplate, semexecutarclipscope. Lower77PCs restantes:MTIR/MFIR,flagsmac/clip/status,DIV/Q/EFU/XGKICK etc.

Patchúnicoregenerado, bundle2/buildpassaram (vu-conversion-build.log). Run90s level00-vu-conversions-test.log entrougameplay:t54.2/g32.4upd961,t67.3/g34.4upd1021,t77.8/g36.4upd1081; encerrouautomaticamente, semguestfault/missingtarget/reserveddetail/exhaustion/assertion inspecionados. Não valida rendercorreto e não novoA/B; nenhumganhoadicionalFPSafirmado. Experimentalatualizado590pares, comando run_vu_experimental.sh120. PercentualglobalVU/jogável não mensurável. Próximolowermovimentos/flags/Q/EFU evariantes/VU0 para reduzirfallback, depoisnativeblock/dispatch/commit estrutural.

### 2026-10-05 — Lower transferências e flags AOT

Emissor inclui MTIR/MFIR, FCEQ/FCSET/FCAND/FCOR/FSEQ/FSSET/FSAND/FSOR/FMEQ/FMAND/FMOR/FCGET. Imediatos24/12bits extraídos com encodingcorreto; VI0 protegido nasdestinaçõesvariáveis, FCEQ/FCAND/FCOR escrevemVI1conformeprodução. MACcomparação16bits/FMAND-FMOR usamviuint16,FCGETclip12bits; mantémsemânticarawVI32doFastVU atéqueueViWriteprecisa normalizar16bits. MTIR lêbitsVFrawcomcompbits22:21 eextendsigned16;MFIR replica int16raw como32bits emVF por máscara, semnormalizaçãofloat. Adapterruntime delegaqueueFcset/queueFsset reais. Subiu590→642/667lower epares,upper667inalterado; +52pares ambosvariantes. Restam25lowerPCs (Q/DIV/EFU,ILWR/ISWR,XGKICK etc),VU0/outrosmicroprogramas nãofinalizados.

Harnesspaired oracle extrai todoscasosnovos deprodução, incorporaqueueFcset/Fsset real eativahelpers automaticamentequando setters estão presentes; compara também writesStatus/writesSticky/clipfieldsdeflags. Fixturevu_flags_fixture.py368pares mistura limitesimm/VI0/aliases/MTIRcomp/MFIRmáscaras, produtoresFMAC/CLIP e128pares comFSSET+FMAC/FCSET+CLIP nomesmo ciclo para validarordem/prioridade. Bateriafinal main40000+second40000+fixture40000sequenciais e368×64=23552corpos/helperscommits passouASan/UBSan (143552comparações). Logs vu-flags-scheduler-test.log/vu-second-flags-scheduler-test.log/vu-flags-fixture-scheduler-test.log/vu-flags-fixture-body-test.log. Lowerarithmetic-test filtranovosopcodesparaoracleseparado; memoryguardharnessganhoustubsabortFCSET/FSSET semexpansãoscope.

Patchúnicoregenerado, bundle/buildpassed(vu-flags-build.log). Run90s level00-vu-flags-test.log chegougameplay:t57.8/g34.4upd1021,t70.4upd1081,t80.2upd1141. Últimocounter601666071compiled/209931753fallbackVU1. Encerrouautomaticamente, semrunnerrestante; semguestfault/missingtarget/reserveddetail/exhaustion/assertion noslogsinspecionados. Não prova renderingcorreto, não énovoA/B/ganhoadicionalFPS. Experimentalatualizado642pares. PercentualglobalVU/jogável nãomensurável; próximoescalaresQ/EFU e ILWR/ISWR/XGKICK antesdeVUs/variants/blocosnative.

### 2026-10-05 — ILWR/ISWR no emissor AOT

Adicionados acessos inteiros indiretos ILWR/ISWR com endereço VI uint16 ×16, wrap pelo tamanho de memória, prioridade de componente da máscara, extensão signed16 da carga e fila real de store. Guard de memória é verificado antes do upper para impedir execução parcial. Ambas capturas passaram de642 para643/667pares; upper667inalterado. Restam24PCslower, VU0/outras imagens e otimização estrutural. Q/DIV/WAITQ ainda adiados: precisam cobertura explícita de espera FastVU e comparação da fila FDIV antes de ativação.

Fixture128pares (2operações ×16máscaras ×4aliases/VI0):16384casos memória e40000pares sequenciais passaram. Main40000 + second40000 também passaram com scheduling/commit real FastVU eaccurate sob ASan/UBSan:136384comparações nesta etapa. Nenhum ganho adicional de updates/s medido; percentual global jogável/VU permanece não mensurável.

Bundle2/build passaram (vu-indirect-build.log). Run90s level00-vu-indirect-test.log chegou gameplay, t69.3/g42.4upd1261 e t80.2/g44.3upd1321; encerrou automaticamente. Sem guest-fault/missing-target/reserved detail/exhaustion/assertion nos logs inspecionados e sem runner restante. Não confirma gráficos corretos ou ganho comparativo de desempenho.

### 2026-10-05 — Escalares Q no emissor AOT

Adicionados DIV/SQRT/RSQRT e WAITQ; adapter delega normalizeResult/queueQ reais. Operações preservam normalização, sinal/saturação, flags DI e latências7/7/13. WAITQ não escreveQdiretamente: scheduler original realiza espera antes do upper. Cobertura643→655/667pares por variante; restam12lowerPCs dessas capturas, além VU0/outrasimagens/EFU/XGKICK.

Harness usa oracle de corpos originais, queueQ real e compara FDIV valid/readyCycle/value bitwise/statusDi. FastVU reproduz esperas WAITQ/recursoFDIV, accurate usa decoder/readiness originais. Main40000+second40000+fixture256pares40000sequenciais passaram; fixture256×64=16384corpos/commits com extremos IEEE passou. Regressões memória239×128=30592 e lower237×256=60672 passaram. Total227648comparações finais; execução adicional bodypréedges não incluída na contagem. Sem ganho de desempenho novo medido e percentual global jogável/VU não mensurável.

Patch único regenerado, bundle2/build passou (vu-q-build.log). Run90s level00-vu-q-test.log chegou gameplay e encerrou automaticamente; último contador576823213compiled/178151507fallback. Sem guest-fault/missing-target/reserved detail/exhaustion/assertion nos logs inspecionados e sem runner restante. Não valida render correto nem ganho comparativo. Os12PCs restantes dessas capturas são XTOP/XITOP (special104/105) e XGKICK(108), não EFU; EFU/VU0/outrasimagens permanecem escopo geral ainda não finalizado. Próximo integração VIF TOP/ITOP e saída GIF XGKICK, mantendo scheduler e callback reais.

### 2026-10-05 — VIF TOP/ITOP AOT

Adicionados XTOP/XITOP: VI recebe TOP/ITOP &0x3ff; VI0 protegido. Ambos665/667pares (+10); somente2PCsXGKICK restantes nestas capturas. Fixture512encodings(2op×16VI×4sources×4masks), TOP/ITOP random e limites0/1/3ff/400/7ff/ffff/80000000/ffffffff; main40000+second40000+fixture40000sequenciais efixture512×64=32768body/commits passaram ASan/UBSan. Total152768comparações finais. Não mede percentual global VU/jogável ou ganho adicional de desempenho.

Investigação XGKICK: startXgkick inicia estado de PATH1, progressXgkick lê memória circular em qwords com crédito por ciclo, parseia múltiplosGIFtags/EOP, finishXgkick envia ao memory->submitGifPacket ou GS. O harness sequencial atual não avança progressXgkick, portanto ativação AOT foi adiada até validar esses eventos e sincronização LSU antes de PATH1. Próxima etapa deve instrumentar/checkar bytes/ciclos da transferência real, não usarstub de envio como prova.

Bundle2/build passou (vu-vif-build.log). Run90s level00-vu-vif-test.log entrou gameplay mais tarde nesta rota (t66.2/g62.4upd1861; t80.8/g64.4upd1921), encerrou automaticamente; último296910261compiled/87917131fallback. Sem guest-fault/missing-target/reserved detail/exhaustion/assertion inspecionados e sem runner restante. Teste limitado de integração, não valida gráfico e não há A/B/performancegain novo.

### 2026-10-05 — XGKICK integrado ao AOT

Emitter lowerXGKICK chama startXgkick(VIisuint16), adapter delega método real. Ambas capturas agora667/667upper/lower/pares, semfallback de opcode nessas imagens ePCsobservados; VU0/outrasimagenscontinuamfallback. Não éVUcompleta nem bloco nativo: corpos ainda usam scheduler/commits/dispatch porpar.

Harness --xgkick utiliza start/progress/advanceOneCycle/commit reais e captura finish somente na entrada do consumidor, sem alegar rasterizaçãoGS/GPU. Compara controlstate, payload,ciclo/envio,eexpectedbytechain independente. Fixture128encodings×64transferências=8192passaram; capturas128cada=256passaram, main40000+second40000sequênciasregressão(semXGKICKemmemóriaaleatória)passaram. Total88448casosfinais sobASan/UBSan. FormatosGIFpacked/reglist/image/image2, nloop0..3, tagsencadeadas/EOP, wrap, VI0/signednegative eLSUstore antesdePATH1 cobertos; tamanhos16KiB, semcertificarbudget/resume comkickativo ouconsumidorreal.

Próximo apósrun: ampliaroutrasimagens e/ou remover custo estrutural de dispatch/scheduler em blocos, medirA/B; cobertura completa das2capturasnão garante ganho deFPS. PercentualglobalVU/jogávelnão mensurável.

Patchúnicoregenerado, bundle2/buildpassed(vu-xgkick-build.log). Run90s level00-vu-xgkick-test.log chegougameplay:t66.2/g62.4upd1861,t80.6/g64.4upd1921; encerrouautomaticamente. Semguest-fault/missing-target/reserved detail/exhaustion/assertion noslogsinspecionados e semrunnerrestante. Não valida rendercorreto ou ganho adicional deFPS. Experimental667pares observado nessas2imagens pronto; próxima comparaçãoA/B e redução estrutural dispatch/commits, além ampliarVU0/outrasimagens.

### 2026-10-05 — A/B repetido interpretador vs AOT667

Usuário pediu medir antesVU0. Benchmark4runsABBA, mesmo runnerSHA/backendGPU/PADscript;120updatesaquecimento e180medidos após primeiroestadoGameplay. Interpretador5.0847/5.1576 média5.1212updates/s; AOT4.8913/4.9315 média4.9114, −4.096% nesta amostra. Nenhum ganho observado; não prova regressão fixa, magnitude sensível àjanela. Conferência contadoresiguais apósambaswarmups:120updates−18.98%/−4.39%;240updates incluindoaquecer−1.41%/−2.99%. Entrada varia e sampling60updates limita alinhamento; não frame/stateequivalence, não VUthroughputisolado, não FPS. Semfaults ou runnerrestante; AOTapenasruns2/3. Logs/resultsrecomp/diagnostics/vu-abba-667; relatório ps2recomp/diagnostics/VU_PERFORMANCE_2026-10-05.md. Nenhum código runtime alterado. Próximo profile scheduler/dispatch/fallback antesprometeraceleração oupriorizarVU0. PorcentagemglobalVU/jogávelnão mensurável.

### 2026-10-05 — Perfil e despacho único no emissor AOT

Usuário autorizouinvestigar performance pósA/B. samplemacOS5s/1ms eminterpretador eAOT durantegameplay, sequencialmente, sem alterarbinário durantecoleta. Arquivos recomp/diagnostics/vu-dispatch-profile; profile_vu_modes.py reproduz. TopstackI:run764/commit370/execUpper271/execLower185;AOT:run658/commit365/executeUpper168/executePair168 (amostras, não percentuaisCPU/tempo comparável). Callgraph confirmou3despachos AOT e chamadas separadasupper/lower.

Emissor passaexecutarcorpos diretamente em1switchPC comlambdas ehelperdeshadow; conserva guardwords/memória antesqualquermodificação, upper/lowerorder, normalize/queues/callbacksreais. Scheduler original intacto. Main40000+second40000+fixturememóriaADD/LQ/SQ40000sequências,main665×64=42560helpers/commits e8192+128transferênciasXGKICK passaramASan/UBSan; total170880casos. Bundle2/buildpassed(vu-fused-build.log). Bench4runsABBA pósfusão emandamento vu-abba-fused; ainda semganhoafirmado.

BenchmarkfusãoABBA terminou: I5.0279/4.4335 média4.7307; AOT5.0420/5.2478 média5.1449 (+8.7555% aritmético), porémIúltimavarioumuito e janelasglobaisdiferiram. Primeiropar alinhadoexato961..1141 (guest32.4..38.4) I35.8s/AOT35.7s,+0.28% praticamenteempate; segundoAOT1441..1621 vsI1141..1321 semintervalocomum apóswarmup. Performanceinconclusiva, não declarar ganho8.8% nemcompararAOTantigo isoladamente. Todosrunssemfaults, fechados, semrunnerrestante. nmfinalexecuteLowerAOTausente, mas1310símboloscontendoexecuteFusedBodies indicam helpersespecializadosnãoeliminados; lambdas não garanteminline, próximosprofilerhelpers/commits/blocos. ReportVU_FUSED_DISPATCH_2026-10-05.md. Correçãoestruturalmantidaexperimentalopt-in; defaultinterpretador. PorcentagemglobalVU/jogávelnão mensurável.

### 2026-10-05 — Redução de chamadas do helper AOT

Retomadahelpers/commits. Hipótese1: alwaysinlinehelper+todoscorpos; testessemânticos2capturas/XGKICK passaram, masmicroteste500kpares/3reps deu−14.46%FastVU/−9.04%accurate (curto, indicativo). Nuncaativado/buildado no runner; mantidoapenasmodoallopcional deexperimento. Hipótese2: inline somenteexecuteFusedBodies, corposdecididoscompilador.

Microbenchmarkantes/depois,2milhões deparesdeterminísticos porpath,6repetiçõesalternadas/versão, aquecimentosforaamostra. FastVUmedian0.240784728s→0.2297672905s (+4.795%throughput), accurate0.3337687905s→0.3227227905s (+3.423%). Estados+memóriachecksumsiguais emcadapath; resultadosvu-helper-microbenchmark-long.json. Éharnesssintético comdecoder/scheduler/commits reais, semEE/IOP/GPU/XGKICK/CFG/budget, não ganhoFPS/gameplay. Binaryharnesshelper650→0símbolosexecuteFusedBodies, bytes703384→515032.

Modohelperdefaultdoemissor; compilerpreservaheurísticaoriginal,allopcional. AmbasvariantesASan/UBSan40000+40000,fixturememória40000,fixtureXGKICK8192+captura128:128320comparaçõespassaram. Cobertura667pares porvarianteinalterada;VU0/outrasimagens ainda interpretadas. Bundle/buildrunneremandamento vu-helper-build.log; avaliar nmfinal e executar90s integração antesclaimruntime. Semalteraçãoheaders/runtimeCPP/semânticascheduler. PorcentagemglobalVU/jogávelnão mensurável.

Bundle2/buildhelperpassou(vu-helper-build.log), metadataJSONagora registra inline_modehelper. nmrunner1310→0símbolosexecuteFusedBodies, bytes85187984→84935648. Run90slevel00-vu-helper-test.log entrougameplayt66.4/g62.4upd1861 e t80.4/g64.4upd1921, fechouautomaticamente. Semguest-fault/missing-target/reserved detail/exhaustion/assertion noslogsinspecionados, semrunnerrestante. ReportVU_HELPER_INLINE_2026-10-05.md. Ganho+4.8/+3.4 éisoladosintético, nãoFPS/gameplay; nenhumA/Bend-to-end novo. Próximo commits/scheduler/eventos, preservandoordemLSU/PATH1/flags. Cobertura667porcaptura/VU0semalteração.

### 2026-10-05 — Commits: stores descartados e estatística

Investigadosstores:85/97PCscapturados fullmask. Fastpathcópia16direta vsRMW:micro2m/6repsFastVU−1.797%,accurate−0.121%;NEONmaskedselect−0.188%/+0.414%,ranges sobrepostos. Semganhoconfiável; ambasdescartadas,nuncaativadas nobináriorunner. Core restauradoexatobyteabyte aooriginalsnapshotvu-core-before-store-fastpath.cpp; patchregenerado. Testsvu_commit_store_test.py49152casosporvariante ebaseline restaurada passaramASan/UBSan;256máscaras, overlapping/future/invalid/null/small/unaligned/mixedflagsQP.

Novo--commit-statsnoharness(contadores somente,nãobenchmark), semduplicarcommitnaentradadecadapar, cadênciacomo runtime. Checksumsiguaisbaseline. 2mpar/pathFastVU2015942calls/5900755slotsvisitados/1662634retirados/570800nodue=28.315%;accurate2022631/13509088/4412889/186825=9.237%. Sequênciasintética, nãoCPUtime/gameplay; nãoestimarganhoFPSdessasfrações. Instrumentaçãorejeitadapelo benchmark_vu_helpers.py. Dadosvu-commit-statistics.json; relatórioVU_COMMITS_2026-10-05.md. Próximo cachedeadline/eventos requer validar enqueues/reset/budgets/samecycle/ordemflagsQP-LSU-VFVIACC/PATH1 e medir custo deatualizaçãodocache. Runnercontinuaúltimaversãohelper, nenhumjogoabertonestaetapa,VU0semalteração,percentualglobaljogávelnãomensurável.

### Diretriz do usuário — conversão antes de FPS (2026-10-05)

Usuário esclareceu que visa código convertido, não somente FPS, como base para melhorias futuras. Priorizar expandir recompilação e reduzir dependência do interpretador, mesmo sem ganho imediato. Avaliar conversão, correção e desempenho separadamente. Ganhos de0.1% também interessam, mas só afirmar quando medição distinguir ruído; não descartar conversão correta apenas por não acelerar agora. Próxima etapa proposta: inventariar e iniciar AOT dos microprogramas VU0 com validação de semântica/runtime específicos da unidade, sem presumir integração VU1 aplicável automaticamente. VU1 demais imagens e blocos nativos continuam backlog.

### 2026-10-05 — Primeiro microprograma VU0 AOT integrado

Diretrizconversãoprimeiro. CatálogoVU0:77d305d95d59af9a763203callbacks/162PCsobservados;757121c0db138fe6200700/95;686f3730c337d24c127/36. Callbacks não medemCPUtime/instructionfrequency. Convertido primeiro77d:162/162upper/lower/pares observados;fullimagem4KiB, não convertemos350PCsnãoobservados/VU0macro/ISA completa. Demais2imagenspermanecemfallback.

Bundle aceita --vu0(completo4096bytes), diferenciaunit/hash. RuntimeAOTagoraVU0 eVU1:seleção degetVU0CodeGeneration vsgetVU1CodeGeneration, cachethreadlocal2slots(comowner/gen/pointer/size), wordguardsinalterados,countersseparados[VU0 AOT]/[VU AOT]. Scheduler/timing/VUmacro/flags/PC logic nãoalterados. MesmoPS2X_VU_AOT opt-in; defaultinterpretador. build_vu_experimental.sh quartoargopcionalVU0, builder --vu0repetível;reportmetadataunit/image_bytes/inlinehelper. Capturascopyrightignoradas fora patch.

HarnessautoUnitVU0dosheadersnovo ecode/data4KiB, PCmask0xfff. Primary40000sequenciaisFast/accurate comdecoder/stalls/queues/flags/Q/branchdelay +162×64=10368corpos/commits até13ciclos passaramASan/UBSan;VU1regressão40000passou;90368comparações semânticas, identityguardtest3imagensrejeitaunidadeerrada ehashdesconhecido. OriginalCFG/budget/execuçãoVUmacro/hardwarePS2não certificados porharness. Patchúnicoregenerado,bundle3/buildpassou(vu0-primary-build.log).

Run90s level00-vu0-primary-test.log entrougameplay:t67.6/g38.4upd1141,t76.6/g40.4upd1201. ÚltimocounterVU0compiled39734835/fallback4305357,image77d. Encerrouautomaticamente,semguest-fault/missing-target/reserved detail/exhaustion/assertion noslogsinspecionados;semrunnerrestante. Não ébenchmarknovo/FPSnemvalidação gráfica. Conversãocorretaeintegradaéresultado destaetapa, independentedeganhodeFPS. Próximo converter757(95PCs) e686(36PCs), validarparticularidadesunit0/memória/flags eampliarblocos/CFG. Percentualglobaljogável/VUinteira nãomensurável.

### 2026-10-05 — Três imagens VU0 capturadas no AOT

Incluídas757121c0db138fe6(95/95pares) e686f3730c337d24c(36/36), além77d(162). Total293pares observados porimagem/VU0, não293instruçõesúnicas nemVU0inteira/ISA/macros/níveis completos. Duasnovasimagensapenasoperaçõesjáemitidas, incluindoMOVE/MR32/MFIR/flags/SQRT/RSQRT/WAITQ/inteiros/branch,semexpandirISAoualterarruntime. BuildscriptaceitaqualquerquantidadeVU0a partirquartoargumento. Bundle5imagens(3VU0+2VU1),seleçãounit/hash testada,rejeitaunidadeerrada/hashdesconhecido.

ASan/UBSan segunda40000sequenciais+95×64=6080corpos/commits;terceira40000+36×64=2304,total88384comparações novas passaram. Data/code4KiB/PCmask0xfff/Unit0auto metadata. ScheduleroriginalCFG/budget não certificado porsequênciasaleatórias; runtimeconservaagendamento/pausas. Buildpassedvu0-all-build.log. Headercapturadoignoradonãopublicado;CPPpatchruntimeinalteradodesdeprimeiraVU0.

Run90slevel00-vu0-all-test.log entrougameplay:t79.1/g48.4upd1441. LogVU0primeirocompiled1/fallback0hash686, hash757registradoemcompiled27262976/fallback0, últimocompiled34603008/fallback0hash77d. Portanto3variantespresentesnaexecução efallbackVU0zeronoúltimocontadoramostrado, não garantiaoutrosníveis/macro/novasvariantes. Encerrouautomaticamente,semrunner;semguest-fault/missing-target/reserved detail/exhaustion/assertion noslogsinspecionados. NãoA/B/FPSnovo nemimagemcorreta. Resultadoéconversãoeintegracão293PCs porvariantes capturadasVU0, prioridadeusuário. Próximoblocosnativos/CFG&budgets/retomadas,demaisimagensVU1/VU0macro. Porcentagemglobaljogávelnão mensurável.

### 2026-10-05 — Contrato inicial dos blocos VU0

O runtime agora chama os blocos para imagens VU0/hash correspondentes. A rotina de um par foi extraída de `run()` e compartilhada entre blocos e despacho normal; os stalls, filas, hazards, budgetEnd, avanço do ciclo, branch-delay, E/D/T e XGKICK continuam no mesmo processamento por par. `executeBlock` só agenda o próximo corpo se o scheduler deixar PC no sucessor esperado; budget/yield interrompe no limite e a execução retoma do estado real.

`vu_block_aot_test.py` passou com IBEQ tomado/não tomado, delay slot PC8, fatias 1–8 e SQ→XGKICK: SQ enfileirada em PC24 fica visível antes do payload PATH1 iniciado em PC32; retomadas não duplicam o evento. `vu_pair_aot_test.py /tmp/vu1-xgkick.hpp --xgkick` passou 8192 transferências usando `progressXgkick`/`advanceOneCycle` de produção: bytes, ciclos, formats/tags/EOP/wrap e ordem LSU comparados; GS/rasterização não cobertos. VU0 runtime Level_00 smoke e estado compilado estão no registro anterior. Ainda falta comparar estado/ciclos em um bloco completo com scheduler de produção e medir A/B; sem ganho FPS afirmado.

### 2026-10-05 — Bundle para as 33 variantes VU1 capturadas

Diretriz do usuário: continuar primeiro a VU; renderer fica para depois. Catálogo Level_00 local:33 hashes VU1 +3 VU0. O builder aceitava apenas2 VU1; `build_vu_aot_bundle.py` e `build_vu_experimental.sh` agora aceitam opções repetidas `--vu1` sem quebrar o segundo argumento posicional nem VU0. Emissor determinístico por hash/coverage, fallback preservado.

Bundle local inclui as33 VU1 e3 VU0 capturadas. Inventário emitter: VU1 11.387 pares-PC observados por hash,11.359 upper+lower completos,28 lower-PCs fallback; expansão de10.025 corpos completos sobre os2 hashes anteriores. São ocorrências estáticas por imagem, não instruções únicas/dinâmicas ou porcentagem da VU. 31 imagens novas somam85.566 chamadas registradas no catálogo da captura, não ciclos nem instruções executadas. Header20MiB; `ps2EntryRunner` 86MiB; build Ninja/LTO passou.

Smoke90s auto-fechado: logs registram17 hashes VU1 distintos entre os33 do bundle e os3 hashes VU0; backend paralelo produziu scanout640x448. Sem guest-fault/reserved detail/exhaustion/assertion. Isto confirma seleção/execução de múltiplas novas imagens em Level_00, não render correto nem jogabilidade completa. Log `/tmp/black_vu-all33-test.log`. Sem A/B ou ganho de FPS medido. Próximo converter os28 lower PCs ainda sem corpo onde forem seguros, conferir corpo/site com harness, e capturar imagens de outras rotas/fases antes de dizer cobertura total.

### 2026-10-05 — MFP e EFUs nas capturas VU1

Análise dos28 lower-PCs incompletos:12 eram MFP (4 PCs em cada uma de3 imagens),16 eram EFU ESQRT/ESIN/ERCPR. Emitter emite MFP como P broadcast a VF por máscara com applyDest; decoder/scheduler legado conserva latência/write tracking. Fixture MFP passou oracle de corpo com64 estados; três imagens passaram cada uma40.000 pares sequenciados FastVU+accurate com decoder/stalls/filas/flags/stores.

O smoke MFP de30s (`/tmp/black_vu-mfp-test.log`) carregou Level_00/scanout paralelo, mas não registrou contadores VU; não é evidência de execução AOT na gameplay.

Em seguida, ESQRT/ESIN/ERCPR foram adicionadas nos16 sites lower restantes. ESQRT/ERCPR usam queueP real; ESIN reutiliza a normalização polinomial do interpretador e queueP com latência29. Harness compara64 estados por corpo com helpers do interpretador; sequência EFU de3 pares passou40.000 pares FastVU e accurate comparando filas, stalls, latências e estado. Runtime passou a expor queueEsin ao AOT e o patch `ps2recomp/patches/0001-black-runtime-fixes.patch` foi regenerado.

Bundle33 VU1 +3 VU0: 11.387/11.387 pares-PC VU1 e293/293 pares-PC VU0 observados têm corpos emitidos. Ninja/LTO compilou o runner. Estes totais são estáticos, condicionados às imagens capturadas; não representam ISA completa, frequência dinâmica, percentagem global ou jogabilidade terminada. Smoke de90s auto-fechado em `/tmp/black_vu-efu-test.log`: atividade AOT em9 hashes VU1 e2 VU0, fallback zero nos contadores amostrados, sem guest-fault/reserved detail/exhaustion/assertion. O contador não identifica PCs/opcodes EFU, portanto o smoke não prova execução específica de ESQRT/ESIN/ERCPR. Não é benchmark FPS nem validação visual completa.

### 2026-10-05 — Blocos sequenciais VU1

Generalizado o emissor de blocos sequenciais de VU0 para VU1. Cada entrada cobre no máximo8 pares consecutivos e para em branch, controle ou halt; bundle dispatch seleciona por hash e unidade. Runtime VU1 agora tenta `executeBlock` antes do fallback por par. O bloco chama o mesmo `processPair` a cada issue, preservando decode guard, hazards, pipeline, commits, branch delay, budget e resume. Isso ainda não remove o scheduler por par nem é alegação de FPS; mudança prioriza consolidar corpos convertidos sob dispatch estático e habilitar etapa futura de block scheduling.

Teste `vu_block_aot_test.py`: VU0 e VU1 fixtures passaram fatias1..8, branch taken/untaken, delay slot, store SQ antes do evento XGKICK e retomada. `vu_pair_aot_test.py --scheduler --real-helpers` passou 40.000 pares em fixture VU1 contra decoder/scheduler/commits reais. Bundle recomposto: 33 hashes VU1/11.387 pares-PC e3 VU0/293, todos com corpos e entrada de bloco candidata. Ninja/LTO passou. Smoke 60s `/tmp/black_vu1-block-test.log`: atividade AOT em14 hashes VU1 e3 VU0, fallback zero em todos contadores amostrados; sem guest-fault/reserved detail/exhaustion/assertion. Execução fecha automaticamente. Não é benchmark nem validação visual. Harness oracle corrigido: `vuEsin` agora é extraído mesmo em fixtures sem ESIN, pois referência lower inclui a operação.

Teste de chamada template estática no scheduler: fixtures e scheduler semântico passaram, mas ThinLTO do runner ficou mais de5min em ~2,5GiB sem concluir. Interrompido; removida a especialização genérica por corpo por risco de custo de compilação/tamanho. Bundle recompilado e runner voltou ao despacho por callback compacto, com Ninja sem trabalho pendente após build. Smoke45s `/tmp/black_vu1-static-call-test.log` auto-fechado: atividade AOT em13 hashes VU1 e3 VU0, fallback zero nos contadores amostrados, sem guest-fault/reserved detail/exhaustion/assertion. Não houve alteração persistente na semântica do runtime por essa tentativa.

### GIF/CLUT e cópia de cadeia — 2026-10-05

Captura correlacionada de Level_00 `black-level00-prefix-final.bin`/`black-level00-snapshot-trigger.vram.bin`: `trace_clut_upload.cpp` usa `GSMem::PixelBitAddress` para as 16 entradas CSM1/CT32 de CBP11647. Todas 16 batem exatamente com último upload CT32 nº 57751 (BP11616, BW1, 64×64), incluindo `df577064`, `7a59574d`, `33bb8d47`, `33d64589`. Confirma que esta CLUT foi carregada da imagem do fluxo GIF; não prova que os bytes produzidos pelo guest sejam os desejados. O mapa DMA de 4.691.712 bytes era de outra cadeia; o evento PATH3 final desta captura mede 4.792.368 bytes, portanto não atribuir o upload ao tag EE 0x01df9710 a partir dessa comparação. A instrumentação `PS2X_DMA_CHAIN_MAP_ALL` e `PS2X_DMA_REF_DUMP` permanece opcional.

Removida uma cópia de cadeias GIF grandes: `GifArbiter::submit` aceita `vector<uint8_t>&&`; `PS2Memory::processPendingTransfers` move `chainData` quando PATH3 não está mascarado, preservando caminho antigo para máscara/callback. Teste `gif_arbiter_order_test.cpp` passou 5040 permutações, reentrância e ownership do buffer de 4 MiB com ASan/UBSan; build runner passou. Smoke sem captura pesada entrou no Level_00 e encerrou automaticamente aos 120 s sem guest-fault/missing-target/reserved/exhaustion inspecionados. Janelas 60 updates: 80.8→93.8 = 4.62/s, 93.8→103.8 = 6.0/s, 103.8→117.9 = 4.26/s; variação grande, sem ganho consistente demonstrado. Corrupção visual permanece sem correção comprovada.

### Proveniência exata do upload da CLUT — continuação 2026-10-05

A opção diagnóstica `PS2X_DMA_CHAIN_MAP_ACTIVE` grava somente a última cadeia GIF de 4.792.368 bytes enquanto o trigger do snapshot GPU existe; o snapshot remove o trigger e congela o mapa. Nova captura `/tmp/black-active-prefix.bin` + `/tmp/black-active-chain-map.txt`: o último PATH3 é evento164888 de 4.792.368 bytes, FNV64 `915f62836f4606c0`, exatamente igual ao mapa DMA da mesma execução. TEX0 em evento164889, valor `2005afe55d40ad60` (TBP11616, PSMT4 128×32, CBP11647, CSM1/CT32, CLD1). `audit_texture_uploads.py --trace-bp 11616` coloca o último CT32 64×64 em PATH3 evento164888 offsets4626592..4642960, transfer ordinal62343. `trace_dma_source.py` mostra tag REF em 0x01df9710, QWC1462, fonte EE 0x007aead0 para o primeiro byte de imagem. Os16KiB da imagem batem byte-a-byte com o dump EE da execução anterior (SHA256 `9d263df33985f3084028948070e836fa028f8c15704f35c5392f24ecac1c1834`) e com a ISO original no offset2156104400, extraído em `recomp/disc/LEVELS/LEVEL_00/UNIT_01.BIN` offset588496/0x8fad0. Isso demonstra que esse upload é dado estático original do asset, transportado sem alteração pela cadeia. A CLUT na VRAM e no cache GPU continua combinando com o upload; a causa da imagem corrompida não foi identificada. Investigar seleção de TEX0 e ordenação PATH2/PATH3, sem trocar dados do asset por hipótese. Captura com diagnóstico pesado não serve de benchmark. Runner terminou automaticamente.

### Cadeia VIF1 do Level_00 truncada — 2026-10-05

O mapa opcional `PS2X_VIF_TEX0_MAP` revelou uma cadeia VIF1 que atingia exatamente o teto anterior de 4.096 tags, com último tag `CALL` e milhares de endereços distintos; não era um loop simples. Aumentado o teto somente da VIF1 para 16.384 tags, mantendo GIF/VIF0 em 4.096, com aviso limitado caso o teto ainda seja alcançado. Build do runner passou. Execução de 130 s autoencerrada em `/tmp/black-vif-expanded.log` alcançou Level_00; mapa final `/tmp/black-vif-tex0-expanded.txt` tem 6.064 tags, 3.494.352 bytes e último tag `END` (`0x01ded660`). Isto confirma que a cadeia capturada deixou de ser truncada; não confirma, por si só, a imagem visual correta. A taxa no gameplay com rastreamento ativo ficou em 60 updates/18,1 s = 3,31/s (t67,3→85,4), 60/18,1 = 3,31/s (85,4→103,5) e 60/12,4 = 4,84/s (103,5→115,9). É ainda lenta e não é uma comparação A/B limpa. Próximo: medir sem rastreamento e verificar quadro visual; depois identificar tempo gasto em VIF/VU/GS sem reintroduzir o truncamento.

### Perfil sem rastreamento e flags VU1 — continuação 2026-10-05

Execução com backend GPU e roteiro PAD, sem mapa DMA, `/tmp/black-vif-no-trace-gameplay.log`: Level_00 em t66,4/update1861; janelas seguintes de 60 updates duraram 19,1 s, 17,8 s, 12,2 s e 18,2 s. A lentidão permanece sem o rastreamento. Amostragem de 5 s em `/tmp/black-vif-no-trace-sample.txt`: GameThread 2888 amostras; 1984 estavam em `processVIF1Data` e 1626 em `VU1Interpreter::run` sob a chamada MSCNT, enquanto a espera de `ParallelGS::vsync` neste ramo foi muito menor. Amostras de pilha indicam foco na VU1, não são percentuais precisos de CPU. A primeira tentativa sem roteiro PAD ficou no menu, foi encerrada e descartada da medição.

`calculateFmacProductSticky` agora usa o sinal do produto float quando seu expoente está seguramente afastado das bordas de underflow/overflow; zero, subnormais, valores extremos, infinito e NaN continuam no cálculo exato anterior. `vu_product_sticky_fast_test.cpp` passou matriz de bordas e 1.000.000 de pares aleatórios com ASan/UBSan, comparando flags e bits do resultado com a referência. Build ThinLTO passou. Smoke integrado `/tmp/black-vu-sticky-fast.log` entrou Level_00, autoencerrou, sem guest-fault/missing-target/reserved/exhaustion/tag-limit nos logs. Cinco janelas de 60 updates: 18,3 s, 18,5 s, 15,0 s, 18,8 s e 18,6 s (3,2–4,0 updates/s). O roteiro entrou na gameplay em updates diferentes do baseline, portanto não é A/B alinhado e nenhum ganho de FPS está demonstrado. Perfil novo `/tmp/black-vu-sticky-fast-sample.txt` segue concentrado em VU1. Próximo investigar custo do scheduler/flags com comparação alinhada e conferir visual da cadeia VIF1 completa.

### Prazo do próximo commit VU — continuação 2026-10-05

`VU1Interpreter` agora mantém `m_nextCommitCycle`: as filas de flags, Q/P, store, VF/VI/ACC atualizam o prazo ao inserir resultados; `commitReadyPipelines` o recalcula após visitar as entradas prontas e ignora ciclos anteriores ao próximo prazo. `resetScheduler` invalida o cache. A ordem dos commits e do `progressXgkick` não foi alterada. `vu_sparse_commit_test.py` comparou 150.000 operações aleatórias com implementação de varredura integral; `vu_pair_aot_test.py ... --scheduler` comparou 40.000 pares sequenciados (modos rápido/preciso), e `--xgkick` comparou 8.192 transferências PATH1/LSU. Todos passaram. Microcaso sintético com uma escrita muito futura: commit repetido 5,09 ms contra 63,80 ms da referência; não é FPS.

Build ThinLTO passou. Execução Level_00 `/tmp/black-vu-deadline.log` autoencerrada, sem guest-fault/missing-target/reserved/exhaustion/tag-limit. Janelas iniciais de 60 updates: 17,8 s, 17,6 s, 15,7 s, 14,4 s, 17,8 s. No intervalo guest-time 34,4→40,4 s, a execução nova levou 47,9 s, contra 51,8 s de `/tmp/black-vu-sticky-fast.log` (mesmo roteiro/backend, entrada no nível em momento diferente). Diferença observada ≈8% nesse recorte, sem repetição A/B ou confirmação visual; não declarar ganho geral. A lentidão severa persiste (~3–4 updates/s). Próximo medir repetibilidade por guest-time/estado e conferir textura/cena visual; ainda falta converter/simplificar mais trabalho de VU1, não reintroduzir truncamento VIF1.

Repetição `/tmp/black-vu-deadline-repeat.log` também entrou Level_00 e autoencerrou sem esses erros. Quatro janelas de 60 updates após a entrada ficaram em 17,6/17,6/17,7/17,7 s; a quinta em 12,1 s. A rota automática entrou no nível em outro guest-time, sem trecho equivalente do baseline para comparação pareada. O resultado confirma estabilidade da nova lógica, mas não estabelece melhoria end-to-end. O próximo benchmark deve travar um estado/trecho comparável e alternar cache ligado/desligado no mesmo binário ou usar capturas determinísticas; a amostragem anterior indica VU1 como principal custo.

### Catálogo VU fora do caminho normal — continuação 2026-10-05

O perfil anterior atribuía 55 amostras totais ao `catalogVuPair`, mesmo sem `PS2X_VU_CATALOG`. `run` agora só chama o catálogo de pares quando essa variável está presente. O caminho AOT também passa `aotUnit`, corrigindo o registro VU1 que antes usava unidade0; a execução normal sem catálogo não altera o programa VU. Build ThinLTO passou. Smoke GPU `/tmp/black-vu-catalog-guard.log` entrou Level_00 e autoencerrou, sem guest-fault/missing-target/reserved/exhaustion/tag-limit. Perfil de 5 s `/tmp/black-vu-catalog-guard-sample.txt` não contém `catalogVuPair` e ainda mostra `VU1Interpreter::run`/`commitReadyPipelines` como custos relevantes. Janelas de 60 updates: 18,8 s (inclui sampling), 17,3 s, 12,1 s, 17,5 s. Sem ganho de gameplay reproduzível demonstrado: cenas e entrada no nível variam, e o sample perturba a primeira janela. A remoção da chamada diagnóstica é verificável; a lentidão principal permanece.

### Localização da corrupção visual — continuação 2026-10-05

Adicionado `ps2recomp/diagnostics/decode_gs_frame.cpp` para ler uma captura bruta de 4 MiB da VRAM do GS com o layout de memória do runtime e decodificar o framebuffer indicado pelo registro FRAME. Na captura `level00-cached-clut-snapshot.vram.bin`, FRAME `00000000000a0046` (FBP70, FBW10, PSMCT32), a imagem 640×448 `level00-gpu-vram-decoded.png` já contém as faixas horizontais e a cena quebrada observadas na janela. Portanto o defeito aparece antes da apresentação/cópia final para a janela; a captura isolada ainda não separa rasterização GPU de comandos GIF/VU incorretos.

Execução comparativa de 150 s com backend CPU (`/tmp/black-cpu-texture-compare.log`) entrou Level_00 em t59,6/g44,4/update1321 e terminou automaticamente. `level00-cpu-compare-latest.png` também mostra mundo com cores e blocos corrompidos, embora a assinatura visual difira da GPU. Isto reduz a chance de um defeito exclusivo do paraLLEl-GS, mas não prova que os dois backends estejam corretos. As imagens estão em `recomp/diagnostics/` (ignoradas pelo Git). Nenhum runner foi deixado aberto.

Próxima investigação: selecionar um triângulo visivelmente corrompido e correlacionar, em uma única captura temporal, bytes de textura/paleta na VRAM, TEX0/TEX1/CLAMP e vértices ST/Q/XYZ do GIF antes do rasterizador. Comparar a interpretação desses mesmos comandos com uma referência GS independente; corrigir o primeiro estágio que divergir. Não mudar CBP, swizzle, UV ou cores por tentativa visual: o snapshot TEX0 anterior já confirmou que cache de CLUT e VRAM concordavam naquele ponto, sem provar que os bytes ou comandos de origem fossem os esperados.

### Vértices correlacionados ao TEX0 indexado — continuação 2026-10-05

`trace_gif_tex0_vertices.py` decodifica os eventos GIF brutos, registra ST/Q/XY/ADC após um TEX0 escolhido e evita interpretar vértices como triângulos visíveis. Ele segue os campos do `writeRegisterPacked` real (XY do `lo` em bits 0..15/32..47; Q do `hi` em bits 0..31 de packed ST; ADC de `hi[47]`). Um parser preliminar que tratava RGBAQ packed como fonte de Q foi descartado por produzir Q falsamente subnormal.

Na captura V4, TEX0 `0x2005afe55d40ad60` aparece 33 vezes, com 2.656 vértices observados após suas gravações antes de outra textura do mesmo contexto. Resultado limitado em `recomp/diagnostics/level00-tex0-vertices-v4.json`: as ocorrências 77112 e 79654 têm sequência inicial idêntica de XY/ST/Q, inclusive S/Q≈−4,98→3,02 e T/Q≈0,725→5,355 nos quatro primeiros vértices; ADC=0 nesses quatro. Essas coordenadas podem ser legítimas com wrap/clamp e XYOFFSET; ainda não são diagnóstico de defeito. Evento 82196 é o gatilho final e não contém vértice posterior. Próximo: identificar draw que cobre pixel defeituoso e comparar estado/resultado do mesmo pacote com referência GS; o TEX0 isolado não basta para concluir qual estágio falha.

O rastreador agora também aplica PRMODE/PRMODECONT para escolher contexto, XYOFFSET e SCISSOR. Nas ocorrências 77112/79654, os quatro primeiros vértices ficam em coordenadas de tela (225,625;157,875), (225,625;171,125), (250,9375;162,4375), (250,9375;174,8125), dentro do recorte [0..639]×[0..447]. Todos os 2.656 vértices vinculados ao TEX0 passaram no teste de scissor por vértice; isto confirma geometria na região visível, mas não prova que cada vértice gerou pixels (ADC, profundidade, testes e sobreposição ainda importam). Fixture sintética de TEX0 packed + ST/Q + XYZF2/ADC passou. A comparação de um pixel e do draw que o escreveu continua pendente.

### Captura pareada da corrupção em gameplay — continuação 2026-10-05

O framebuffer bruto da captura correlacionada V4 era predominantemente céu, sem a corrupção central da gameplay; o triângulo/pixel analisado ali não podia provar a causa da parede defeituosa. Adicionado filtro opt-in `PS2X_PARALLEL_SNAPSHOT_TEX0` ao módulo GPU, compilado com Ninja, para disparar somente no TEX0 completo indicado. A primeira tentativa iniciou o fluxo cedo demais e encheu o limite de 512 MiB antes do snapshot (`capture_current_event=false`); a segunda terminou antes do readback selecionado. Nenhuma foi usada como evidência correlacionada.

A terceira execução iniciou a gravação no readback 2653 e disparou no TEX0 `0x200763e5dd40bae0` no readback 3029. `level00-corrupt-exact-trigger.registers.json` confirma `capture_current_event=true`, 98.082 eventos, 193.687.944 bytes e prefixo de 48 bytes no último pacote. `cut_stream_at_snapshot.py` confirmou o corte exato, salvo em `level00-corrupt-exact-prefix.bin`; sem mensagem de limite de stream. VRAM bruta do mesmo evento decodificada em `level00-corrupt-exact-frame.png`: cena de gameplay com parede direita intensamente magenta/verde e geometria/texturas quebradas. Runner encerrado, nenhum jogo aberto.

O TEX0 disparador é o último evento da captura, portanto ainda não há vértices posteriores a esse TEX0 no fluxo. Para rastrear a parede já desenhada, `trace_gif_tex0_vertices.py` aceita `all` e um pixel; resultado `level00-corrupt-wall-pixel-500-200.json`. Seis triângulos strip com ADC de kick ativo cobrem geometricamente (500,200) antes do snapshot. Dois pares têm a mesma geometria e TEX0s diferentes para contextos 0/1, por exemplo no evento 97079 `0x2006ffe5dd40b7c0` e `0x2006f406213136a0`; outro par no 97302 usa `0x20067be5dd40b3a0` e `0x20066405dd30b2e0`. Os vértices do último par vão de x≈−1611 a x≈2312 após XYOFFSET; isto pode ser clipping de triângulo PS2 e não prova erro VU. O teste é apenas geométrico: não resolve profundidade, blending, máscara, escrita final nem textura/paleta no instante de cada desenho. Próximo registrar o último draw que efetivamente escreve um pixel anômalo e comparar sua textura/CLUT e ordem com referência GS/PCSX2 ou replay controlado, antes de alterar renderer/VU.

### Colisão temporal das CLUTs da parede — continuação 2026-10-05

Rastreio das transferências até o evento PATH1 97302 encontrou causa concreta para as paletas ruidosas no estado observado: as 16 entradas da CLUT CBP13088 foram sobrescritas pelo upload CT32 nº37384 (BP13024, BW1, destino x0/y64, 32×32) depois do upload cinza da paleta BP13088 e antes do TEX0 PATH1. As 16 entradas da CLUT CBP13279 foram sobrescritas pelo upload CT32 nº37401 (BP13248, 64×32). Todas as 32 entradas reconstruídas por `PixelBitAddress` batem com a VRAM da captura; nenhum bug de readback é necessário para explicar esses valores. Os uploads estão dentro do mesmo pacote PATH3 de 4.792.368 bytes no evento96780, que chega inteiro ao GS antes dos TEX0 PATH1. Isto sugere fortemente erro de escalonamento DMA/GIF/VIF, mas ainda exige comparar ordem temporal com referência PS2 antes de declarar solução. O runtime processa imediatamente a cadeia GIF achatada; o árbitro só atua entre pacotes enfileirados, não entre GIFtags internos. Detalhes/reprodução e critérios de correção em `ps2recomp/diagnostics/CLUT_OVERLAP_LEVEL00.md`. `trace_clut_upload.cpp` agora aceita CBP variável. Não houve correção visual nesta etapa; não alterar paleta/swizzle por heurística.

### Confirmação da CLUT carregada pela GPU — continuação 2026-10-06

Captura direta em `TEX0=0x20067be5dd40b3a0` (readback 2808, contexto 0, CLUT instance 298) confirmou que as 16 entradas no cache GPU coincidem com a decodificação atual da VRAM. A textura PSMT4 128×128 decodificada no mesmo snapshot tem padrão ruidoso, mostrando que os valores ruins antecedem a saída do framebuffer. Um trace adicional de DMA/GIF em gameplay não observou drains mistos PATH1/PATH3; as escritas dos canais chegam serialmente, e `processPendingTransfers()` esvazia cada cadeia no início do canal. Adiar todo o PATH3 antes do processamento deixou o guest preso esperando a conclusão DMA e foi removido. A correção real exige avanço cooperativo dos canais/FIFO e IRQs, validado primeiro com a ordem DMA e depois com a mesma captura CLUT; o documento de diagnóstico descreve o próximo passo e por que cortes fixos/reordenação total não são seguros.

### Protótipo incremental por EOP — 2026-10-06

`PS2X_GIF_INCREMENTAL=1` compila e chega à sala do Level 00, sem falha de runtime observada. A captura `/tmp/black-gif-incremental-current.png` ainda mostra texturas severamente corrompidas. A execução sem a variável, com o mesmo roteiro, também chegou à mesma sala e mostrou corrupção semelhante em `/tmp/black-gif-baseline-current.png`. O protótipo não corrigiu a imagem e segue opt-in; não ativar no launcher. A sessão de referência encerrou automaticamente. Relatório em `ps2recomp/diagnostics/CLUT_OVERLAP_LEVEL00.md`.

A primeira implementação processa a cadeia VIF1 inteira enquanto PATH3 está pendente, então não preserva os instantes em que MSCAL/XGKICK tornam PATH1 disponível. Próxima etapa: instrumentar estado STR/TADR/QWC de ambos os canais e o serviço em cada EOP na mesma cadeia, validar a prontidão do caminho no guest e comparar o ordenamento com PCSX2 antes de ajustar o escalonador. Apenas fatiar PATH3 em EOP não é suficiente.

### Prova SDL_GPU com upload capturado de Level_00 — 2026-10-06

O alvo diagnóstico opcional `ps2x_sdl_gpu_gs_probe` liga SDL_GPU ao `GSCpuBackend` real. O shader Metal calcula o swizzle CT32 em GPU. Reproduzido o upload do bundle correlacionado V4 em BP11616/BW2, 128×128, 65.536 bytes; todos os 16.384 texels da saída GPU conferiram bit a bit com o payload capturado. Swapchain Metal apresentou 120 frames. Build e execução passaram. Isso valida apenas esse upload e esse formato, não a sequência de draws/CLUT inteira; o backend ainda não é usado pela gameplay e a corrupção visual permanece.

### Prova SDL_GPU de textura indexada capturada — 2026-10-06

`ps2x_sdl_gpu_psmt4_probe` executa no driver Metal e lê `level00-correlated-snapshot-v4.vram.bin` com TEX0 `0x2005afe55d40ad60`, usando a CLUT capturada em `level00-correlated-snapshot-v4.clut.bin`. Resultado: PASS para os 4.096 pixels de PSMT4 128×32; GPU compute e `GSMem::ReadTexture` + CLUT do cache produzem o mesmo CT32, e a CLUT capturada tem zero entradas divergentes da VRAM. Isso fecha a validação de leitura indexada para esse snapshot, sem provar renderização de primitivas nem explicar a corrupção temporal de outros draws. A imagem de saída fica em `recomp/diagnostics/sdl-gpu-captured-psmt4.ppm` (arquivo diagnóstico ignorado). Ainda é um probe isolado: a gameplay usa o backend existente.

### Corrupção de texturas do Level_00 resolvida: gating do PATH3 por MSKPATH3 — 2026-10-07

Causa: o Black controla o PATH3 por `MSKPATH3` no stream VIF1. A cadeia GIF de 4,79 MB (14 EOPs, ~1.580 uploads) chegava com PATH3 mascarado, entrava inteira na fila mascarada e o primeiro `MSKPATH3(0)` despejava tudo; os uploads seguintes reusavam o pool e sobrescreviam CLUTs antes dos draws PATH1. Correção em `ps2_memory.cpp`/`ps2_vif1_interpreter.cpp`: com PATH3 mascarado a cadeia é dividida por EOP ao entrar na fila e cada transição 1→0 de `MSKPATH3` libera um único pacote. Reset VIF1 e novo PATH3 sem máscara continuam despejando tudo. **Ligado por padrão**; `PS2X_PATH3_EOP_GATE=0` restaura o comportamento antigo.

Validação (`bash ps2recomp/diagnostics/TestarPath3Gate.command [default|gate|baseline]`, SDL_GPU, mesmo roteiro de pad; resultados em `recomp/diagnostics/path3-gate/`, runs anteriores em `run1`…`run4`):

- Verificador de CLUT (`clut_last_writer.py`): gate/default 65.388–65.943 TEX0 indexados, 100% lidos de upload de paleta e 0 sobrescritos, em 5 runs; baseline 12.636–17.196 de paleta e 48.853–52.125 sobrescritos, em 3 runs. Máximo de uploads sem TEX0: 197–202 contra 1.578–1.585 (PCSX2: 151).
- `[path3-gate]`: todas as cadeias chegam mascaradas no kick (0 não mascaradas); com o gate `flush-all=0`; a fila oscila entre 2 e 8 e `max-fifo=14` durante 150 s de fase, sem crescer.
- Visual: o usuário confirmou no jogo as texturas da sala corretas; o baseline mostra paredes em faixas magenta/verde (`run2/baseline-frame2.png`).
- Sem guest-fault, missing-target ou exhaustion.
- Updates/s na fase iguais nos dois modos: 0,77–0,82 com gate e 0,79 sem (medido com `BLACK_DUMP_EVERY=5` e captura GIF ligada). O gate não custa nem resolve desempenho; ~0,8 upd/s é o próximo problema.

Patch `0001-black-runtime-fixes.patch` regenerado com o comando do SKILL §0 (67 arquivos; inclui também o backend SDL_GPU que estava fora do 0001 desde 2026-10-05). `PS2Recomp-local-changes.patch`, citado no README, **não** foi regenerado e não contém o gate.

Limites: o roteiro de pad chega ao Level_00 em updates diferentes entre runs (826–1861) e às vezes a janela termina ainda no texto "4 DAYS EARLIER"; o tempo de guest avança ~1 s a cada 30 updates. O `PS2X_GIF_INCREMENTAL` segue ligado por padrão e não atua aqui porque as cadeias chegam mascaradas.

### Desempenho no Level_00: onde o tempo vai — 2026-10-07

Medições com SDL_GPU, gate do PATH3 ligado, mesmo roteiro de pad, sala inicial do Level_00 (arquivos em `recomp/diagnostics/perf-level00/`, `*-sample.txt` são perfis `sample` de 5–10 s):

- **Sem hooks de debug (`BLACK_FPS=1`): ~2,0 updates/s** com uma travada de ~2 s a cada ~10 s (média ~1,6). Com `BLACK_DEBUG=1` cai para ~0,8 porque `traceGs` → `ReadVram` → `GSSDLGpuRaster::sync()` espera a GPU inteira a cada dump; os números de upd/s medidos com debug não valem como desempenho.
- A fase submete **~82 mil triângulos por update**; 98% viravam um compute pass individual (~165 mil passes/s). 96% das recusas de lote eram "texture": o draw tem registradores MIPTBP (metade mip amostrável, metade só nível base com área < 4096 px). Áreas: a maioria ≤ 64 px.
- A travada periódica é o wrap da arena de upload de 128 MiB (`stageUploadDataParts`), que submete e espera a fence: a GPU acumula segundos de atraso.
- **Teto sem nenhum raster** (draws descartados em `submit`, diagnóstico temporário já removido): **3,7 updates/s**. Nesse perfil ~80% da thread do jogo é o interpretador da VU1 (`run` 29%, `commitReadyPipelines` 12%, `execUpper` 10%, `execLower` 7%, flags/sticky FMAC ~14%). Ou seja: raster custa cerca de metade do frame e a VU1 quase toda a outra metade; nenhum dos dois sozinho chega perto de 30 upd/s.

Experimento `PS2X_GS_WIDE_BATCH=1` (opt-in, **desligado por padrão**): inclui as faixas de VRAM dos níveis de mip na checagem de conflito, deixa draws com MIPTBP entrarem em lote e aumenta o lote ordenado para até 128 draws (`PS2X_GS_ORDERED_MAX_DRAWS`) com orçamento de testes de bounds (`PS2X_GS_ORDERED_WORK_BUDGET`, padrão 2^20). Resultado: passes caem 16× (16,5 draws/pass; o limite passa a ser o `finishPass` de cada `loadClut`, ~1 a cada 20 draws), as travadas somem, mas a média fica igual (1,5–1,7 upd/s) — a GPU continua sendo o limite, então o custo dominante não é o número de passes de raster. Orçamentos menores são piores (32768 → 1,1–1,3; 0 → 0,3–1,3): um lote pequeno (blit dos parâmetros + compute) custa mais que passes individuais. **Regressão**: com o lote largo as texturas do mundo corrompem depois de ~190 updates na fase (após o flash em upd≈2021; `run6/default-frame1.png`), enquanto a política antiga fica limpa no mesmo ponto (`legacy-late.png`). Causa não identificada. O smoke `ps2x_sdl_gpu_backend_smoke` passa 25/25 nos dois modos (as duas asserções de mip agora checam a imagem e não a contagem de passes).

Também: o report `[path3-gate]` só sai com `PS2X_PATH3_GATE_STATS` (estava saindo sempre depois de o gate virar padrão). Patch 0001 regenerado.

Próximos passos candidatos, em ordem de custo/benefício estimado: (1) achar o hazard do lote largo e tirar o `finishPass` por CLUT e o blit de parâmetros por lote (meta: raster de ~0,3 s/frame para bem menos, limite 3,7 upd/s); (2) VU1: o AOT atual mantém o loop/filas por par e por isso não ganha; ganho real exige blocos com pipeline/flags resolvidos estaticamente; (3) tirar VU1 e GS da thread do EE.

Bisect da regressão do lote largo: `PS2X_GS_WIDE_BATCH=1` com `PS2X_GS_ORDERED_MAX_DRAWS=16` e `PS2X_GS_ORDERED_WORK_BUDGET=0` (só o mip em lote, limites antigos) fica limpo no mesmo ponto tardio (`recomp/diagnostics/path3-gate/bisect-mip16-late.png`, uma execução). Portanto a corrupção vem dos lotes ordenados grandes (mais de 16 draws e/ou orçamento de trabalho), não das faixas de mip. Os kernels `rasterPixel`/`batchRasterPixel` e `sample`/`batchSample` do shader são textualmente equivalentes; falta verificar hazard de ordem/CLUT com lote grande ou aborto de command buffer longo pelo Metal.

### VU1 recompilada estaticamente — 2026-10-08

Regra do projeto (usuário, 2026-10-07): código que o console executa tem de ser recompilado, não interpretado. A VU1 passou a rodar microcódigo recompilado por padrão; `PS2X_VU_RECOMPILED=0` volta ao interpretador de referência. A VU0 (microprogramas) continua no interpretador de referência.

Como funciona:
- `ps2x_vu1_recompile` (`ps2recomp/diagnostics/vu1_recompile.cpp`) lê imagens de 16 KiB da memória de código da VU1 e gera `src/lib/vu/black_vu1_recompiled.inc` (local, não versionado: contém código do jogo). Cada imagem vira uma função com um `switch` no PC e fallthrough; cada par alcançável é uma chamada a `VU1Interpreter::stepPair()` com literais. `stepPair` e os corpos das instruções (`ps2_vu1_upper.inl`/`ps2_vu1_lower.inl`, extraídos dos antigos `.cpp`) são inline, então o compilador dobra a decodificação. Os fatos de cada par vêm de `VU1Interpreter::describeImage()`, o mesmo decodificador e a mesma análise do runtime.
- Semântica: a do modo FastVU do interpretador (mesmos stalls de FDIV/EFU/XGKICK, mesmas filas). Diferenças deliberadas: stores LSU são gravados na hora (equivalente ao commit no ciclo seguinte) e as flags MAC/status de um FMAC só são calculadas quando a análise estática (`analyzeFastPairs`) mostra que algum FMAND/FMEQ/FMOR, um fim de programa ou um salto indireto pode observá-las. Se a imagem tiver qualquer FSEQ/FSSET/FSAND/FSOR tudo fica exato; sem isso os bits sticky Z/S/U/O do status da VU1 não são mantidos (`PS2X_VU_EXACT_FLAGS=1` força exato). Nas 46 imagens: 16.783 escritores de flags, 1.502 ainda calculam.
- Imagem desconhecida: é interpretada pelo mesmo `stepPair` e avisada em `[vu-recomp]`; com `PS2X_VU_RECOMP_CAPTURE=<dir>` a imagem é salva. `ps2recomp/build_vu_recompiled.sh` regenera a partir de `vu-coverage-level00/`, `vu-trace/images/` e `vu-captured/`, valida e recompila o runner.

Validação:
- `ps2x_vu1_trace_replay recomp/diagnostics/vu-trace/level00.bin --engine recompiled`: 2.327 execuções reais (`PS2X_VU_TRACE`, execute e resume com pipelines ociosos), 3.171.642 pares, 1.990 pacotes PATH1 — 0 divergências de registradores, memória e pacotes contra a referência (status comparado com máscara 0xC3F), e a referência bate com o resultado gravado no jogo.
- `PS2X_VU_FAST_VERIFY=<arquivo>` (+ `PS2X_VU_VERIFY_RECOMPILED=1`): execução em sombra dentro do jogo; 262.144 programas sem divergência no recompilado e 786.432 no motor por pares, atravessando a troca de cena.
- Bancada: 9,1 ns/par contra 46,7 ns/par da referência. No jogo (Level_00, sem hooks de debug): 4,4–5,0 updates/s contra ~2,0; a VU caiu para ~15% da thread do jogo e o resto é GS/GPU.
- Frame da introdução da fase igual ao anterior (`recomp/diagnostics/path3-gate/recomp-intro.png`).

Pendências conhecidas:
- **Jogabilidade corrompida em todas as configurações** (inclusive antes desta mudança): os frames "limpos" validados em 2026-10-07 eram da câmera de introdução. Quando o `select` do roteiro inicia a jogabilidade, as texturas do mundo corrompem. Fatos medidos: o dump do PCSX2 tem ~700 uploads por frame intercalados com os draws; o recomp faz ~40 por frame, todos antes dos draws; por frame há uma cadeia VIF1 grande (≈5,7 M ciclos de VU, 15 janelas `MSKPATH3(0) NOP MSKPATH3(1)`, 13 vazias) e uma pequena (4 janelas), com cadeias GIF de 3 pacotes (3896/1798/10485 qw) e 2 pacotes (2/1 qw) disparadas depois de cada uma; o jogo lê GIF_STAT 3×/frame (valores 4, 6 e FQC 0x10/0x03), D1_CHCR e D2_CHCR 1×. Hipóteses abertas: o VIF1 é concluído no próprio kick (no hardware a cadeia GIF disparada 5 mil ciclos depois passaria nas janelas do mesmo frame) e a regra "um pacote por janela" (no PCSX2 a máscara só impede o próximo pacote de começar, e um pacote curto deixa o seguinte passar). `PS2X_DMA_COOPERATIVE=1` agora avança o VIF1 em proporção aos ciclos de EE, mas não corrigiu.
- VU0 (microprogramas) e IOP continuam interpretados.
- `PS2X_VU_FAST=1` seleciona o mesmo laço por pares sem o código gerado (útil para isolar o gerador).
- Os alvos `ps2x_vu1_trace_replay`/`ps2x_vu1_recompile` estão em `ps2xRuntime/CMakeLists.txt`, que não entra no patch 0001.

### VU0 também recompilada; IOP medido — 2026-10-08

- Os microprogramas da VU0 usam o mesmo recompilador e o mesmo padrão (`PS2X_VU_RECOMPILED=0` desliga os dois). Na VU0 todas as flags ficam exatas (o EE as lê por COP2). `PS2X_VU0_TRACE=<arquivo>` grava execuções da VU0; `recomp/diagnostics/vu-trace/vu0-session.bin` tem 8.000 execuções (395.055 pares, 3 imagens): 0 divergências contra a referência e contra o resultado gravado no jogo. Bancada: 28 ns/par contra 52 ns/par (execuções curtas, ~49 pares, dominadas pelo custo fixo por chamada).
- `build_vu_recompiled.sh` agora cobre `vu0-*.bin` e `vu1-*.bin`, exporta as imagens de todos os traces em `recomp/diagnostics/vu-trace/*.bin` e força a recompilação de `ps2_vu1_core.cpp` (o `__has_include` do arquivo gerado não é visto pelo sistema de build; sem isso o runner ficava sem o código gerado).
- No jogo, com VU0+VU1 recompiladas: 4,1–5,1 updates/s no Level_00, nenhuma imagem desconhecida no log, frame da introdução inalterado (`recomp/diagnostics/path3-gate/recomp-vu01-intro.png`).
- Pendente no gerador: imagens capturadas sem cobertura (`vu-captured/`) estão saindo com 0 pares e são interpretadas sem aviso.
- IOP: no perfil da fase, `IopEmulator::runEeCycles` é 0,5% da thread do jogo e `IopCpuCore::executeInstruction` 0,3% (o IOP fica quase todo ocioso). Os módulos são 9 IRX do disco (`recomp/disc/IOP`: DBCMAN, DS2O, DSPROUTE, GTFSCDVD, LIBSD, MC2_D, RWA, SIO2D, SIO2MAN) carregados e relocados em tempo de execução. Recompilar é viável pela mesma técnica (capturar o texto já relocado e gerar C++ com cada instrução fixada como constante), mas não muda updates/s; ainda não foi iniciado.
- Para comparação, no mesmo perfil `GS::processGIFPacket` é 68,6% da thread do jogo e o código recompilado da VU 16,2%.

### Backend SDL_GPU: passes por frame e contrapressão — 2026-10-08

Medido no Level_00 (câmera de introdução), sem hooks de debug, VU0/VU1 recompiladas. Estado anterior: 4,4–5,0 updates/s com travadas e 53% da thread do jogo em `GSSDLGpuRaster::submit` (um compute pass por triângulo).

- A corrupção "do lote largo" de 2026-10-07 era a corrupção da jogabilidade (existe em todas as configurações, inclusive no raster de CPU: `recomp/diagnostics/path3-gate/cpu-gameplay.png`). O lote largo voltou a ser o padrão (`PS2X_GS_WIDE_BATCH=0` desliga).
- Um blit para um buffer seguido de um compute pass que o lê custa à GPU dezenas de vezes mais que um pass com dados em uniform. Por isso: lotes ordenados de até 64 draws vão como uniform (`rasterOrderedUniform`: cabeçalho + tabela compacta de retângulos + `Params`), lotes de upload de imagem ≤ 32 KB vão como uniform (`imageUploadUniform`), e o miss do cache de CLUT preenche o slot com um segundo dispatch em vez de um blit.
- Hits do cache de CLUT não copiam mais para o slot 0 na hora: o draw cujo TEX0 coincide com o último load amostra o slot do cache (`p[46]`), e os loads pendentes só são reaplicados em ordem (`materializeClut`) quando algo precisa do slot 0. Trocar de paleta deixou de quebrar o lote.
- Scanout: o host apresenta muito mais vezes do que o jogo fecha frames; se nada escreveu na faixa de VRAM exibida desde o último scanout da mesma fonte, reaproveita a imagem (sem trabalho de GPU).
- Contrapressão: com os três slots de scanout em voo, `submit` espera o mais antigo. Sem isso a CPU produzia 6–7 updates/s e a fila da GPU crescia até uma travada de 5–9 s.
- Teste de profundidade antecipado nos kernels (`rasterPixel`/`batchRasterPixel`) quando o teste de alfa não pode falhar: 4,1 → 4,8 updates/s.
- Sensibilidade medida com contrapressão (taxa = limite da GPU): 10 draws/pass → 2,6; 13 → 3,0; 35 → 4,1; orçamento de testes 16× maior não muda nada; kernel ordenado sem fazer nada → 6,7 (limite da CPU). Ou seja, o custo dominante da GPU é o sombreamento por pixel coberto, não o número de passes nem os testes de retângulo.
- Resultado: **4,7–5,0 updates/s estáveis, sem travadas**, contra 4,4–5,0 com travadas de vários segundos. O smoke `ps2x_sdl_gpu_backend_smoke` passa 25/25 e o frame da introdução está igual (`gs-batch-intro.png`).

Próximo teto: a fase tem overdraw alto (estimativa pelas áreas dos retângulos: dezenas de milhões de pixel-draws por update) e cada pixel-draw no kernel faz 6–7 leituras aleatórias de VRAM/LUT/CLUT. Chegar perto de 30 updates/s pede o rasterizador nativo da GPU com cache de texturas, não mais ajuste do kernel de compute.

### Corrupção na jogabilidade do Level_00: o que está estabelecido — 2026-10-08

Sintoma: depois do fim da introdução (upd≈2011, flash) a imagem **alterna** entre frames corretos e frames com as texturas do mundo em lixo, na mesma câmera (`recomp/diagnostics/path3-gate/four-frames.png`: 2161 lixo, 2171/2181 corretos, 2191 lixo). A média de brilho do dump distingue: ~52/49 correto, ~40 lixo. Vale para VU de referência ou recompilada, raster de CPU ou GPU, gate ligado ou desligado, DMA síncrono ou cooperativo.

Descartado, com evidência:
- **Raster/GPU**: o raster de CPU mostra o mesmo (`cpu-gameplay.png`), e uma simulação independente da VRAM em Python, só com os uploads capturados, decodifica o mesmo lixo (`stream-decode.png`).
- **VU**: execução em sombra sem divergência; `PS2X_VU_RECOMPILED=0` dá a mesma série.
- **Swizzle**: tabelas de bloco e de coluna (CT32/CT16/T8/T4) conferidas com as do hardware.
- **Ordem dos kicks**: o gerenciador de DMA do jogo (`func_002B2638`, fila com entradas 0x21 = par GIF+VIF1, 0x01/0x02, 0x40/0x41 = flip, 0x42 = decremento de contador, 0x4F = callback, 0x7F = salto) dispara a cadeia GIF de texturas imediatamente antes da cadeia VIF1 que a usa: mascara o PATH3 escrevendo `MSKPATH3` na FIFO do VIF1, espera `GIF_STAT.M3P`, põe `GIF_MODE=IMT`, dá o kick do GIF, espera `FQC != 0` e só então dá o kick do VIF1; antes de cada par espera D1/D2 sem STR, `VIF1_STAT & 0x1F000003 == 0`, VU1 parada e `GIF_STAT & 0xC00 == 0`. Adiar o VIF1 (`PS2X_VIF1_DEFER`, experimento removido) mostrou que o jogo nunca dá o kick do GIF "durante" o VIF1 anterior. `func_002B2BA8` é o tratador de VSync (limitador de frames, flip e kick da cadeia de 56 qw).
- **Conclusão do canal GIF no esvaziamento da fila mascarada** (`PS2X_GIF_COMPLETE_ON_DRAIN=1`, opt-in): fiel ao hardware, mas não altera o stream.
- **Tags DMA**: nos frames ruins a cadeia GIF do par grande é uma única tag `end` com 3 qw (pacotes de 2 e 1 qw); nos bons tem ~40 `ref`/`cnt`/`call`/`ret`. O percurso das tags está certo; o jogo é que monta a cadeia vazia.

O que os dados mostram (`PS2X_ORDER_TRACE=<gatilho>` imprime por kick de VIF1 as tags DMA, pacotes, janelas e conclusões):
- Por frame há dois pares: (A, VIF1 grande, ~2790 XGKICKs, 15 janelas `MSKPATH3(0) NOP MSKPATH3(1)`) e (B, VIF1 pequeno, ~125 XGKICKs, 4 janelas).
- Na introdução A tem 14 pacotes (~1500 uploads) e B 3; o alocador percorre o pool inteiro 11616..16351 em anel, alternando metades por lote.
- Na jogabilidade B continua com 3 pacotes (35 uploads, incluindo o atlas do HUD CT16 256×128 em 11616), mas os três lotes de B caem todos em 11616..12447 e se sobrescrevem; A fica vazia na maioria dos frames. Os draws do mundo usam TBP 11904..12416 (dentro dessa faixa: lixo) e 12544..14208 (fora: corretos).
- O dump do PCSX2 da mesma fase tem ~700 uploads por frame, em anel por 11616..16255, intercalados com os draws: lá a jogabilidade se comporta como a nossa introdução.

Hipótese de trabalho: no runtime o anel de texturas do jogo fica restrito a 11616..12447 depois da transição, como se o restante do pool continuasse "em uso". O candidato mais provável é a contabilidade que o jogo faz pelas entradas 0x42 da fila de DMA (decrementam um contador quando a fila chega nelas) e pelas interrupções de conclusão; falta localizar o alocador no código do jogo e ver qual contador não volta. Próximo passo: achar quem escreve o DBP dos BITBLTBUF de B (pacotes `cnt` de 2/4 qw) e vigiar as variáveis de limite do anel na transição (upd 2011..2031).

Ferramentas novas desta investigação: `PS2X_ORDER_TRACE`, `ARM_AFTER`/`KEEP_CAPTURE`/`UNSET`/`PAD` em `path3_gate_test.py`, e os arquivos `gameplay-transfers.bin(.seq)`/`intro-transfers.bin.seq` em `recomp/diagnostics/path3-gate/`.

### Corrupção na jogabilidade do Level_00 resolvida: estouro da lista de DMA do próprio jogo — 2026-10-08

Causa (bug latente do jogo, não do runtime). A hipótese do anel de texturas acima estava errada: o jogo monta os uploads do mundo em todos os frames; quem some com eles é a própria lista.

- Cada buffer de DMA do jogo (2 × 0x7F800 bytes, pré-alocados com 1 MiB em `func_002B4FB0`) guarda, de baixo para cima, a fila de entradas e a lista VIF1, e no topo a lista GIF dos uploads. No primeiro upload do frame o gerenciador de texturas (`D_00440280`, `func_00270B98`) reserva (62 + 512) × 24 qw ≈ 220 KiB para a lista GIF, então sobram ~290 KiB para a lista VIF1.
- O alocador da lista (`func_002B3D88`) pede `a1 + 4` qw livres: os 4 são o terminador que o flush (`func_002B2E20`) grava. Só que vários renderizadores escrevem mais do que reservam (`func_001CDD98` pede 3 qw e grava 8; medido: até 80 bytes além da reserva).
- Na jogabilidade a lista VIF1 do mundo enche o buffer em todo frame (o alocador falha, dá flush e troca de buffer no meio do frame). Quando o último a escrever foi um desses renderizadores, a lista termina a menos de 0x40 do topo e o terminador cai em cima dos primeiros qwords da lista GIF (`0x1d6de80`). A cadeia GIF do par passa a ser só `end` 3 qw (o próprio terminador da VIF1) e o frame inteiro é desenhado sem os uploads: lixo. Como o tamanho da lista varia alguns qwords de frame para frame, a imagem alternava.
- Num PS2 o mesmo código faria o mesmo com a mesma lista; lá a lista não deve chegar a encher o buffer nesse ponto (ver "Pendente").

Correção: `ps2recomp/overrides/black_dma_guard.cpp` soma 8 qw ao pedido de toda chamada a `func_002B3D88`, de modo que o alocador dá flush 8 qw mais cedo e o terminador nunca alcança a lista GIF. `BLACK_DMA_GUARD=0` desliga.

Validação (mesmo roteiro, dump a cada 7 updates para pegar as duas paridades, média de brilho do frame):
- sem a correção: `2038:28 2045:35 … 2101:43 2108:42 2115:42 2122:52 2129:52 2136:42 2143:42 2150:52 2157:42 2164:41`;
- com a correção, duas execuções: `2038:49 2045:50 … 2157:52 2164:50 2171:50`, nenhum frame abaixo de 49. Quatro dumps consecutivos em `recomp/diagnostics/path3-gate/guard-four.png`.

Como foi achado: `ps2recomp/overrides/black_texarena_trace.cpp` (diagnóstico, só com variável):
- `BLACK_TEXARENA_TRACE=<arquivo>` (arma quando `<arquivo>.trigger` existe; `BLACK_TEXARENA_EVENTS=n`): loga reset/flip/upload/lista GIF do gerenciador de texturas e alloc/grow/flush/swap/pump/qadd do gerenciador de DMA, com ponteiros das listas, entrada corrente da fila, D1/D2 e GIF_STAT/VIF1_STAT no pump;
- `BLACK_TEXARENA_WATCH=1` embrulha todas as funções e aponta quem deixa a lista VIF1 maior que a última reserva (`UNRESERVED-WRITERS`, `MAX-OVER`).
- No runtime: `PS2X_ORDER_TRACE` agora imprime as tags DMA da cadeia GIF (`cnt2 ref330 … end0`), `B`/`N` (KiB e tags da cadeia VIF1) e `D` (kilo-ciclos até a VIF1 concluir); `PS2X_RAM_DUMP=<arquivo>` grava a RAM quando `<arquivo>.trigger` aparece; `PS2X_DMA_STRICT=1` (com `PS2X_DMA_COOPERATIVE=1`) tira a fatia grátis do VIF1. Nenhum deles muda o padrão.

Também descartado nesta rodada, lendo o PCSX2 (`Gif_Unit.h`, `Gif.cpp`): a máscara do PATH3 só segura o pacote seguinte (estado IDLE/WAIT depois de um EOP), igual ao nosso gate; a granularidade mais fina que aparece no dump do PCSX2 (um upload, alguns draws, outro upload) vem do fatiamento por IMT ao longo do tempo, não de uma regra de máscara diferente.

Pendente:
- Por que a lista do mundo enche o buffer aqui: no dump do PCSX2 (outra câmera da mesma fase) há 2364 TEX0 e 143 texturas base por frame; no nosso spawn são ~4300 TEX0 e 318 (178 distintas, 101 das 105 do PCSX2 entre elas). Pode ser só a câmera, ou estamos desenhando mais do que o console (culling/LOD). Vale conferir porque também é custo de GPU.
- Os vidros das janelas no spawn mostram um mosaico de blocos (reflexo/ambiente); não foi comparado com o console.

### Renderer: paraLLEl-GS volta a ser o padrão do launcher — 2026-10-08

Com as duas correções de ordem/conteúdo no lugar (gate do PATH3 e guarda da lista de DMA), o backend paraLLEl-GS que tinha sido arquivado por "cores erradas" passou a desenhar o Level_00 certo: `recomp/diagnostics/path3-gate/parallel-four.png` (quatro dumps seguidos na jogabilidade). Os erros de antes eram do stream, não dele.

Medições no Level_00, sem hooks de debug, VU0/VU1 recompiladas, mesmo roteiro:

| Renderer | updates/s na introdução e na jogabilidade | Thread do jogo |
|---|---|---|
| SDL_GPU (kernels próprios) | 4,7–4,9 | 93% ocupada: 32% montando lotes (`TryMetal`/`submit`), ~23% VU, 9% uploads/CLUT |
| paraLLEl-GS (MoltenVK) | 8,8–9,1 (3,5 min estáveis, sem erros) | 22% esperando a GPU (`vkWaitSemaphores` em `flush_submit`), ~31% VU, 7% entregando GIF, 5% em mutex |

- Apresentação nativa (`PS2X_GS_NATIVE_PRESENT=1`) dá os mesmos 9,0: o readback do scanout não é o gargalo.
- Os vidros das janelas no spawn saem com o reflexo liso no paraLLEl-GS e em mosaico no SDL_GPU: o mosaico é defeito do nosso kernel (filtro/mip), não do jogo.
- `ps2recomp/JogarBlack.command` usa paraLLEl-GS quando `recomp/gpu/build/libblack-parallel-gs.so` e o MoltenVK existem; `BLACK_GS=sdlgpu` força o SDL_GPU. Sem o módulo, continua no SDL_GPU.
- `ps2recomp/gpu/parallel_gs_module.cpp`: as variáveis de diagnóstico passaram a ser lidas uma vez (`ENV_ONCE`); eram ~8 `getenv` por pacote GIF (2% em `__findenv_locked` mais boa parte dos 5% em mutex). **Ainda não compilado**: o clone do paraLLEl-GS usado no build de 06/10 estava em `/tmp` e foi parcialmente apagado pelo sistema; o módulo em uso é o binário antigo. `ps2recomp/gpu/build.sh` refaz o clone (commit fixado) em `recomp/gpu/parallel-gs` e recompila.

Para onde vai o tempo agora (frame de ~111 ms):
- VU1 ~34 ms. O código gerado ainda simula o pipeline em tempo de execução: `commitReadyPipelines` 4,7%, `calculateFmacProductSticky` 3,7%, `normalizeFmacResult` 2,1%, `updateFmacFlags` 1,4% — cerca de 12% do frame só em flags/commits. O próximo passo do gerador é resolver a latência das flags estaticamente por bloco (saber em geração qual instrução está visível em cada leitura) em vez de enfileirar em tempo de execução.
- Espera da GPU ~25 ms, dentro do flush do par pequeno (HUD) no `swap` do jogo.
- Tudo isso roda na thread do jogo. A lógica do jogo em si (EE recompilado) é ~10–20% do frame; tirar VIF1/VU1/GS da thread do jogo (a cadeia já é copiada no kick) é o que separa 9 de ~20 updates/s.

### paraLLEl-GS alimentado por uma thread própria — 2026-10-08

`GSParallelBackend` (`include/runtime/gs/gs_parallel_backend.h`) deixou de chamar o módulo na thread do jogo. Pacotes GIF, escritas de registrador, uploads, clears e flushes são anexados a um fluxo de bytes (segmentos de 256 KiB, no máximo 48 MiB em fila) e reproduzidos em ordem por uma thread do adaptador. Tudo o que lê estado da GPU esvazia o fluxo antes: `Present` (o flip que escolheu o frame vem depois dos draws dele), `ConsumeLocalToHostBytes`, `ReadVram`/`WriteVram`/`SnapshotVram`, `Sync(Finish|Reset)` e `Reset`. `PS2X_GS_PARALLEL_THREAD=0` volta à alimentação síncrona.

- Medido no Level_00 (introdução e jogabilidade): **9,0 → 14,0–14,4 updates/s**. A thread do jogo não espera mais a GPU (`__psynch_cvwait` 22% → 1%) nem o mutex disputado com o apresentador.
- Imagem igual à do modo síncrono: mesma série de brilho e `recomp/diagnostics/path3-gate/parallel-mt-four.png`.
- Perfil depois disso: VU1 é 51% da thread do jogo, sendo ~20% do frame só em flags/commits (`commitReadyPipelines` 7,2%, `calculateFmacProductSticky` 6,5%, `normalizeFmacResult` 3,7%, `updateFmacFlags` 3,0%); `processVIF1DataPrefix` 5,6%, `memmove` 5,6%, `advanceEeTimers` 4,3%.
- VU1: `calculateFmacProductSticky` não roda mais quando ninguém pode observar o registrador de status (VU1 sem FSEQ/FSAND/FSOR na imagem carregada; nenhuma das 27 imagens do Level_00 tem; FSSET sozinho só escreve). `analyzeFastPairs` marca isso por unidade; a VU0 continua exata porque o EE lê as flags dela. Replay dos traces: 0 divergências em 2327 execuções/3,17 M pares da VU1 (máscara de status 0x3F, só flags correntes) e em 8000 execuções da VU0 (máscara completa). 14,0 → **14,4 updates/s**.
- Estado no fim de 2026-10-08, Level_00: thread do jogo 99% ocupada, thread de alimentação do GS 14%, GPU folgada. Divisão da thread do jogo (frame de 69 ms): corpos recompilados da VU1 33%, `commitReadyPipelines` 9%, `runFast` 5%, flags 6%, código do jogo (EE recompilado) 9%, IOP 6%, `processVIF1DataPrefix` 6%, `memmove` 5%, `advanceEeTimers` 5%.
- Próximo passo de maior retorno: tirar a cadeia VIF1 inteira (unpack + VU1 + envio ao GS) da thread do jogo. A cadeia já é copiada no kick; falta enfileirar na mesma ordem os kicks de GIF, as escritas diretas nos FIFOs e os registradores privilegiados do GS (o flip tem de ficar atrás dos draws), manter no lado do EE uma sombra de MSKPATH3/VIF1_STAT/GIF_STAT e esvaziar a fila antes de qualquer leitura de volta. Estimativa pelo perfil: thread do jogo ~21 ms, thread VIF1/VU1 ~43 ms por frame (~23 updates/s), e daí em diante o limite é o custo da VU1.

### Cadeia VIF1 fora da thread do jogo (`PS2X_VIF1_THREAD=1`) — 2026-10-08

Com a variável ligada (o launcher liga; `BLACK_VIF1_THREAD=0` desliga), a thread do EE deixa de consumir o snapshot da cadeia que ela mesma copia no kick. Cadeias GIF, cadeias VIF1, escritas no FIFO da VIF1, reset da VIF1 e os registradores de CRT entram numa fila e são reproduzidos em ordem por uma thread ("sink") que passa a ser dona de tudo o que vem depois: estado da VIF1, VU1, máscara do PATH3 e sua fila, árbitro GIF e front end do GS. Código em `ps2_memory.cpp` (bloco "DMA sink thread"), sem mudança de header.

- O EE continua vendo o DMA concluir na hora, como no modelo síncrono. O que o jogo consulta e depende do que está na fila é mantido em sombra no lado do EE, percorrendo o stream VIF1 enfileirado com as mesmas regras de tamanho de comando do interpretador (`dmaSinkScanVif1`): máscara do PATH3 (GIF_STAT.M3P) e pacotes retidos atrás da máscara (GIF_STAT.FQC — sem isso o pump do jogo `func_002B2638` fica preso esperando FQC≠0 depois do kick do GIF).
- Os registradores de CRT (PMODE, SMODE, DISPFB/DISPLAY, BGCOLOR) viajam na fila e a apresentação usa a cópia "ordenada" (`ps2xGsSetOrderedDisplayRegs` em `gs_frontend.cpp`): o flip que o jogo escreve depois de ver o DMA concluir não pode ser apresentado antes de a thread desenhar aquele frame.
- O que não é snapshot de cadeia (transferências em modo normal, leitura de volta do GS) esvazia a fila e roda como antes. VIF0 fica na thread do jogo, sem esvaziar nada (estado próprio). Os callbacks de MSCAL/MSCNT não tocam o contexto do EE quando rodam na sink.
- Os timers do EE passaram a receber o tempo em fatias de 2048 ciclos (`advanceEeTimers` acumulava a cada checkpoint de bloco, com busca em mapa); acesso a registrador de timer aplica o que está pendente antes.
- Medido no spawn do Level_00 (visão mais pesada da fase), sem hooks: 14 → **20,5 updates/s**; em visões mais leves a fase chega a tempo real (30 updates/s por segundo de host, `g/t` ≈ 1,0). A máquina estava carregada por outra sessão compilando (load 10–40), então os números são conservadores.
- Imagem no spawn igual à do modo síncrono: mesma série de brilho e `recomp/diagnostics/path3-gate/sink-spawn.png`.
- Agora quem limita é a própria thread sink (100% ocupada): 81% VU1 — corpos recompilados 47%, `commitReadyPipelines` 8,6%, `runFast` 7,4%, `normalizeFmacResult` 4,9%, `updateFmacFlags` 3,6% — mais `processVIF1DataPrefix` 8,3% e `memmove` 8,5%. A thread do jogo fica ~50% ociosa esperando a fila.
- Limitações conhecidas (por isso é variável, não padrão do runtime): acesso direto do EE à memória da VU1 e stubs HLE do GS que chamam o front end sem passar pela fila não esvaziam a sink; SIGNAL/FINISH chegam ao CSR no tempo da sink.

Defeitos de imagem vistos nesta rodada que **não** são da sink (aparecem também no modo síncrono): a arma em primeira pessoa fica pequena no canto e em alguns frames vira espetos pretos (`sync-street.png`, quadro inferior direito); com a câmera virada para outras áreas aparecem leques de triângulos nos cantos do céu e chão verde liso (`sink-street.png`). Não investigados.

Nota de ambiente: outra sessão (repositório `silent-hill-origins-recomp`) compila e roda um binário com o mesmo nome `ps2EntryRunner`; shells e execuções daqui que citavam esse nome morreram várias vezes junto com os builds de lá. `path3_gate_test.py` agora roda uma cópia chamada `blackRunner`.

### VU1: imagens não recompiladas em jogo e gerador corrigido — 2026-10-08

- Na sessão de jogo real do usuário o log mostrou **32 imagens de microcódigo VU1 (e 1 VU0) sem código recompilado**, rodando interpretadas (~5× mais lento por par). Os roteiros automáticos (introdução + spawn) não passam por elas, por isso as medições não mostravam.
- O gerador (`ps2recomp/diagnostics/vu1_recompile.cpp`) tinha um `else` pendurado: o "emitir todos os pares" das imagens capturadas sem cobertura estava ligado ao `if` de dentro do laço, então essas imagens saíam com 0 pares (era o defeito "imagens de `vu-captured/` saem vazias"). Corrigido; com as 7 imagens já capturadas o arquivo gerado passou a 50 imagens / 35.023 pares / 5.045 corpos distintos. Dos 17.811 pares que escrevem flags FMAC, só 1.833 (10%) ainda precisam calculá-las.
- O launcher agora exporta `PS2X_VU_RECOMP_CAPTURE=recomp/diagnostics/vu-captured`: toda imagem desconhecida vista em jogo é salva (uma vez) e `ps2recomp/build_vu_recompiled.sh` a inclui na próxima geração. Fluxo: jogar → rodar o script → jogar de novo sem interpretação.
- Tentado e revertido: flags MAC "preguiçosas" (guardar o resultado bruto e derivar o MAC só na leitura). Correto nos traces (0 divergências fora do status), mas sem ganho: 14,1 ns/par no replay antes e depois, e no jogo o custo só trocou de função. O tempo de `commitReadyPipelines` vem de ser chamado a quase todo ciclo por causa de Q/stores/escritas atrasadas, não das flags.

Recebido da sessão do Silent Hill Origins (mesmo runtime), não verificado aqui: no raster SDL_GPU a tabela de swizzle de Z16/Z16S usa páginas de 64×32, mas páginas de 16 bits são 64×64 (`gs_sdl_gpu_raster.cpp`, laço da LUT; `bitAddress()` em `gs_metal_shader.h`, casos 50 e 58). Só afeta jogos com Z de 16 bits e o caminho SDL_GPU; o padrão aqui é o paraLLEl-GS.

### VU1 recompilada por microprograma, extraída do executável — 2026-10-08

Substitui o fluxo "jogar para capturar imagens" na VU1.

- Todo o microcódigo da VU1 está no `SLUS_213.76`, em comandos VIF MPG. `ps2recomp/diagnostics/extract_vu_programs.py ELF DIR [--verify imagens...]` segue as cadeias de MPG com endereços contíguos e grava cada programa em `recomp/diagnostics/vu-programs/prog1-<endereço>-<fnv>.bin`: **70 programas, 23.512 pares**. Conferido contra as 34 imagens capturadas com cobertura: os 22.774 PCs executados caem todos dentro de um programa inteiro extraído (o que sobra nas imagens é resto de programa antigo parcialmente sobrescrito, nunca executado).
- As "imagens" de 16 KiB que o runtime identificava por hash são sobreposições desses programas na ordem em que o jogo os carrega (47 distintas nas capturas, 32 novas numa única sessão de jogo). Não dá para enumerar offline, então o runtime deixou de depender delas: quando o código da VU muda, `vuMatchPrograms` (em `ps2_vu1_core.cpp`) confere quais programas da tabela gerada estão inteiros no endereço de carga e monta, por par de instruções, qual função recompilada é dona dele; `runFast` despacha por PC. A busca por imagem inteira continua (VU0 e imagens capturadas).
- O gerador (`vu1_recompile.cpp`) aceita arquivos `prog<unidade>-<endereço>-<id>.bin`: compila o programa inteiro (sem alcançabilidade) dentro de uma imagem sintética cujo entorno só lê flags MAC, para que um resultado que possa sair do programa com flags pendentes continue calculando-as. Emite `VuRecompiled::programs()` e define `PS2X_VU_RECOMPILED_PROGRAMS`.
- `build_vu_recompiled.sh` agora extrai os programas do executável e gera a partir deles mais as imagens da VU0. Arquivo gerado: 70 programas + 3 imagens VU0, 19.988 pares, 4,4 MB (antes 5,5 MB com 50 imagens).
- Validação: replay dos traces com 0 divergências (VU1 2327 execuções/3,17 M pares; VU0 8000 execuções), 9,0 ns/par (igual ao caminho por imagem). Na introdução sem pular, que antes mostrava 14 imagens "not recompiled", agora nenhuma.
- Para gerar o arquivo pela primeira vez sem esperar a compilação do antigo: um `black_vu1_recompiled.inc` stub (só `find` devolvendo `nullptr`) basta para compilar o gerador.
- Fica valendo a captura do launcher (`vu-captured/`) só para código que não venha do executável; não se espera nenhum na VU1.

### Módulo paraLLEl-GS recompilado e IOP recompilado estaticamente — 2026-10-08

**paraLLEl-GS.** `ps2recomp/gpu/build.sh` refez o clone (commit fixado, agora em `recomp/gpu/parallel-gs`) e recompilou o módulo com a troca dos `getenv` por leitura única. O jogo roda igual; no spawn a taxa ficou em ~21,6 updates/s (o gargalo é a thread da VU1, então o ganho não aparece ali).

**IOP.** Os módulos IRX deixaram de ser interpretados:
- `iop_cpu_exec.inl`: o corpo do executor virou `IopCpuCore::executeDecoded(cpu, palavra)`, compartilhado pelo interpretador (que busca a palavra) e pelo código gerado (que passa um literal, e o decode some na compilação).
- O carregador informa o tamanho do texto (`IopImageLoadResult::textSize`, do cabeçalho IOPMOD). Com `PS2X_IOP_RECOMP_CAPTURE=<dir>` o runner grava o texto já relocado de cada módulo (`iop-<base>-<fnv>.bin`); o launcher aponta para `recomp/diagnostics/iop-captured`. Os 10 módulos do Black carregam nos primeiros segundos do boot.
- `ps2recomp/diagnostics/iop_recompile.py` gera `ps2xIOP/src/emulator/black_iop_recompiled.inc` (local, nunca versionar): uma função por bloco básico (10 módulos, 6.279 blocos, 30.865 instruções) e uma tabela por módulo com a função que começa em cada palavra. `ps2recomp/build_iop_recompiled.sh` gera e recompila.
- No runtime (`iop_emulator.cpp`): ao carregar um módulo, se base, tamanho e hash do texto batem, a tabela é registrada; `runCpu` despacha o PC para o bloco. Cada instrução do bloco chama `stepRecompiled`, que mantém tudo o que `step`/`runCpu` fazem (condições do laço, interrupção, DMA, callbacks, contadores) e confere se a palavra na memória ainda é a compilada; se não for, aquela instrução volta ao interpretador. Stubs de import continuam resolvidos pelo interpretador. `PS2X_IOP_RECOMPILED=0` desliga.
- Verificação: é equivalente por construção (mesmo corpo de instrução, mesma sequência de passos); no jogo, com ligado e desligado o roteiro de pad chega ao Level_00 igual e nenhum módulo fica sem código ("has no recompiled code" não aparece). Não há teste diferencial instrução a instrução.
- Custo: **não ficou mais rápido**. Blocos recompilados 5,1% do tempo ocupado da thread do jogo contra ~5–6% do caminho interpretado (`executeInstruction` + `decode` + `step`). A primeira versão, com uma função por 8 KiB de texto, ficou 3× mais lenta (funções de 2048 instruções em que o jogo entra e sai a cada poucas instruções) e foi trocada pelos blocos. O custo do IOP nunca foi executar instruções: é o escalonador rodando a cada quantum com o IOP quase sempre ocioso.
- Por isso o launcher passou a usar `PS2X_IOP_QUANTUM=512` (padrão do runtime: 128 ciclos de IOP): o IOP cai de 19,8% para 13,3% do tempo ocupado da thread do jogo (2048 dá 12,0%). 512 ciclos são ~14 µs de tempo do console.

Com isso não resta código do console interpretado no caminho normal: EE, VU0, VU1 e IOP são recompilados; os interpretadores ficam como referência e para código que não bate com o gerado.

### FPU do EE: `sqrt.s` lia o operando errado (e mais três desvios) — 2026-10-08

Erros do tradutor de FPU do recompilador (`ps2xRecomp/src/lib/fpu_translator.cpp`), todos no código do jogo já recompilado:

- **`SQRT.S`**: no EE a codificação é `SQRT.S fd, ft` (operando no campo ft; fs é zero). O tradutor emitia `sqrt(f[fs])`, isto é, sempre `sqrt(f0)`. Das 200 `sqrt.s` do Black, 147 tinham ft≠0 e calculavam o valor errado (ex.: `0x46020084` = `sqrt.s f2,f2` virava `f2 = sqrt(f0)`). O desassemblador nem reconhece a instrução (`c1 0x…`), por isso passou despercebido.
- **`RSQRT.S`**: emitia `1/sqrt(f[fs])`; o EE calcula `fs / sqrt(ft)` (1 ocorrência no jogo).
- **`DIV.S` por zero**: dava infinito; no EE dá ±FLT_MAX (sinal de fs xor ft) — o EE não tem infinito nem NaN (1075 divisões).
- **`CVT.W.S`**: usava `nearbyintf` (arredonda); no EE sempre trunca e satura (455 ocorrências).
- Também: `SQRT`/`RSQRT` usam o módulo do operando. Macros novas em `ps2_runtime_macros.h` (`FPU_DIV_S(ctx,a,b)`, `FPU_RSQRT_S`, `ps2FpuCvtW`). Exige regerar o código do jogo (`ps2recomp/build.sh`).

Efeito no Level_00 (mesmo roteiro):
- O jogador passa a nascer **dentro da sala**, com mesa e cadeira à frente e a arma grande à direita — a mesma pose do dump do PCSX2 de 06/10 (`~/Library/Application Support/PCSX2/snaps/…111320.png`). Antes nascia do lado de fora, colado na parede das janelas, com a arma minúscula no canto: posição, câmera e arma estavam erradas por causa das raízes.
- Os "espetos pretos" da arma somem com o `CVT.W.S` truncando (conferido isolando cada correção).
- A fase roda a **30 updates/s** no spawn e nas visões testadas (antes ~21 no spawn errado), com a máquina carregada.
- Quadro: `recomp/diagnostics/path3-gate/fpufix-spawn.png`.

**Ainda errado nessa visão:** a casca do prédio (paredes de tijolo, teto, janelas) que aparece no PCSX2 não é desenhada; vê-se o cenário externo e o céu atrás da mesa. É igual com a VU de referência (`PS2X_VU_RECOMPILED=0`), então não vem da VU1 recompilada; fica para investigar no lado do EE (visibilidade/setores) ou no stream.

Conferido e sem problema: os códigos de função COP1 usados pelo jogo (0x00–0x07, 0x16, 0x18, 0x1C, 0x1E, 0x24, 0x28, 0x29, 0x32, 0x34, 0x36 e CVT.S.W) estão todos tratados; `VRSQRT` da VU0 ignora o numerador, mas as 394 ocorrências usam `vf0w` (=1).

## VF0 zerado nas threads secundárias: casca do prédio sumida e jogador preso (2026-10-08)

**Causa:** `R5900Context()` zerava tudo e só o contexto da thread principal recebia `vu0_vf[0] = (0,0,0,1)`. Toda thread do jogo criada depois (e invocações de interrupção/callback com contexto novo) rodava com VF0 = 0. O jogo carrega 1.0 com `vaddw.x vfN, vf0, vf0w`; na thread que carrega a fase, o construtor das seções do mundo (`func_00125C60`) montava a matriz identidade em `obj+0x70` e saía tudo zero. A esfera de visibilidade (`func_0012A158`: matriz × `obj+0x60` → `obj+0x20`, registrada na árvore de esferas `func_00272C28`) ia para a origem.

**Sintomas que isso causava:** paredes/teto/janelas da sala inicial ausentes, objetos aparecendo e sumindo ao girar a câmera, personagem sem sair do lugar (a animação da arma respondia).

**Correção:** `vu0_vf[0] = (0,0,0,1)` no construtor de `R5900Context` (`ps2xRuntime/include/ps2_runtime.h`). Header global: rebuild completo.

**Como foi achado (método reutilizável):** o savestate do PCSX2 (`~/Library/Application Support/PCSX2/sstates/*.p2s`) é um zip com membros zstd (método 93; extrair o membro cru e passar no `zstd -d`). `eeMemory.bin` são os 32 MiB do EE. Nossa RAM sai com `PS2X_RAM_DUMP=<arquivo>` + `<arquivo>.trigger`. O heap do jogo é determinístico: os endereços dos objetos coincidem entre as duas RAMs, então dá para comparar objeto por objeto (classe pelo ponteiro de vtable em `obj+0x10`). As 31 seções do mundo (vtable `0x3DC920`, draw `func_00127CF0`) existiam e estavam habilitadas nas duas; só `+0x20` e `+0x70..0xAF` diferiam.

Mapa útil: fila de desenho em `[D_0040F4C0]+0x14+0xCA58 + balde*0x18` = `{itens, chaves, n, cap}`, item de 0x14 bytes (`geo, matriz, ...`); teste caixa × 6 planos `func_0026DB20` (microprograma VU0 `0x880`; 0 fora, 1 cruza, 2 dentro), usado para oclusores (`D_0040F4F4`) e frustum (`[D_0040F4C0]+0xCFD0`). Sonda: `ps2recomp/overrides/black_cull_probe.cpp` (`BLACK_CULL_PROBE`, `BLACK_CULL_FORCE`, `BLACK_CULL_SAMPLES`, `BLACK_QUEUE_SITES`). `black_debug.cpp`: `[black-fs] open` agora mostra código de erro e nº de handles; `BLACK_WATCH_ENTRY` imprime o histórico ao armar.

Quadro depois da correção: `recomp/diagnostics/path3-gate/vf0fix.png`.

**Cuidado ao medir:** `path3_gate_test.py` com `UNSET=PS2X_GS_SDL_GPU` e `prof.py` com `PS2X_GS_SDL_GPU=<unset>` caem no rasterizador de CPU; para paraLLEl-GS é preciso passar `PS2X_GS_PARALLEL=1 PS2X_GS_PARALLEL_MODULE=… PS2X_GS_MOLTENVK=…` como o launcher faz.

**Pendente:** `func_0027CAB8` (abrir arquivo) falha de forma intermitente, cada rodada num arquivo diferente (`bg1_pst.db`, `speech.slb`, `CredRoll.ssh`…), com o arquivo presente — só há 2 handles e parece corrida entre threads; ferramentas do difftest de macro VU0 (`vu0_macro_emit.cpp`, `vu0_macro_difftest.cpp`) ainda sem seção própria; `VSQRT`/`VRSQRT` de macro com operando negativo.

## Arredondamento do EE: truncar, como o PS2 (2026-10-08)

**Sintoma:** depois do conserto do VF0, objetos/texturas ainda sumiam e voltavam ao olhar para direções específicas.

**Causa:** o código do EE (COP1 e macros COP2 da VU0) rodava com o arredondamento padrão do host (para o mais próximo). O PS2 trunca (em direção a zero). O sin/cos embutido do jogo (ex.: `func_0016C728`, init dos oclusores) reduz o ângulo somando e subtraindo `1.5·2^23` (`vmsubai`/`vmaddai` com `I = 0x4B400000`) para obter a parte inteira de `x/2π`; isso só funciona truncando. Com round-to-nearest, sin(350°) saía −0,16727 em vez de −0,17365 (reproduzido bit a bit numa simulação IEEE fora do jogo). Matrizes de rotação e planos dos oclusores ficavam tortos → oclusão errada em certos ângulos.

**Correção:** `std::fesetround(FE_TOWARDZERO)` no início da `GameThread` (`ps2_runtime.cpp`); todo código EE do jogo roda nela. `PS2X_EE_ROUND_NEAREST=1` volta ao padrão do host para A/B. O HLE `__ieee754_rem_pio2f` (`LibC.cpp`) passou de `nearbyintf` para `roundf` para não depender do modo. A VU em microprograma já truncava (`ps2_vu1_core.cpp`).

**Validação:** as matrizes dos 6 oclusores (`[D_0040F4F4]+0xD0`, `obj+0x70..0x10F`) ficaram idênticas bit a bit às do savestate do PCSX2 (antes diferiam). O teste caixa × planos (`func_0026DB20`, programa VU0 `0x880`) já estava certo: 8000 amostras reais batem com um modelo offline. Quadro: `recomp/diagnostics/path3-gate/roundfix-spawn.png`.

Ferramentas: `ps2recomp/diagnostics/vu_disasm.py <imagem> <pc hex> <pares>` (desmontador simples de microcódigo VU); `BLACK_FRAME_RING=N` + `BLACK_DUMP_EVERY=2` dá uma sequência de quadros (`BLACK_DUMP_EVERY=1` não funciona). Teclado: WASD só no analógico esquerdo, setas só no direcional, IJKL câmera (`ps2_pad.cpp`).

**Não tratado:** a divisão do EE no PCSX2 arredonda para o mais próximo por padrão; aqui `div.s` também trunca agora. `VSQRT`/`VRSQRT` de macro com operando negativo e as aberturas de arquivo intermitentes seguem pendentes.

### Em aberto: chão liso/buracos perto da porta da sala inicial (2026-10-08)

Pose reproduzível: no spawn, `rs_left:0.7` no roteiro de pad (tempo em segundos do guest; a fase começa em g≈30 ou g≈62 conforme a rodada, então conferir o quadro). Quadro: `recomp/diagnostics/path3-gate/floor-defect-pose.png`. O que já foi descartado:
- culling das seções do mundo (`BLACK_CULL_FORCE=1` não muda; programa VU0 `0x880` confere com modelo offline);
- renderizador (paraLLEl-GS e rasterizador de CPU mostram o mesmo chão liso);
- o triângulo do chão chega ao GS com textura PSMT4 128×128 mipmapada (`MXL=4`, `MMIN=4`, `K=-50`, `L=0`), paleta normal e UV plausíveis (`gif_prims_at_point.py <captura> <x> <y> <bytes finais> <n>` lista as primitivas que cobrem um ponto da tela).
Hipótese atual: falta uma camada de detalhe (sujeira/entulho) desenhada por objetos, não pelas seções do mundo. A fila de desenho do PCSX2 tem mais itens em todas as faixas, mas a pose do savestate é outra, então não dá para comparar item a item. Falta um savestate do PCSX2 na mesma pose.

## VU: stalls de pipeline no caminho rápido — geometria que sumia conforme a câmera (2026-10-09)

**Sintoma:** piso, partes do braço/arma, decalques e fogo sumiam e voltavam dependendo de para onde a câmera olhava. Captura do usuário (`BLACK_CAPTURE=1` no launcher) mostrou que as seções eram enfileiradas com o flag de clipping certo, mas os triângulos próximos não chegavam ao GS: era o clipper da VU1.

**Causa:** o caminho rápido da VU (`stepPair`, usado pelo interpretador rápido e pelo código recompilado) não contava três esperas que o hardware faz, e o pipeline de flags anda por ciclo. Um `FMAND` do clipper lia os flags da instrução errada.
1. Hazard de registrador: um par que lê VF/VI ainda no pipeline de quem escreve espera (sem bypass).
2. Recurso da EFU: uma operação da EFU espera a anterior terminar (`m_efuResourceReady`).
3. O stall é reavaliado depois de cada espera: um `XGKICK` espera a transferência anterior inteira.

**Como foi isolado:** teleporte para a pose da captura (`BLACK_POSE_REF=<RAM>` + `<RAM>.trigger`, em `black_cull_probe.cpp`); `PS2X_VU_ACCURATE=1` conserta; bisseção com `PS2X_VU_ACCURATE_PARTS` (1 = stalls, 2 = VF atrasado, 4 = ACC, 8 = VI) mostrou que só os stalls bastam. Duas hipóteses anteriores estavam erradas e foram descartadas por teste: elisão de flags MAC (não conserta) e "só hazards de registrador" (faltavam EFU e a reavaliação).

**Correção:**
- `PairFacts::hazardReads/hazardWrites` (derivados de `InstructionUsage`), passados como literais a `stepPair`; bits novos `PairEfu` e `PairIndirect`.
- `pruneFastHazards()` reduz as máscaras ao que pode de fato travar: uma leitura só espera por registrador escrito num dos 3 pares executados antes (predecessores pelo fluxo do código). ACC nunca trava. Depois de `JR/JALR` os predecessores são desconhecidos: os 4 pares seguintes passam por `waitHazardsOfPair()` (fora do caminho quente) e as escritas em volta de todo salto indireto ficam registradas.
- O motor de referência passou a usar stalls por padrão; `PS2X_VU_NO_HAZARDS=1` desliga nos dois motores.
- A elisão de flags MAC ficou desligada em programas que têm leitor de flags (a análise conta pares, não ciclos); `PS2X_VU_FLAG_ELISION=1` religa.
- Resultado no gerador: 8.254 corpos de par, 292 com checagem de leitura, 2.726 com marcação de escrita.

**Validação:** rápido × referência = 0 divergências em 3,17 M pares (VU1) e 395 k (VU0). O trace antigo tinha 189 registros com P diferente (gravados com a temporização antiga): `ps2x_vu1_trace_replay <trace> --rebaseline <saida>` regrava os resultados esperados com a referência; trace antigo em `vu-trace/level00.pre-stalls.bak`. No jogo, com o interpretador rápido corrigido, o piso volta na pose da captura.

**Armadilhas desta rodada:**
- Sem `black_vu1_recompiled.inc` o runner cai no interpretador de referência: um teste "no modo normal" sem o arquivo gerado não testa o caminho rápido. Para testar o rápido sem regenerar: `PS2X_VU_FAST=1 PS2X_VU_RECOMPILED=0`.
- Mudar a assinatura de `stepPair` quebra a compilação do gerador enquanto o `.inc` antigo existir: mover o `.inc` para fora antes de rodar `build_vu_recompiled.sh`.
- Embutir a checagem em todo par fez a compilação do `.inc` passar de uma hora; por isso a poda.

## 16:9 e janela (2026-10-09)

- `BLACK_WIDESCREEN=1` (`black_widescreen.cpp`): o jogo decide 16:9 por `sceScfGetAspect()` via `func_0026F2E0`; o override faz essa função responder 16:9 e força `settings+4 = 1` em `func_00108BB8`.
- `PS2X_DISPLAY_ASPECT=1.7778` estica a apresentação; `PS2X_WINDOW_SIZE=1920x1080` é em pixels reais (em Retina vira 960×540 pontos).
- `PS2X_GS_UPSCALE=2|4` (supersampling do paraLLEl-GS + scanout em alta resolução) existe mas derruba o jogo na inicialização no MoltenVK (textura Metal com altura inválida). Não usar ainda.
- O savestate do PCSX2 usado como referência está em 16:9 com escala 1,3333 no visor (o jogo usa 1,2): comparar filas de desenho com ele exige o mesmo modo.

## Correções tardias de 2026-10-09 (EE, libm, compilação)

- **Teste diferencial do EE** (`ps2recomp/diagnostics/ee_difftest.cpp`, alvo `ps2x_ee_difftest`): 2557 palavras distintas do jogo contra um modelo independente do R5900, 0 divergências depois de corrigir MOVZ/MOVN (copiavam 32 bits), PSLLVW/PSRLVW/PSRAVW (operandos trocados) e `rsqrt.s` com divisor zero/denormal. Sem referência ainda: `pmulth`, `qfsrv`, `ll`, fluxo de controle, máscara de endereço de LQ/SQ.
- **libm de `double`**: as stubs liam/escreviam `double` pelo FPU; no EE `double` é soft-float (GPR de 64 bits). Corrigido em `LibC.cpp` (`libmDoubleArg`/`libmSetDoubleResult`). Era a causa das partículas grossas.
- **`-ffp-contract=off`** em todo o runtime (`ps2xRuntime/CMakeLists.txt`): o FPU e o VU do PS2 multiplicam e somam em passos separados; o Clang no arm64 fundia em FMA. Os traces do VU foram regravados.
- **Supersampling (`PS2X_GS_UPSCALE=2`)**: o paraLLEl-GS renderiza em 4×, mas a leitura do scanout na CPU sai com o layout errado (imagem repetida). Desligado; pendente junto com a apresentação nativa (`PS2X_GS_NATIVE_PRESENT=1`, ainda não conferida visualmente).
- O patch `0001-black-runtime-fixes.patch` agora inclui `ps2xRuntime/CMakeLists.txt` no `git diff`.

## VU: caminho para o backend direto (2026-10-09)

Objetivo: custo por par bem abaixo do console (3,39 ns/par no trace `level00`), para caber 120 atualizações/s. Medidas no replay (`ps2x_vu1_trace_replay ... --engine recompiled --bench N --bench-engine fast`), sempre com 0 divergências contra a referência:

| etapa | ns/par |
|---|---|
| início do dia (stalls corretos, elisão de flags ciente de stall) | 13,6 |
| cache da análise por imagem + flags sob demanda (só 5 programas quentes recompilados) | 9,9 |
| aritmética FMAC em flush-to-zero | 8,7 |

O que mudou no núcleo (`ps2_vu1_core.cpp`, `ps2_vu1_upper.inl`, `ps2_vu1.h`):

- **`ImageAnalysis`**: decodificação, fatos por par e donos recompilados ficam guardados por conteúdo da imagem (hash + tamanho). O jogo reenvia os mesmos microprogramas o tempo todo; antes cada envio redecodificava e reanalisava 2048 pares e recasava os 70 programas.
- **Flags sob demanda (`LazyFlags`)**: em imagens sem leitor do registrador de status (todo o VU1 do jogo), um FMAC vivo só grava os operandos do último estágio (`x`, `y`, tipo) num anel de 8; MAC/status são derivados em `resolveLazyFlags()` quando um FMEQ/FMAND/FMOR/FS* lê, ou no fim da execução. `PS2X_VU_NO_LAZY_FLAGS=1` desliga. Flags sticky de Z/S/U/O não são mantidas nesse modo (já não eram com a elisão).
- **Flush-to-zero**: `runFast` liga FPCR.FZ. Com FZ e arredondamento para zero, add/sub/mul nunca produzem denormal nem infinito e tratam operando denormal como zero, então a "normalização" do PS2 sai de graça; resta trocar inf/NaN de operando pelo maior finito (3 instruções NEON). É o mesmo modo que o PCSX2 usa no VU. `fzLaneFlags()` reconstrói Z/S/U a partir dos operandos (soma/produto exato em `double`). A referência não usa FZ, mas passou a normalizar o produto intermediário de MADD/MSUB/OPMSUB para ter a mesma semântica (1 registro do `level00` muda; regravar com `--rebaseline`).
- MAX/MINI/ABS/ITOF/FTOI/CLIP continuam com normalização completa dos operandos.

Ciclo de desenvolvimento rápido: gerar o `.inc` só com os programas quentes (`prog1-0000-{d1574f8a…,fc1517cb…,bd4e199d…,92ff0071…,f6eba9e7…}.bin`) compila em ~10 min em vez de 20–40; sem `.inc` nenhum, ~4 min (valida o motor rápido interpretado). `PS2X_VU_PC_HISTOGRAM=<arquivo>` (motor rápido interpretado) grava contagem e ciclos por par de cada imagem. No `level00`: stalls são 3,5% dos ciclos; dois laços de 19 pares (0x630 e 0x6C8 do programa `d1574f8a`) somam 48% dos pares executados.

### Contabilidade por par, XGKICK sob demanda e blocos diretos (2026-10-09, continuação)

- `stepPair` só restaura VF0/VI0 quando o par pode escrevê-los (`vuPairMayWriteZeroRegister`) e só passa pelo desvio pendente em pares que são desvio ou delay slot (bit `PairNoPendingBranch`, posto pelo gerador). Cuidado: o par do próprio desvio precisa passar por ali, é onde o atraso é contado.
- **XGKICK sob demanda**: os motores rápidos não avançam mais a transferência PATH1 ciclo a ciclo; `syncXgkick()` a traz até o ciclo atual antes de qualquer gravação na memória do VU, do XGKICK seguinte e do fim da execução. No fim do programa `flushPipelines()` avança direto de qword em qword.
- **Blocos diretos** (`ps2recomp/diagnostics/vu1_direct_emit.inl`): o gerador escreve NEON puro para trechos em linha reta do VU1 (FMAC add/sub/mul e variantes, MAX/MINI/ABS/ITOF/FTOI, LQ/SQ e variantes, inteiros, MOVE/MR32, MTIR/MFIR, FMEQ/FMAND/FMOR e, no fim, um desvio condicional com seu delay slot). VF/VI/ACC ficam em variáveis locais; ciclo, PC e contagem de pares são gravados uma vez no fim; a confirmação de Q/P/CLIP é uma comparação por par. O gerador rastreia quais componentes são sabidamente normais para dispensar o ajuste de operando. Pares fora do conjunto, com stall possível, fim de programa, ou até 4 pares depois de um retorno de sub-rotina continuam em `stepPair`. Se a condição de entrada falha (`m_hazardUnknown`, flags não preguiçosas, orçamento), a função devolve `NotHandled` e o motor interpretado rápido executa aqueles pares.
- Depuração: `PS2X_VU_NO_DIRECT=1` (runtime) e `PS2X_VU_RECOMPILE_NO_DIRECT=1` (gerador) desligam os blocos; `PS2X_VU_DIRECT_RANGE=<lo>-<hi>` e `PS2X_VU_DIRECT_LIMIT=<n>` restringem quais blocos/execuções rodam direto, para bisseção com o replay (o replay agora mostra o qword divergente).
- **Erro antigo da referência achado pelos blocos**: quando a instrução de cima de um par tinha VF0 como destino (ex.: `ITOF0 vf0, vf9`), a de baixo do mesmo par lia o valor escrito em vez da constante (0,0,0,1). No hardware a escrita em VF0 é ignorada. Corrigido em `stepPair` e no laço de referência; 4 registros do `level00` mudaram (uma componente saía 32× maior num microprograma quente) e os traces foram regravados.

| etapa | ns/par (5 programas quentes) |
|---|---|
| contabilidade por par reduzida + XGKICK em lote | 7,9 |
| blocos diretos | 6,5 |

(Com os 70 programas a etapa anterior aos blocos deu 6,34 ns/par; a medida com 5 programas inclui ~20% de pares interpretados.)

## 120 fps: levantamento inicial de constantes de taxa (2026-10-09)

O jogo é baseado em ticks: `func_0027F730(taxa)` grava `D_0040EBAC = 1/taxa` e os subsistemas recebem dt. Fora isso, o executável tem constantes imediatas que podem depender de 30 Hz e precisam ser auditadas uma a uma antes de mudar a taxa (varredura de pares `lui`/`ori`):

- `1/30` (0x3D088889): 2 locais — 0x124FD4 (`func_00124E90`), 0x14081C (`func_001407C8`).
- `30.0` (0x41F00000, só `lui`): 26 locais (primeiros: 0x100FB0, 0x10A1E0, 0x10A460, 0x129BA0, 0x13D0FC, 0x13FCE4, 0x17D8B8, 0x183EC4, 0x1894EC, 0x189DB4, 0x18DD98, 0x18DEE8); 6 palavras em dados.
- `60.0` (0x42700000): 20 locais (0x101944, 0x101EB0, 0x10BD48, 0x1283CC, 0x129B98, 0x15878C…); 3 em dados.
- `1/60`: 1 local (0x37C460, provavelmente dado).

Nem todo `30.0`/`60.0` é taxa de quadros (podem ser ângulos, distâncias). O primeiro teste continua sendo taxa 60 com 2 atualizações por vblank.

### Blocos diretos, versões 2 e 3 (2026-10-10)

- **Stalls dentro do bloco**: a base de ciclos do bloco (`c`) é variável. Par com `hazardReads` testa `m_vfReady`/`m_viReady` em linha; par com `PairStall` testa FDIV/XGKICK em linha e só chama `waitPairStall()` quando precisa esperar. Depois de uma espera o bloco confere o orçamento de ciclos e, se não couber, sai antes do par (grava registradores, PC, estado do desvio pendente) e o motor interpretado continua.
- **`m_hazardUnknown` dentro do bloco**: os quatro primeiros pares de cada bloco fazem a checagem completa (`waitHazardsOfPair`) enquanto o contador não zera. Sem isso, toda entrada de programa e todo retorno de `JR` mandava o trecho inteiro para o interpretador.
- **Mais instruções**: DIV/SQRT/RSQRT (em linha, com confirmação rápida de Q em `D_commit`), CLIP, FCEQ/FCAND/FCOR/FCGET, ILW/ISW/ILWR/ISWR, MFP, XTOP/XITOP, XGKICK, WAITQ.
- **Encadeamento e laços**: um bloco continua depois de um desvio condicional (saída lateral quando tomado); quando o destino é outro bloco da mesma função, salta direto (`goto`), e quando é o próprio início do bloco repete sem tirar os registradores das variáveis locais. O laço principal do Level_00 (0x630–0x758, 38 pares) vira um único laço nativo.
- **XGKICK**: cópia em lote até o fim dos dados de cada GIFtag, na execução e no `flushPipelines()`.
- Flags sob demanda: caminho rápido em `fzLaneFlags()` quando o resultado não tem expoente zero.

| etapa (70 programas, trace `level00`) | ns/par | pares em blocos |
|---|---|---|
| antes dos blocos | 6,34 | 0% |
| blocos v1 | 5,06 | — |
| blocos v2 (stalls, mais instruções, XGKICK em lote) | 3,67 | 92,2% |
| blocos v3 (saídas laterais, laços nativos) | 3,48 | 95,2% |
| + clip flags sob demanda (`LazyClip`, `resolveLazyClip()`), CLIP em linha | 3,29 | 95,2% |

A medida inclui ~0,5 ns/par de reposição da memória de dados feita pelo próprio replay. O console faz 3,39 ns/par. No perfil da v3 sobram: corpos dos blocos ~57%, resolução de flags em FMAND ~9%, fila de clip flags (`queueClip` + `commitReadyPipelines`) ~10%.

Clip flags: no modo sob demanda, CLIP/FCSET gravam (ciclo em que sai do pipeline, novo valor) num anel de 8 e FCEQ/FCAND/FCOR/FCGET pegam o mais novo já pronto; `drainLazyFlags()` devolve os pendentes à fila ciclo a ciclo ao sair do modo.

Próximo no VU: flags resolvidas na geração quando escritor e leitor estão no mesmo bloco, corpos dos blocos (registradores carregados/gravados por bloco), e depois os 120 fps propriamente ditos (taxa de ticks 60/120).

## Taxa de ticks 60: primeiro experimento (2026-10-10)

Como o jogo marca o tempo: `func_0027F730(taxa)` (30 em NTSC, 25 em PAL) grava `D_0040EBAC = 1/taxa` e `D_0040EBA8 = 1000/taxa`; `func_002B4DD0(n)` grava em `D_0040DF74` quantos vblanks cada quadro dura (2 no console) e o tratador de vblank escrito à mão em `0x2B2BA8` só apresenta quando o contador chega a esse valor.

`BLACK_TICK_RATE=60` (`ps2recomp/overrides/black_tick_rate.cpp`, experimental, desligado por padrão) multiplica a taxa e divide o intervalo. Funciona: o jogo passa a fazer 60 atualizações por segundo de tempo do jogo. Mas o host não acompanha: no Level_00 o tempo do jogo anda a 0,5× (30 atualizações por segundo real), ou seja, o custo por atualização é ~33 ms e não há folga a 30.

Perfil por thread (amostragem de 8–10 s no Level_00, `PS2X_VIF1_THREAD=1`, paraLLEl-GS):

| | 30 ticks | 60 ticks |
|---|---|---|
| thread do jogo ocupada | 97% | 90% |
| — dentro de `sub_002B32D8` (envia a lista e espera o DMA em laço) | 27% | 14% |
| — código do jogo (EE) | 33% | 33% |
| — VU0 | 13% | 16% |
| — IOP | 18% | 14% |
| — agendamento/despacho do EE | 21% | 13% |
| thread VIF1/VU1 ocupada | 54% | 71% |

A 60 ticks a thread do jogo gasta ~25 ms de trabalho real por atualização e a do VU ~23 ms; para 60 quadros por segundo as duas precisam ficar abaixo de 16,6 ms, e para 120, de 8,3 ms.

Alvos na thread do jogo, por retorno esperado: VU0 (roda com flags imediatas e sem blocos diretos; `commitReadyPipelines` e `calculateFmacProductSticky` aparecem no topo), IOP em passo travado com o EE (`IopKernel::beginNextReady`, `runEeCycles`), agendamento do EE (`EeScheduler::checkpointDue`, `dispatchGuestBranch`, acesso a TLS, `advanceEeTimers`), e por fim a qualidade do código EE recompilado.

## Rumo aos 60: medição limpa e gargalos fora da VU (2026-10-10, madrugada)

**Medição limpa.** `ps2recomp/diagnostics/perf_run.py <tag> [VAR=valor ...]` roda o jogo com as configurações do launcher e sem ganchos de depuração, entra no Level_00 pelo script de controle, amostra o processo e imprime atualizações/s (`BLACK_FPS`), ocupação por thread (`sample_threads.py`) e em que cada thread espera (`sample_waits.py`). O `path3_gate_test.py` força `BLACK_DEBUG=1` e a captura do fluxo do GS, que aparecem como `ReadVram`, cópias e contenção de mutex: serve para validar imagem, não para medir.

O que mudou e o efeito a 60 ticks (`BLACK_TICK_RATE=60`, Level_00, atualizações por segundo real):

| mudança | atualizações/s |
|---|---|
| ponto de partida (VU1 v3) | 30 (jogo a 0,5×) |
| VU0 com flags sob demanda e blocos diretos (`black_vu0_flags.cpp`, `PS2X_VU0_FLAGS_UNOBSERVED`) + contabilidade do EE em lotes de 128 ciclos (`PS2X_EE_ACCOUNT_BATCH`) + histórico de despacho opcional (`PS2X_DISPATCH_HISTORY=1`) | 31–42 |
| 8 contextos de quadro no Granite (`PS2X_GS_FRAME_CONTEXTS`, era 2: a thread do GS parava a cada `flush` esperando a GPU) | 38–55 |
| caminho rápido do UNPACK do VIF1 (sem máscara, modo 0, CL=WL) | 46–57 |

- **VU0**: o EE do Black nunca lê status/MAC/clip do VU0 (nenhum CFC2 dos registradores 16–18 no executável). Com isso o VU0 usa o mesmo caminho do VU1: 95% dos pares em blocos, 24 → 10,5 ns/par no `vu0-session.bin`, 0 divergências. O gerador e a validação rodam com `PS2X_VU0_FLAGS_UNOBSERVED=1` (posto em `build_vu_recompiled.sh`); o runner liga o mesmo no início. O que sobra na VU0 é custo fixo por chamada (`m_vu0.reset()`, cópia do estado na ida e na volta).
- **`sub_002B32D8`** não espera DMA: depois de enviar a lista ele fica em laço chamando o callback ocioso (`sub_001C4D00`) até o tratador de vblank trocar o quadro. A 30 ticks isso é folga, não trabalho.
- **UNPACK**: `PS2X_VIF_UNPACK_VERIFY=1` roda o caminho rápido ao lado do laço geral e compara; 2,1 milhões de UNPACKs no Level_00, 0 diferentes.
- **IOP**: o quantum padrão do runtime é 128 ciclos; o launcher usa 512. As medições antigas pelo harness inflavam a fatia do IOP.

Continuação da mesma noite:

| mudança | atualizações/s a 60 ticks |
|---|---|
| blocos: tempos de "pronto" gravados só na saída do bloco, ajuste de operando feito uma vez para registradores que o bloco não escreve (VU1 3,15 ns/par) | 47–58 |
| despacho de chamadas do EE: `checkpointDueFast()` em linha, uma consulta à tabela e um acesso TLS por chamada | 51–57 |
| UNPACK rápido também para STMOD 1/2 (soma de ROW) e para escrita com passo (WL=1, CL>1): 4,2 milhões verificados, 0 diferentes | 46–58, thread do VU1 de 99% para 86% |
| `execute()` do VU só reinicia o agendador inteiro se sobrou algo pendente; `beginMicroCall()` na VU0 (VU1 2,96 ns/par, VU0 7,2 ns/par) | 53–57 |
| trava do backend paraLLEl-GS solta durante a espera da cópia do scanout (ABI do módulo v6: `unlock`/`relock` em `GSParallelScanout`) | 56–60 na maior parte do percurso, ~48 num trecho pesado |

Série típica de `BLACK_FPS` a 60 ticks (leituras de 2 s): menus 60,0 cravado; Level_00 56–60, com um trecho de ~8 s em 47–49 e volta a 60+. A 30 ticks: 30,0 cravado, sem as quedas para 27–28 que existiam no começo da noite.

Achados que valem para a próxima etapa:
- O perfil por endereço do `sample` (coluna `+ offset`) resolve o que o perfil por função esconde: foi assim que apareceu que o laço geral do UNPACK ainda rodava (STMOD e passo) depois do primeiro caminho rápido.
- A 60 ticks, thread do jogo: ~46% código do EE (dos quais ~9% são o laço de espera do vblank), ~11% despacho, ~10% IOP, ~9% VU0, ~5% cópias do DMA. `PS2X_IOP_QUANTUM=2048` não mudou nada em relação a 512.
- O Granite com 16 contextos de quadro não rende mais que com 8.
- `PS2X_GS_NATIVE_PRESENT=1` não muda a taxa (a espera era a fila de contextos, não a leitura do scanout).

### Alta resolução (`PS2X_GS_UPSCALE=2`): ainda com defeito (2026-10-10)

As linhas do scanout agora são gravadas com o passo da própria imagem (`parallel_gs_module.cpp`), mas o defeito não era esse: a imagem 2560×1792 que o paraLLEl-GS devolve já mostra só o canto superior direito do quadro (160×112 pixels nativos), ampliado e repetido 4 vezes na horizontal. `PS2X_GS_SCANOUT_FLAGS=<máscara>` (1 `raw_circuit_scanout`, 2 `adapt_to_internal_horizontal_resolution`, 4 `internal_resolution_scanout`, 8 `anti_blur`) troca as opções de leitura: 0 e 2 dão quadro preto (1280×448 e 2560×448), 4, 5 e 6 dão o mesmo defeito. O próximo passo é comparar com o caminho de `high_resolution_scanout` do PCSX2 (patches em `recomp/gpu/parallel-gs/misc/`) e conferir `SMODE1/SMODE2` que passamos; `gs_renderer.cpp:4297` desliga o modo quando eles não batem com o esperado.

### Fecho da madrugada de 2026-10-10

- `runFast` liga arredondamento para zero e flush-to-zero com uma escrita de FPCR (sem `fesetround`); a cópia da lista de DMA reserva espaço pelo tamanho da anterior do mesmo canal.
- Mesmo percurso com o script de controle a 30 e a 60 ticks (`path3_gate_test.py`, com e sem `EXTRA_BLACK_TICK_RATE=60`): o quadro final é o mesmo (mesma sala, mesma pose), com 3841 atualizações a 30 e 7261 a 60 no mesmo tempo de jogo. O movimento do jogador e a câmera, pelo menos, respeitam o dt.
- Estado a 60 ticks no Level_00: 56–60 atualizações/s na maior parte do percurso e ~45–50 num trecho pesado. Ali o quadro passa pouco dos 16,6 ms, perde o vblank e espera um inteiro; faltam uns 10% na thread do jogo (EE ~40% de trabalho real + ~10% de laço de espera, IOP ~12%, despacho ~11%, VU0 ~8%).
- VU1 2,94 ns/par, VU0 ~7–8 ns/par, 0 divergências nos dois traces.

Candidatos para o que falta (nenhum é ajuste pequeno):
1. IOP em thread própria (hoje executa dentro da thread do jogo, ~12% dela).
2. Chamadas diretas entre funções do EE no código gerado, sem passar por `dispatchGuestBranch` (~9% entre a função e o acesso TLS). Precisa manter a tabela de funções por causa dos overrides.
3. Espera do vblank bloqueante em vez do laço do jogo, e apresentação sem quantizar em vblanks inteiros (VRR).
4. Para 120: a thread VIF1/VU1 está em ~87% a 60; dobrar a taxa exige dividir esse trabalho (por exemplo, VIF/UNPACK numa thread e VU1 em outra) ou reduzir o custo por par à metade.

### Espera do vblank sem custo de host (2026-10-10, fim da madrugada)

Teste que derrubou uma hipótese: `PS2X_EE_CYCLE_SCALE=50` (fator novo no agendador: multiplica os ciclos cobrados pelos checkpoints do código gerado; 100 por padrão) **piorou** o trecho pesado a 60 ticks (de ~47 para ~35 atualizações/s). Motivo: o laço em que o jogo espera o vblank (`sub_002B32D8` chamando `sub_001C4D00`) é emulado a mais ou menos 1× o tempo real, ou seja, esperar 1 ms de tempo de jogo custa ~1 ms de processador. Um quadro que termina cedo não dá folga nenhuma ao host, e em cena pesada o tempo de jogo fica abaixo do real.

Correção: `ps2recomp/overrides/black_idle_wait.cpp` cobra 2000 ciclos de tempo ocioso do EE (`PS2Runtime::eeChargeIdleCycles`, não escalado) a cada chamada do callback ocioso; a espera chega ao vblank em poucas mil voltas e o agendador dorme até a hora do vblank no host. `BLACK_IDLE_WAIT=0` desliga, `BLACK_IDLE_WAIT_CYCLES=<n>` ajusta.

| | antes | com a espera barata |
|---|---|---|
| 30 ticks: thread do jogo ocupada | 98% | 56% |
| 30 ticks: atualizações/s | 30,0 | 30,0 |
| 60 ticks: mínimo no trecho pesado | 45 | 51,5 |
| 60 ticks: média no Level_00 | 56,4 | 57,2 |

A 60 ticks, no trecho pesado, quem limita agora é a thread VIF1/VU1 (99%): programas da VU1 ~65% dela (espalhados por muitos blocos, não um laço só), resolução de flags ~7%, cópias de pacotes GIF ~6% (três cópias por pacote: XGKICK, árbitro, fila do GS), `GS::processGIFPacket` ~3%.

### Cena pesada de referência e chamadas dentro dos blocos (2026-10-10, manhã)

**Cena de referência.** O usuário achou um ponto do Level_00 (olhando pela janela lateral, 16:9) com ~117 mil primitivos por quadro, onde a thread VIF1/VU1 ficava em 99% mesmo a 30 ticks. Ficou salvo: `recomp/diagnostics/perf/heavy-spot.ram` (posição; `POSE=<arquivo>` em `perf_run.py` e `soak_run.py` teleporta o jogador para lá com `BLACK_POSE_REF`) e `recomp/diagnostics/vu-trace/heavy-spot.bin` (3000 chamadas da VU1, 1 a cada 20, 4,2 milhões de pares). O percurso do script de controle é leve demais para servir de medida.

**Chamadas dentro de blocos custam caro.** Um bloco mantém ~25 registradores vetoriais vivos; qualquer chamada de função dentro dele faz o compilador salvar e recarregar todos (no AArch64 os registradores NEON são do chamador). O perfil por endereço mostrava as amostras concentradas logo depois de cada `bl`. Passaram a ser resolvidos em linha, sem chamada no caminho comum:
- leitura de MAC (`D_mac`): acha o registro mais novo já pronto e tira Z/S do resultado NEON; só chama o caminho exato quando uma componente zero pode ser underflow. Também trata em linha o caso de mais de 8 resultados gravados sem leitura, que era o comum e fazia tudo cair no caminho lento;
- confirmação de Q (`D_commit`);
- leitura de clip flags (`D_clipFlags`);
- espera de hazard sem nada pendente no intervalo (só move a base de ciclos).

| trace | antes | depois |
|---|---|---|
| `heavy-spot` (VU1) | 3,04 ns/par | 2,23 |
| `level00` (VU1) | 2,94 | 2,23 |
| `vu0-session` | ~7,2 | ~5,4–6,8 (medida ruidosa) |

No jogo, no ponto pesado: a 30 ticks, 30 cravado com a thread do VU1 em 60% (era 99% e caía para 25); a 60 ticks, ~36 atualizações/s (era ~33), com VU1 em 84% e a thread do jogo em 76%. O limite ali passou a ser o GS: a thread do paraLLEl-GS fica 69% ocupada montando primitivos (`packed_STQRGBAXYZ`, `drawing_kick`) e o resto esperando a GPU; a VU1 espera 15% em `feedAppend` e o jogo 22% esperando a VU1.

**Congelamento em aberto.** Duas vezes em jogo real (não reproduzido em 12 minutos de `soak_run.py`): `qsort` (0x35EC50), chamado por `0x1AF298` para ordenar um balde da fila de renderização (`[D_0040F4C0]+0x14+0xCA58+k*24`: itens, vetor de ordenação, contagem, capacidade, índice, comparador), recebe base e comparador inválidos (0x02D402A9; 0x7800E802). A função que adiciona itens (0x1AF1C0) confere a capacidade, então o cabeçalho do balde foi sobrescrito por outra coisa. O launcher agora grava `recomp/diagnostics/fault.ram` e o histórico de chamadas quando isso acontecer (`PS2X_FAULT_RAM_DUMP`, `PS2X_DISPATCH_HISTORY=1`). Logs: `recomp/diagnostics/freeze*-2026-10-10.log`.

### Renderizador nativo do GS — fase 0 e primeira versão da fase 1 (2026-10-10)

Plano, decisões e andamento em `ps2recomp/GS_NATIVE_RENDERER_PLAN.md`. Resumo: dump reproduzível de GS no módulo paraLLEl (`PS2X_GS_DUMP*`), ferramenta `gs-replay` (replay em qualquer módulo + comparação de quadros), captura de referência `recomp/diagnostics/gs-dump/heavy.gsdump` (119/120 scanouts idênticos ao jogo no paraLLEl), e módulo Metal `ps2recomp/gpu/native/` (`libblack-native-gs.so`, mesma ABI) que já desenha a cena no alvo principal; a imagem final ainda sai errada por causa do pós-processamento (fase 3). Não é padrão em lugar nenhum; o launcher continua no paraLLEl-GS. `gs_feature_census.py` agora lê dumps (eventos de registrador e de scanout) e teve os endereços de DTHE/COLCLAMP corrigidos.

### Congelamento em jogo: causa raiz encontrada (2026-10-10)

O congelamento aleatório (`qsort` saltando para um ponteiro de comparação inválido na fila de desenho) era um erro do tradutor da EE: `BLTZ/BGEZ/BLEZ/BGTZ` (e as formas likely/link) eram emitidos testando só os 32 bits baixos (`GPR_S32`), e o R5900 testa o registrador de 64 bits inteiro. `func_0028A688` (preenchimento por inundação de um grafo, chamada por `func_0028AFF8` ← `func_00289F78` ← `func_00175750`) lê uma máscara de 64 bits e desvia com `bltz` (bit 63); com o teste errado a travessia reempilhava os mesmos nós para sempre, a pilha interna de 2.500 entradas (objeto em `0x4CBB50`, pilha em `+0x210`, ponteiro em `+0x1598`, sem checagem) estourava, sobrescrevia o próprio ponteiro e passava a escrever sobre a tabela de baldes em `[D_0040F4C0]+0x14+0xCA58`.

Como foi achado: `BLACK_WATCH=1` no launcher roda o jogo sob lldb com `ps2recomp/diagnostics/watch_bucket.py`, que arma um ponto de vigia de hardware em um campo da tabela quando `func_001AF580` a inicializa e registra a pilha de chamadas (os nomes `sub_XXXXXXXX` dão a função do jogo) e a RAM no momento da escrita. Não custa desempenho e o lldb não pede senha para binários locais.

Correção: `ps2xRecomp/src/lib/control_flow_emitter.cpp` passa a emitir `GPR_S64`; os 1.417 arquivos gerados com esses desvios (2.780 pontos) receberam a mesma troca por `sed`, equivalente a regenerar. Vale conferir de novo os sintomas antigos de comportamento (camada de detalhe faltando, diferenças de fila contra o PCSX2), que podem ter a mesma origem.

### Comparação com o PCSX2 depois da correção dos desvios (2026-10-10)

Savestate do usuário (`recomp/diagnostics/pcsx2/state01.eeMemory.bin`, rua do Level_00 no meio de um tiroteio) contra a nossa RAM teleportada para a mesma pose (`recomp/diagnostics/pcsx2/ours.ram`). A fila de desenho não deu para comparar item a item (o nosso jogo estava numa pausa de tutorial, com a câmera antiga). O que a comparação do **código** do jogo na RAM mostrou:

- **Divisão da VU0 em modo macro (corrigido).** O PCSX2 troca a ordem de dois pares de instruções em `0x37EB14` e `0x37EB30` (patch do GameIndex: "COP2 Rearrangement. Fixes broken collisions"). O jogo faz `vdiv Q = 1/vf17.y` e logo em seguida `vaddq.x vf17 = Q`: no console essa leitura ainda vê o resultado da divisão anterior (1/x). O nosso código resolvia a divisão na hora e gravava 1/y em x: recíprocos errados no código de colisão. Varredura de todo o código gerado: 409 divisões, 249 seguidas de `vwaitq`, e só esses 2 lugares leem Q dentro da latência. Correção: os dois pares trocados no arquivo gerado e em `ps2recomp/add_instruction_patches.py` (roda no `build.sh`).
- **Patch de 60 fps do PCSX2 do usuário:** muda só `li a0,30→60` (`0x102648`, `0x125078`) e o intervalo de vblank `2→1` (`0x12508C`, `0x1C5B10`). É o mesmo que `black_tick_rate.cpp` faz por gancho nas duas funções. Não há outras constantes de 30 Hz nesse patch.
- **Escrita em código pela nossa HLE:** `ps2_stubs::sceMpegCreate` grava a estrutura de trabalho em `0x1717BC..0x171904`, que no ELF é código (o ponteiro vem do jogo; no PCSX2 esse trecho fica intacto no momento do savestate). Como o código é recompilado estaticamente não tem efeito, mas o ponteiro passado merece uma olhada.
- `BLACK_WATCH_ADDR` / `BLACK_WATCH_ARM` / `BLACK_WATCH_HITS` generalizam `watch_bucket.py` para qualquer endereço.
