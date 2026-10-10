# black-recomp

![Gameplay em Veblensk](docs/gameplay.png)

Veblensk rodando nativo no Mac.

## O que é

Isso aqui é uma tentativa de rodar o **Black** (PS2, EA/Criterion) nativo no Mac, sem emulador.

A ideia é recompilar o código do jogo pra ARM64 usando o [PS2Recomp](https://github.com/ran-j/PS2Recomp) do ran-j. O resto (gráficos, vídeo, som, controle) fica por conta do runtime do PS2Recomp, com um monte de correções minhas.

Só funciona no macOS com Apple Silicon (testei num M1 Pro).

**Não tem nenhum arquivo do jogo aqui.** Nada de ISO, ELF, BIOS, textura, microcódigo ou código gerado a partir do jogo. Pra rodar você precisa da sua própria cópia do Black. Os scripts não usam BIOS, só os arquivos do seu disco.

## Quanto falta pra ser um recomp bom de jogar

**Leitura de hoje (2026-10-10): uns 45%.** A meta é jogar o Black no PC com gráfico bom e 60 fps de verdade (o jogo gerando 60 quadros, não acelerado). É um chute, mas é assim que eu chego nele:

| pedaço da meta | peso | onde tá | por quê |
|---|---|---|---|
| Rodar sem travar nem corromper | 25% | ~80% | O congelamento aleatório foi achado e corrigido (era um desvio de 64 bits traduzido errado). Ainda tem a abertura de arquivo que falha às vezes e só o começo do jogo foi testado. |
| Gráfico bom | 25% | ~65% | O renderizador Metal bate com a referência em 1× e já roda em 2×/4× de resolução interna com céu, brilho e névoa. Falta limpar fumaça/clarão, os riscos nos feixes de luz em 4× e virar o padrão. |
| 60 fps de verdade | 25% | ~55% | Mediana de ~60 no ponto mais pesado, mas ainda cai (10% do tempo abaixo de ~47, mínimo ~31) porque jogo, VU1 e GS ficam no limite juntos. E o jogo foi feito pra 30: falta revisar o que assume 30 Hz. |
| Jogo inteiro, som, save | 25% | ~5% | Só o Level_00 foi testado. Áudio e memory card ainda não foram medidos. |

Somando com os pesos dá ~51%; eu puxo pra ~45% porque a última linha é a mais incerta (ninguém jogou depois da primeira fase) e porque só roda em Mac com Apple Silicon. O que mais mexe nesse número é jogar as outras fases.

## Progresso por parte

Chute meu, com base no que já foi testado. Não é uma métrica oficial.

| parte | % | por quê |
|---|---|---|
| EE (CPU principal) | ~88% | Roda o Level_00 inteiro. 2.557 instruções do jogo testadas contra um modelo independente, 0 diferenças. O travamento era daqui (`bltz/bgez/blez/bgtz` testavam 32 bits em vez de 64) e foi corrigido. Faltam algumas instruções sem teste. |
| VU1 | ~85% | 95% do código em blocos NEON nativos, ~1,5× mais rápida que no PS2, 0 divergências. Falta gerar o timing direto no código. |
| VU0 | ~70% | Mesmo esquema da VU1, mas ainda mais lenta que o hardware (~5–7 ns por par). |
| IOP | ~70% | Recompilado, funciona, mas roda dentro da thread do jogo (~12% dela) e não tem teste instrução por instrução. |
| GS com paraLLEl-GS (padrão) | ~85% | Texturas e imagem certas. Lento no ponto mais pesado (~36 a 60 ticks) e a alta resolução tá quebrada. |
| GS nativo em Metal | ~65% | Bate com o paraLLEl-GS em 5 dumps (0% a 3,8% de pixels diferentes no pior quadro), incluindo o carregamento da fase e o céu. Desenha direto na janela, com resolução interna de 1× a 8×. Ainda tem diferenças em fumaça e clarão, 4× fica em ~38 e não é o padrão. |
| Vídeos (FMV) | ~85% | Tocam e dá pra pular. |
| Controle | ~80% | Teclado e controle funcionam. |
| Disco/arquivos | ~75% | Carrega tudo do disco, mas às vezes uma abertura de arquivo falha. |
| Áudio | ? | Ainda não medi direito. |
| Memory card/save | ? | Passa pelo aviso do memory card, salvar ainda não foi testado. |
| 60 fps | ~75% | 56–60 na maior parte do Level_00. Ainda tem quedas no ponto pesado, e falta revisar o que no jogo assume 30 Hz. |
| Jogo inteiro | ~5% | Só testei o começo (Level_00 / Veblensk). |

## Até onde chegou

- O jogo abre, passa pelo menu, toca os vídeos e chega na primeira fase (Level_00). Já dá pra andar pelas ruas de Veblensk (a foto aí em cima).
- **A sala do começo tá inteira.** As paredes, o teto e as janelas que sumiam eram o VF0 zerado nas threads secundárias. Corrigido, e o jogador não fica mais preso.
- **Geometria que sumia conforme a câmera também foi resolvida.** Era o arredondamento do EE (o PS2 trunca) e uns stalls de pipeline da VU que faltavam.
- **Nada do console é interpretado no caminho normal:** EE, VU0, VU1 e IOP rodam recompilados.
- **A VU1 já é mais rápida que a do PS2.** Os microprogramas viram blocos NEON nativos (~95% dos pares de instrução rodam assim). No replay de traces gravados do jogo dá ~2,2–2,3 ns por par, contra ~3,4 ns no chip de verdade, ou seja, ~1,5× mais rápida que o console, com 0 divergências contra a referência.
- A VU0 usa o mesmo esquema e caiu pra ~5–7 ns por par. Ainda é mais lenta que o hardware, mas é só uns 8% da thread do jogo.
- O microcódigo da VU sai direto do seu ELF na hora do build. Nada disso vem no repo.
- **60 fps tá perto.** Com `BLACK_TICK_RATE=60` (experimental, desligado por padrão) o jogo roda a 60 atualizações por segundo. No percurso do Level_00 fica em 56–60 a maior parte do tempo. A 30 fica cravado em 30.
- O que ajudou a chegar aí: espera do vblank sem gastar CPU, UNPACK do VIF1 mais rápido, 8 contextos de quadro no Granite (paraLLEl-GS) e as otimizações da VU.
- **Num ponto bem pesado** (olhando pela janela, ~117 mil primitivos por quadro) o paraLLEl-GS segura em ~36 a 60 ticks.
- **Renderizador nativo em Metal** (`gpu/native/`, `BLACK_GS=native` no launcher, experimental). No mesmo ponto pesado chega a ~60. Num replay do mesmo trecho leva ~7 ms por quadro contra ~28 ms do paraLLEl-GS. Desenha direto na janela e aceita resolução interna maior (`BLACK_GS_SCALE=2`, `3`, `4`...): em 2× (1280×896) segura ~60. Ainda não é o padrão. Detalhes em [GS_NATIVE_RENDERER_PLAN.md](GS_NATIVE_RENDERER_PLAN.md).
- **O céu aparece no renderizador Metal.** O jogo desenha as nuvens uma vez no carregamento e desenha as próprias paletas com triângulos; o renderizador agora acompanha isso sem passar pela CPU.
- **O travamento aleatório foi resolvido.** Era um erro do tradutor da EE: `bltz/bgez/blez/bgtz` testavam só 32 bits e o PS2 testa 64. Uma travessia de grafo do jogo nunca terminava, estourava a pilha dela e sobrescrevia a fila de desenho. Achei com um ponto de vigia no depurador (`BLACK_WATCH=1`). Depois da correção o tutorial do começo da fase ("PRESS R1 TO FIRE WEAPON") passou a aparecer, o que não acontecia antes.
- 16:9 com `BLACK_WIDESCREEN=1`. Dá pra pular vídeo com Tab ou Select. Teclado e controle funcionam (veja [CONTROLES.md](CONTROLES.md)).

## O que falta

Ainda não dá pra jogar de verdade.

- Ainda tem quedas no ponto pesado: com o renderizador Metal em 2× a mediana fica em ~60, mas 10% do tempo fica abaixo de ~47 e cai até ~31. Jogo, VU1 e GS ficam no limite ao mesmo tempo.
- A correção dos desvios de 64 bits mexe na lógica do jogo inteiro: falta comparar de novo com o PCSX2 os sintomas antigos de comportamento.
- A tela de título em 16:9 no renderizador Metal ainda não foi conferida contra a referência.
- Chão liso ou com buracos perto da porta da sala inicial. Ainda investigando.
- Às vezes uma abertura de arquivo falha sem motivo (parece corrida entre threads).
- Alta resolução: quebrada no paraLLEl-GS. No Metal funciona; em 4× fica em ~38 e aparecem riscos finos nos feixes de luz.
- Só testei o começo do jogo (Level_00 / Veblensk).

## Próximos passos

- Terminar o renderizador Metal e deixar ele como padrão.
- VU de "alto nível" de verdade: resolver o timing na hora de gerar o código, uma função por microprograma e cobrir o resto dos pares. Isso inclui melhorar a VU0.
- Dividir o trabalho da VU1 entre núcleos, colocar o IOP numa thread própria e fazer chamadas diretas no EE (pra tirar as quedas e mirar 120).
- Testar o jogo inteiro.
- Um `setup.sh` pra montar tudo de uma vez.

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
2. Clona o PS2Recomp e volta pro commit certo (não aplica patch na mão, o `build.sh` aplica o `patches/0001-black-runtime-fixes.patch` sozinho):
   ```sh
   git clone https://github.com/ran-j/PS2Recomp.git tools/PS2Recomp
   git -C tools/PS2Recomp checkout 75d729c
   ```
3. Coloca os arquivos do seu disco em `orig/` e `recomp/disc/`. Vai precisar de CMake, Ninja (em `.venv/bin/ninja`), Python 3 e SDL3 (Homebrew).
4. Compila o jogo: `ps2recomp/build.sh`
5. Gera a VU a partir do seu ELF: `ps2recomp/build_vu_recompiled.sh` (demora uns 20 min)
6. Opcional: `ps2recomp/gpu/build.sh` pro paraLLEl-GS e pro renderizador Metal (o paraLLEl-GS precisa do MoltenVK em `recomp/gpu/moltenvk/`)
7. Joga: abre o `ps2recomp/JogarBlack.command`
8. Depois da primeira vez, roda `ps2recomp/build_iop_recompiled.sh` pra recompilar o IOP com os módulos que o launcher salvou

## Créditos

- [PS2Recomp](https://github.com/ran-j/PS2Recomp), do ran-j e contribuidores. Tudo aqui é em cima dele, e o patch segue a licença dele (GPL-3.0).
- SDL3, Raylib, Dear ImGui, rlImGui, FFmpeg, paraLLEl-GS e MoltenVK.
- PCSX2 e RT64 como referência.

Black é marca da Electronic Arts. Projeto independente, sem ligação com a EA ou a Criterion.
