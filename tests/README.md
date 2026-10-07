# Teste de entrada do controle

## Regressão de pausa em chamadas aninhadas

```sh
python3 ps2recomp/tests/dispatch_yield_test.py
```

Compila os corpos atuais de `dispatchGuestBranch` e `eeCheckpointDue` contra um scheduler determinístico. Verifica retorno normal, pausa de descendente no endereço de retorno do ancestral, pausa no próprio entry-PC, preservação da pilha e retorno de stub HLE. Os dois casos de pausa falhavam antes da propagação explícita de transferências. Esse teste não substitui a execução do jogo.

## Falha de inicialização gráfica

```sh
python3 ps2recomp/tests/window_init_test.py
```

Executa o trecho real de inicialização da Raylib com retorno de plataforma simulado, tanto falho quanto bem-sucedido. Verifica que uma plataforma indisponível não inicializa GPU. A correção da dependência fica em `ps2recomp/dependencies/raylib-window-init.diff` e é reaplicada de forma idempotente por `build.sh` após configurar as dependências.

## Entrada de pad

Na raiz do projeto, depois de configurar o runtime:

```sh
clang++ -std=c++17 -Itools/PS2Recomp/ps2xRuntime/include -Itools/PS2Recomp/out/rt/_deps/raylib-src/src ps2recomp/tests/pad_input_test.cpp tools/PS2Recomp/ps2xRuntime/src/lib/ps2_pad.cpp -o /tmp/black-pad-input-test
/tmp/black-pad-input-test
```

O teste usa o backend real de pad e simula somente a entrada Raylib. Verifica teclado, script, cancelamento de direções opostas, liberação, prioridade do gamepad e buffers inválidos. Não confirma resposta do personagem.

Teclado: W/A/S/D = analógico esquerdo (mantém também os botões digitais para menus); I/J/K/L = analógico direito. Setas = direcional digital. E = R1; Enter = Start.

O script aceita `ls_left`, `ls_right`, `ls_up`, `ls_down`, `rs_left`, `rs_right`, `rs_up`, `rs_down`, combináveis com botões: `PS2X_PAD_SCRIPT='30:ls_up+rs_right:1'`. Os horários usam o relógio guest quando instalado. O script substitui somente as dimensões analógicas mencionadas e libera a substituição ao terminar.

### Rasterização de triângulos e IMAGE2

`python3 ps2recomp/tests/gs_triangle_test.py` compila o backend GS real e compara
42 snapshots completos da VRAM entre 0, 1 e 4 workers. Inclui textura sobre o
framebuffer, alias framebuffer/Z, quatro formatos de framebuffer e quatro de Z, wrap da VRAM, filtros,
blend, máscara e 13 formatos de textura. Repete os quatro modos de
arredondamento; os workers devem herdar o ambiente de ponto flutuante do VU. O benchmark impresso mede apenas os triângulos sintéticos.

`python3 ps2recomp/tests/gif_image2_test.py` extrai o parser XGKICK de produção e
verifica PACKED, REGLIST, IMAGE e IMAGE2, payload na volta da memória VU e rejeição
de pacotes maiores que o staging buffer. Os mocks substituem apenas o transporte.

Referência para IMAGE2: `.kiro/skills/ps2-recomp/resources/09-ps2tek.md`, GIFtag
Format (FLG 2 e 3 são IMAGE), e
https://github.com/PCSX2/pcsx2/blob/master/pcsx2/GS/GSState.cpp.

### Interrupções pendentes

`python3 ps2recomp/tests/irq_pending_test.py` usa `queueInvocation` de produção
para verificar que 100 mil sinais repetidos mantêm uma única entrega pendente,
handlers distintos permanecem separados, uma IRQ ativa pode receber uma entrega
posterior, e comandos SIF não são agrupados nem descartados.

`python3 ps2recomp/tests/irq_preemption_test.py` verifica que o scheduler só despacha outra thread após o retorno do ISR, preservando eventos, timers e a preempção comum.

`python3 ps2recomp/tests/vif_image2_test.py` verifica continuação IMAGE/IMAGE2 através de DIRECT/DIRECTHL e preserva o GIFtag seguinte.

`python3 ps2recomp/tests/vu_dependency_test.py` compara os métodos de readiness/write do VU com varredura completa dos 16 registradores, cobrindo todas as 65.536 máscaras e demais pipelines. Benchmark mede somente o traversal, não o FPS do jogo.

`python3 ps2recomp/tests/gs_sprite_test.py` usa o backend real para desenhar um gradiente conhecido e verificar orientação de textura em sprites normais/invertidos nos eixos X/Y, com UV e ST. O teste falhava com a ordenação antiga dos endpoints.

- `parallel_gs_frontend_test.py`: frontend GS real + módulo GPU/MoltenVK, lifecycle pré-init, raw GIF, sprite/clear/upload/FIFO. Exige Mac arm64 e módulo construído.
- `parallel_gs_module_test.cpp`: ponte ABI, GIF dividido com paths intercalados, primitive GPU, native image/FIFO, cópia local multi-page de 224x240 com comparação completa de pixels. Construído por gpu/build.sh.
- Para testar exportação/presentação Metal em macOS, acrescente `--metal-present` ao `recomp/gpu/build/black-parallel-module-test recomp/gpu/build/libblack-parallel-gs.so recomp/gpu/moltenvk/libMoltenVK.dylib`. O teste cria uma janela Cocoa, apresenta 32 quadros pelo CAMetalLayer e exige scanout Vulkan exportável.
- `metal_present_capabilities.cpp`: consulta a MoltenVK configurada e exige `VK_EXT_metal_objects` no Apple Silicon. Exemplo: `clang++ -std=c++20 -I/path/to/parallel-gs/Granite/third_party/khronos/vulkan-headers/include ps2recomp/tests/metal_present_capabilities.cpp -o /tmp/black-metal-present-capabilities && /tmp/black-metal-present-capabilities recomp/gpu/moltenvk/libMoltenVK.dylib`.

- `vu_sparse_commit_test.py`: 150 mil operações aleatórias contra full scan anterior; latências, lanes, sequências WAW, filas cheias, pending masks e reset. Benchmark sparse é isolado, não FPS.

- `mmi_permutation_test.py`: macros reais PEXEH/PEXEW/PROT3W contra a ordem de lanes do EE, 100001 entradas; ASan/UBSan e verificação de que o gerador chama os helpers testados. Semântica conferida no interpretador MMI do PCSX2.
- `vu_neon_normalization_test.py`: normalização vetorial ARM64 de produção contra normalização escalar, quatro milhões de valores bit a bit, incluindo subnormais, sinais de zero, Inf e NaN. O benchmark mede apenas normalização, não FPS.

- `vu_queue_reference.inc`: versão anterior das operações de inserção nas filas, usada pelo teste diferencial para que alterações no código de produção não alterem também o oracle.
- `gpu_texture_snapshot_test.py`: captura VRAM real no carregamento TEX0 e verifica todos os4096pixels/índices decodificados contra uma paleta conhecida. Exige os targetsGPU já construídos. Detecta tabelas de swizzle não inicializadas no diagnóstico.

- O teste de texturas inclui preservação do CLUT interno depois de sobrescrever a VRAM da paleta (PSMT4 ePSMT8, CLD=0).
- `gpu_texture_snapshot_test.py` também valida256cores do CLUT interno (planos RG/BA) e o contexto do trigger. O decoder aceita um quinto argumento opcional com o `.clut.bin`.

- `vif_progressive_test.py`: compara o parser VIF1 real em execução integral e por prefixos de comandos, com ASan/UBSan. Cobre lookahead/alinhamento V3, ciclos fill, ROW/COL, máscaras, MPG, DIRECTHL e callbacks MSCAL/MSCNT. O alvo Ninja `ps2x_dma_cooperative_smoke` verifica STR/IRQ e reentrância no PS2Memory real. O modo `PS2X_DMA_COOPERATIVE=1` é experimental e não está ativado no launcher; os resultados visuais e limites temporais estão em `diagnostics/CLUT_OVERLAP_LEVEL00.md`.
