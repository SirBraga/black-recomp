# black-recomp

## O que é

Isso aqui é uma tentativa de rodar o **Black** (PS2, EA/Criterion) nativo no Mac, sem emulador.

A ideia é recompilar o código do jogo pra ARM64 usando o [PS2Recomp](https://github.com/ran-j/PS2Recomp) do ran-j. O resto (gráficos, vídeo, som, controle) fica por conta do runtime do PS2Recomp, com um monte de correções minhas.

Só funciona no macOS com Apple Silicon (testei num M1 Pro).

**Não tem nenhum arquivo do jogo aqui.** Nada de ISO, ELF, BIOS, textura, microcódigo ou código gerado a partir do jogo. Pra rodar você precisa da sua própria cópia do Black. Os scripts não usam BIOS, só os arquivos do seu disco.

## Até onde chegou

- O jogo abre, passa pelo menu, toca os vídeos e chega na primeira fase (Level_00).
- **Texturas certas,** na introdução e na jogabilidade. Eram dois bugs: a ordem dos uploads de textura e um estouro da lista de DMA do próprio jogo.
- **Nada do console é interpretado no caminho normal:** EE, VU0, VU1 e IOP rodam recompilados.
- **O microcódigo da VU sai direto do executável.** Os 70 microprogramas são extraídos do seu ELF e recompilados na hora do build, então não precisa jogar pra capturar nada. Nada disso vem no repo. No replay dos traces gravados deu 0 divergências.
- **Bug da FPU achado:** o recompilador traduzia o `sqrt.s` errado (e mais três coisas da FPU). Com isso corrigido, o jogador nasce dentro da sala, a arma aparece no lugar certo e os "espetos pretos" sumiram.
- **~30 updates/s no spawn do Level_00** (antes ~21). Parte disso pode ser porque o prédio ainda não é desenhado (veja abaixo).
- Gráficos via paraLLEl-GS (Vulkan em cima do MoltenVK), ou SDL_GPU + Metal se o módulo não estiver compilado. VIF1/VU1 rodam numa thread própria.
- Dá pra pular vídeo com Tab ou Select. Teclado e controle funcionam (veja [CONTROLES.md](CONTROLES.md)).

## O que falta

Ainda não dá pra jogar de verdade.

- **O prédio da sala não aparece:** paredes, teto e janelas somem e dá pra ver o céu atrás da mesa. Acontece igual com a VU de referência, então a suspeita é o teste de visibilidade do lado do EE. Tô investigando.
- **A VU1 ainda é a parte mais pesada:** ~9 ns por par de instruções. O código é recompilado, mas ainda gera um passo por par, sem gerar por bloco.
- **Talvez a gente desenhe demais:** ~4.300 draws por quadro no spawn contra 2.364 num dump do PCSX2. Só dá pra comparar direito quando as duas imagens baterem.
- O IOP recompilado não ficou mais rápido e ainda não tem teste instrução por instrução.
- Tudo isso só foi testado no Level_00.

Os detalhes técnicos de verdade estão em [PS2_PROJECT_STATE.md](PS2_PROJECT_STATE.md).

## Como rodar

Os scripts esperam esse repo numa pasta `ps2recomp/`, ao lado do PS2Recomp e dos seus arquivos do jogo:

```
pasta/
├── ps2recomp/          # esse repo
├── tools/PS2Recomp/    # PS2Recomp no commit 75d729c
├── orig/SLUS_213.76    # ELF do seu disco
├── orig/Black.iso      # ISO do seu disco
└── recomp/disc/        # arquivos extraídos do seu disco
```

1. Clona esse repo: `git clone https://github.com/SirBraga/black-recomp.git ps2recomp`
2. Clona o PS2Recomp, volta pro commit certo e aplica o patch:
   ```sh
   git clone https://github.com/ran-j/PS2Recomp.git tools/PS2Recomp
   git -C tools/PS2Recomp checkout 75d729c
   git -C tools/PS2Recomp apply ../../ps2recomp/patches/PS2Recomp-local-changes.patch
   ```
3. Coloca os arquivos do seu disco em `orig/` e `recomp/disc/`. Vai precisar de CMake, Ninja (em `.venv/bin/ninja`), Python 3 e SDL3 (Homebrew).
4. Compila o jogo: `ps2recomp/build.sh`
5. Gera a VU a partir do seu ELF: `ps2recomp/build_vu_recompiled.sh` (demora uns 20 min)
6. Opcional: `ps2recomp/gpu/build.sh` pro paraLLEl-GS (precisa do MoltenVK em `recomp/gpu/moltenvk/`)
7. Joga: abre o `ps2recomp/JogarBlack.command`
8. Depois da primeira vez, roda `ps2recomp/build_iop_recompiled.sh` pra recompilar o IOP com os módulos que o launcher salvou

## Créditos

- [PS2Recomp](https://github.com/ran-j/PS2Recomp), do ran-j e contribuidores. Tudo aqui é em cima dele, e o patch segue a licença dele (GPL-3.0).
- SDL3, Raylib, Dear ImGui, rlImGui, FFmpeg, paraLLEl-GS e MoltenVK.
- PCSX2 e RT64 como referência.

Black é marca da Electronic Arts. Projeto independente, sem ligação com a EA ou a Criterion.
