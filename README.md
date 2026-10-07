# black-recomp

Recompilação estática do **Black** (EA / Criterion, PlayStation 2, `SLUS_213.76`) para rodar nativamente no **macOS / Apple Silicon**, usando o [PS2Recomp](https://github.com/ran-j/PS2Recomp) (ran-j) como recompilador e runtime.

O código MIPS do ELF do jogo é traduzido para C++ pelo PS2Recomp e compilado como executável ARM64 nativo. O que não é código do EE (GS, VIF/GIF, VU0/VU1, IOP, DMA, vídeo MPEG) é tratado pelo runtime do PS2Recomp, que aqui recebe uma série de correções e um backend gráfico GPU (SDL_GPU sobre Metal).

> **Status: experimental, ainda não jogável.** O jogo inicializa, passa pelo menu e pelos vídeos e chega ao Level_00, mas a gameplay ainda é muito lenta e tem falhas graves de renderização. Não existe uma métrica validada de "percentual restante" para um recomp jogável.

## Nenhum arquivo do jogo está incluído

Este repositório contém **apenas** código, scripts, patches, testes e anotações escritos à mão. Ele **não** contém e nunca deve conter:

- BIOS do PS2, ISO, ELF (`SLUS_213.76`), arquivos extraídos do disco, vídeos, texturas ou áudio do jogo;
- dumps de memória/VRAM, capturas de microcódigo VU ou qualquer outro dado extraído do jogo;
- o código C++ gerado pelo recompilador a partir do ELF (`sub_*.cpp`, `register_functions.cpp`, headers gerados).

Para usar, você precisa da **sua própria cópia, obtida legalmente**, do Black para PS2. O `.gitignore` bloqueia os tipos de arquivo derivados do jogo.

## Onde o projeto está hoje

Resumo baseado em `PS2_PROJECT_STATE.md`, `GPU_BACKEND_PLAN.md`, `ALTERNATIVE_GRAPHICS_ROADMAP.md`, `CONTROLES.md`, `gpu/README.md` e `tests/README.md` (os documentos completos estão neste repositório e têm os detalhes, logs e números de cada medição).

### O que já funciona

- **Boot até o Level_00.** O runner inicializa, passa pelo menu principal, toca os FMVs e chega ao briefing de Veblensk e ao Level_00. No modo GPU-only, o loader alcançou o estado de gameplay do Level_00 sem rejeições de operações GS.
- **Vídeos.** Os clipes `.M2V` (MPEG-2 640×480) são decodificados via FFmpeg (HLE de `sceMpeg*`), com slice threading. No menu e nos clipes o jogo roda por volta de 24–30 updates/s.
- **Skip de cutscenes.** Tab / Select pede a parada ao próprio player de vídeo do jogo (`BLACK_CUTSCENE_SKIP=1`, ligado no launcher); confirmado o skip do FMV seguido do carregamento do Level_00. Cenas dentro da engine ainda não têm skip validado.
- **Controles.** Teclado com analógicos (W/A/S/D e I/J/K/L) e gamepad com prioridade; ver [CONTROLES.md](CONTROLES.md).
- **Backend GS SDL_GPU / Metal (padrão do launcher).** Com `PS2X_GS_SDL_GPU=1`, o GS roda em compute shaders Metal via SDL_GPU, sem MoltenVK e **sem fallback de rasterização por CPU** (operação sem kernel GPU interrompe com erro explícito). Já cobertos em GPU: sprites, triângulos, linhas, line strips e pontos; CLUT; clear; uploads host→local de 32/24/16/8/4 bits; cópias local→local; leituras local→host; Z16/Z16S, CT16S com dither; mipmaps (TEX1/MIPTBP); e conversão do scanout (CT32/CT24/CT16/CT16S) com readback assíncrono por fences em anel de três buffers.
- **Smoke tests.** `ps2x_sdl_gpu_backend_smoke` passa integralmente no Metal (raster e diferenciais contra a referência CPU, guarda GPU-only, transferências, scanout síncrono e assíncrono, CLUT). Há também uma suíte de testes de regressão do runtime (dispatcher, IRQs, GIF IMAGE2, VIF, VU, pad, inicialização de janela etc.) em [tests/](tests/README.md).
- **Backend paraLLEl-GS (experimental).** Módulo dinâmico com paraLLEl-GS/Vulkan sobre MoltenVK, com apresentação nativa Metal opcional (`PS2X_GS_NATIVE_PRESENT=1`) que elimina o readback do scanout. Não é o caminho escolhido e não é habilitado no launcher; ver [gpu/README.md](gpu/README.md).
- **VU AOT (experimental, opt-in).** Microprogramas VU capturados localmente são convertidos para C++ nativo por hash (`PS2X_VU_AOT=1`), mantendo o scheduler do interpretador. O bundle local cobre os pares-PC observados de 33 imagens VU1 e 3 imagens VU0 do Level_00. As capturas e o header gerado não são distribuídos.
- **Correções no runtime** (todas em patches): propagação de pausas no dispatcher de chamadas aninhadas, coalescência de IRQs pendentes e preempção só após o ISR, GIF IMAGE2, limite de tags da cadeia VIF1 ampliado para 16.384, correção da inicialização de janela do Raylib, NEON na VU, permutações MMI, entre outras.

### Gargalo principal atual

Dentro do Level_00 a taxa cai de ~30 updates/s (menu) para **~0,2–2,8 updates/s**. As medições instrumentadas mostram:

- A GameThread fica parada em `METAL_WaitForFences` dentro de `stageUploadData`, sob `PS2Memory::processVIF1Data` → upload VIF1/GIF. A fila da GPU acumula trabalho de upload e não termina antes de a arena de staging (128 MiB) precisar ser reciclada: as esperas de fence nas voltas da arena chegaram a **~18–19 s** (18,07 s / 18,98 s; em outro run 18,36 s / 19,02 s). Aumentar a arena só espaça a pausa.
- O mapeamento do staging não é o problema (máximo ~0,02 ms por chamada); `finishPass`/`submit` ficam abaixo de 0,02 ms.
- O readback síncrono do scanout para a janela Raylib/OpenGL somou **~6 s (5,97 s) em 981 esperas**, com pico de 106 ms.
- No trecho medido houve ~5,3 milhões de draws acelerados e ~247 mil batches/chunks de imagem. Um primeiro agrupamento de uploads reduziu pouco o número de dispatches (402.678 batches para 407.597 chunks) e a taxa ficou em 2,7–2,9 updates/s.

**Próximo passo:** reduzir a quantidade/granularidade de dispatches no fluxo VIF1/GIF com um kernel de upload **multi-op**, que acumula uploads de imagem consecutivos e os envia num único dispatch quando `gsTransferRange` prova que os intervalos de páginas de VRAM são disjuntos (no primeiro overlap, operação GS intermediária ou scratch cheio, o lote é despachado e a ordem é mantida; ranges desconhecidos seguem no caminho serial). Em paralelo, já foi implementado e passou no smoke o compartilhamento de compute pass entre draws sem textura com faixas de páginas de cor/profundidade disjuntas; falta medir o efeito no Level_00. Uma solução estrutural posterior é apresentar a imagem direto da GPU, sem o readback de 1.120 KiB por quadro.

### O que ainda falta

- **Desempenho jogável no Level_00:** resolver o acúmulo da fila GPU de uploads (acima), as esperas de apresentação e o custo da VU1 interpretada, que nos perfis aparece como outro grande consumidor de CPU (`VU1Interpreter::run`, `processVIF1Data`).
- **Corrupção visual:** a imagem do Level_00 ainda tem texturas/cores severamente erradas (também no backend CPU). A investigação mostrou CLUTs sobrescritas por uploads dentro do mesmo pacote PATH3 antes dos TEX0 do PATH1, o que aponta para escalonamento DMA/GIF/VIF. A correção exige avanço cooperativo dos canais/FIFO e IRQs, comparado com uma referência (PCSX2); o protótipo `PS2X_GIF_INCREMENTAL=1` não corrigiu a imagem. Detalhes em [diagnostics/CLUT_OVERLAP_LEVEL00.md](diagnostics/CLUT_OVERLAP_LEVEL00.md).
- **Apresentação GPU-residente:** substituir o readback + `UpdateTexture` da janela Raylib/OpenGL por uma swapchain SDL_GPU/Metal, preservando a UI de debug e a composição CRT/campos.
- **Validação da gameplay:** confirmar visualmente frames contínuos e controle do personagem no Level_00 e medir updates/s de forma repetível (A/B no mesmo trecho).
- **VU AOT:** ampliar cobertura e variantes, reduzir o custo do dispatch e medir A/B; VU0 e instruções não reconhecidas seguem majoritariamente no interpretador.
- **Trilhas futuras** (não prioritárias, ver [ALTERNATIVE_GRAPHICS_ROADMAP.md](ALTERNATIVE_GRAPHICS_ROADMAP.md)): cache de texturas GPU com invalidação por conteúdo, interceptação de comandos gráficos de alto nível, eventuais experimentos de VU em GPU.
- Skip de cutscenes dentro da engine e validação de outras fases além do Level_00.

## Estrutura do repositório

| Caminho | Conteúdo |
|---|---|
| `build.sh` | Pipeline completo: analyzer → entry points por ponteiro → recompilador → cópia para o runner → build nativo |
| `run.sh` | Roda o runner por N segundos e salva o log (diagnóstico) |
| `JogarBlack.command` | Launcher para jogar (SDL_GPU/Metal, GPU-only) |
| `build_vu_experimental.sh`, `run_vu_experimental.sh` | Build/execução com o VU AOT experimental (requer capturas locais) |
| `find_code_pointers.py` | Encontra alvos de código alcançáveis só por ponteiro no ELF (vtables, callbacks) |
| `patches/PS2Recomp-local-changes.patch` | **Estado atual completo** das modificações no PS2Recomp (runtime, IOP, GS/GPU, VU), relativo ao commit upstream `75d729c` |
| `patches/0001-black-runtime-fixes.patch`, `patches/0002-gpu-native-present.patch` | Patches anteriores do runtime (histórico; o patch acima já incorpora essas mudanças, com edições posteriores) |
| `overrides/` | Overrides C++ específicos do Black (I/O de CD, skip de cutscene, contador de FPS, hooks de debug) |
| `dependencies/` | Correções mínimas para Raylib, Granite e paraLLEl-GS |
| `gpu/` | Módulo experimental paraLLEl-GS + apresentador Metal |
| `tests/` | Testes de regressão do runtime e do backend GPU |
| `diagnostics/` | Ferramentas de diagnóstico, probes SDL_GPU/Metal, relatórios e logs de texto |
| `PS2_PROJECT_STATE.md` | Diário principal de estado do projeto |
| `GPU_BACKEND_PLAN.md` | Plano e histórico do backend GPU (estado mais recente) |
| `ALTERNATIVE_GRAPHICS_ROADMAP.md` | Trilhas futuras para VU, GS e texturas |
| `CONTROLES.md` | Controles |

## Como compilar e rodar (visão geral)

Testado em Mac Apple Silicon (M1 Pro). Os scripts esperam este repositório como `ps2recomp/` dentro de uma pasta de trabalho, ao lado do PS2Recomp e dos arquivos do **seu** disco:

```
<pasta-de-trabalho>/
├── ps2recomp/            # este repositório
├── tools/PS2Recomp/      # clone do PS2Recomp no commit 75d729c + patch local
├── orig/SLUS_213.76      # ELF do seu disco
├── orig/Black.iso        # ISO do seu disco (2048 bytes/setor)
├── recomp/disc/          # arquivos extraídos do seu disco (inclui SLUS_213.76)
└── .venv/bin/ninja       # ninja usado pelos scripts
```

1. Clone este repositório: `git clone https://github.com/SirBraga/black-recomp.git ps2recomp`.
2. Clone o PS2Recomp em `tools/PS2Recomp`, faça checkout de `75d729c` e aplique o patch atual:
   ```sh
   git clone https://github.com/ran-j/PS2Recomp.git tools/PS2Recomp
   git -C tools/PS2Recomp checkout 75d729c
   git -C tools/PS2Recomp apply ../../ps2recomp/patches/PS2Recomp-local-changes.patch
   ```
   O `build.sh` tenta aplicar todos os `patches/*.patch` de forma idempotente; com o patch completo já aplicado, ele apenas avisa que o `0001` não aplica limpo e segue.
3. Coloque seus arquivos em `orig/` e `recomp/disc/`. Dependências: CMake, Ninja, Python 3, AppleClang, SDL3 (Homebrew; testado 3.4.16) e as dependências que o PS2Recomp baixa/usa (Raylib 5.5, Dear ImGui/rlImGui, FFmpeg).
4. `ps2recomp/build.sh` compila o PS2Recomp, roda o analyzer (gera `recomp/black.toml`), o `find_code_pointers.py`, o recompilador, copia o código gerado e os overrides para o runner, aplica a correção do Raylib e compila `ps2EntryRunner`. `--skip-recomp` reaproveita o código já gerado.
5. Rodar: `ps2recomp/JogarBlack.command` (jogar, SDL_GPU/Metal) ou `ps2recomp/run.sh [segundos] [log]` (execução limitada para diagnóstico, com `BLACK_DEBUG=1`).

O módulo paraLLEl-GS é construído separadamente por `gpu/build.sh` (ver [gpu/README.md](gpu/README.md)).

## Créditos

- [PS2Recomp](https://github.com/ran-j/PS2Recomp), de ran-j e contribuidores: recompilador estático e runtime nos quais todo este trabalho se apoia. O patch deste repositório é uma modificação do PS2Recomp e segue a licença dele (GPL-3.0).
- [SDL3 / SDL_GPU](https://github.com/libsdl-org/SDL): backend GPU Metal.
- [Raylib](https://github.com/raysan5/raylib), [Dear ImGui](https://github.com/ocornut/imgui) e [rlImGui](https://github.com/raylib-extras/rlImGui): janela, entrada e UI de debug do runtime.
- [FFmpeg](https://ffmpeg.org/): decodificação dos vídeos MPEG-2.
- [paraLLEl-GS](https://github.com/Arntzen-Software/parallel-gs) (Arntzen Software), com Granite, e [MoltenVK](https://github.com/KhronosGroup/MoltenVK): backend GS experimental Vulkan.
- Referências técnicas: [PCSX2](https://github.com/PCSX2/pcsx2) (inclusive o renderer Metal e o GS), [RT64](https://github.com/rt64/rt64) e a documentação PS2tek.

Black é marca da Electronic Arts. Projeto independente, sem afiliação com a Electronic Arts ou a Criterion.
