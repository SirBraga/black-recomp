# black-recomp

## O que é

Isso aqui é uma tentativa de rodar o **Black** (PS2, EA/Criterion) nativo no Mac, sem emulador.

A ideia é recompilar o código do jogo pra ARM64 usando o [PS2Recomp](https://github.com/ran-j/PS2Recomp) do ran-j. O resto (gráficos, vídeo, som, controle) fica por conta do runtime do PS2Recomp, com um monte de correções minhas e um backend gráfico em Metal.

Só funciona no macOS com Apple Silicon (testei num M1 Pro).

**Não tem nenhum arquivo do jogo aqui.** Nada de ISO, ELF, BIOS, textura ou código gerado a partir do jogo. Pra rodar você precisa da sua própria cópia do Black. Os scripts não usam BIOS, só os arquivos do seu disco.

## Até onde chegou

- O jogo abre, passa pelo menu, toca os vídeos e chega na primeira fase (Level_00).
- **As texturas da sala agora aparecem certas.** O problema era a ordem dos uploads: o jogo libera as texturas aos poucos, mas o runtime soltava tudo de uma vez e as paletas eram sobrescritas antes de serem usadas. Agora sai um pacote por vez e as paletas chegam certas na hora do desenho.
- No menu e nos vídeos fica entre uns 24 e 30 updates/s.
- Dá pra pular vídeo com Tab ou Select.
- Teclado e controle funcionam (veja [CONTROLES.md](CONTROLES.md)).
- Os gráficos rodam na GPU via SDL_GPU + Metal.

## O que falta

Por enquanto ainda não dá pra jogar de verdade.

- **Desempenho:** no Level_00 o jogo fica em uns 2 a 3 updates/s. A correção das texturas não mudou isso. A GPU engasga com o volume de uploads e a fila às vezes trava vários segundos. O próximo passo é diminuir esse volume.
- **Câmera no começo da fase:** a arma aparecia deslocada pro lado. Ainda não foi confirmado se isso está resolvido.
- A correção das texturas só foi testada no Level_00.
- **Mostrar a imagem direto da GPU,** sem copiar cada quadro de volta pra CPU.
- **VU1 ainda é lenta,** porque roda no interpretador.
- Falta confirmar que dá pra controlar o personagem de boa na fase.

Os detalhes técnicos de verdade estão em [PS2_PROJECT_STATE.md](PS2_PROJECT_STATE.md) e [GPU_BACKEND_PLAN.md](GPU_BACKEND_PLAN.md).

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
4. Compila tudo: `ps2recomp/build.sh`
5. Joga: abre o `ps2recomp/JogarBlack.command`

## Créditos

- [PS2Recomp](https://github.com/ran-j/PS2Recomp), do ran-j e contribuidores. Tudo aqui é em cima dele, e o patch segue a licença dele (GPL-3.0).
- SDL3, Raylib, Dear ImGui, rlImGui, FFmpeg, paraLLEl-GS e MoltenVK.
- PCSX2 e RT64 como referência.

Black é marca da Electronic Arts. Projeto independente, sem ligação com a EA ou a Criterion.
