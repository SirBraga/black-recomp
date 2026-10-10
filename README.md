# black-recomp

![Gameplay em Veblensk](docs/gameplay.png)

Veblensk rodando nativo no Mac.

## O que é

Isso aqui é uma tentativa de rodar o **Black** (PS2, EA/Criterion) nativo no Mac, sem emulador.

A ideia é recompilar o código do jogo pra ARM64 usando o [PS2Recomp](https://github.com/ran-j/PS2Recomp) do ran-j. O resto (gráficos, vídeo, som, controle) fica por conta do runtime do PS2Recomp, com um monte de correções minhas.

Só funciona no macOS com Apple Silicon (testei num M1 Pro).

**Não tem nenhum arquivo do jogo aqui.** Nada de ISO, ELF, BIOS, textura, microcódigo ou código gerado a partir do jogo. Pra rodar você precisa da sua própria cópia do Black. Os scripts não usam BIOS, só os arquivos do seu disco.

## Quanto falta pra ser um recomp bom de jogar

**Leitura de hoje (2026-10-10): uns 50%.** A meta é jogar o Black no PC com gráfico bom e 60 fps de verdade (o jogo gerando 60 quadros, não acelerado). É um chute, mas é assim que eu chego nele:

| pedaço da meta | peso | onde tá | por quê |
|---|---|---|---|
| Rodar sem travar nem corromper | 25% | ~80% | O congelamento aleatório foi achado e corrigido (era um desvio de 64 bits traduzido errado). Ainda tem a abertura de arquivo que falha às vezes e só o começo do jogo foi testado. |
| Gráfico bom | 25% | ~65% | O renderizador Metal bate com a referência em 1× e já roda em 2×/4× de resolução interna com céu, brilho e névoa. Falta limpar fumaça/clarão, os riscos nos feixes de luz em 4× e virar o padrão. |
| 60 fps de verdade | 25% | ~70% | Na rua do Level_00 (posição de um savestate do PCSX2), em 2× e sem perfilador: 59,9 de média. Sobram engasgos de um quadro (~31–34 ms, uns 10 a cada 50 s parado), com nenhuma thread no limite; a causa ainda não foi achada. A 60 a pistola atira mais rápido que a animação dela. Falta medir em combate e nas outras fases. |
| Jogo inteiro, som, save | 25% | ~5% | Só o Level_00 foi testado. Áudio e memory card ainda não foram medidos; com o IOP em thread própria o jogo fica sem som. |

Somando com os pesos dá ~55%; eu puxo pra ~50% porque a última linha é a mais incerta (ninguém jogou depois da primeira fase) e porque só roda em Mac com Apple Silicon. O que mais mexe nesse número é jogar as outras fases.

## Progresso por parte

Chute meu, com base no que já foi testado. Não é uma métrica oficial.

| parte | % | por quê |
|---|---|---|
| EE (CPU principal) | ~88% | Roda o Level_00 inteiro. 2.557 instruções do jogo testadas contra um modelo independente, 0 diferenças. O travamento era daqui (`bltz/bgez/blez/bgtz` testavam 32 bits em vez de 64) e foi corrigido. Faltam algumas instruções sem teste. |
| VU1 | ~85% | 95,6% dos pares em blocos NEON nativos, 2,29 ns por par no trace do ponto pesado (~1,5× o PS2), 0 divergências. Em jogo ocupa 30–67% de um núcleo a 60. Falta gerar o timing direto no código; dividir entre núcleos só com execução especulativa (medido, não feito). |
| VU0 | ~70% | Mesmo esquema da VU1, mas ainda mais lenta que o hardware (~5–7 ns por par). |
| IOP | ~75% | Recompilado, funciona. Por padrão roda dentro da thread do jogo (~12% dela); com `BLACK_IOP_THREAD=1` vai pra thread própria (experimental: tira engasgos, mas o jogo fica sem som). Não tem teste instrução por instrução. |
| GS com paraLLEl-GS (padrão) | ~85% | Texturas e imagem certas. Lento no ponto mais pesado (~36 a 60 ticks) e a alta resolução tá quebrada. |
| GS nativo em Metal | ~65% | Bate com o paraLLEl-GS em 5 dumps (0% a 3,8% de pixels diferentes no pior quadro), incluindo o carregamento da fase e o céu. Desenha direto na janela, com resolução interna de 1× a 8×. Ainda tem diferenças em fumaça e clarão, 4× fica em ~38 e não é o padrão. |
| Vídeos (FMV) | ~85% | Tocam e dá pra pular. |
| Controle | ~80% | Teclado e controle funcionam. |
| Disco/arquivos | ~75% | Carrega tudo do disco, mas às vezes uma abertura de arquivo falha. |
| Áudio | ? | Ainda não medi direito. Mudo com o IOP em thread própria. |
| Memory card/save | ? | Passa pelo aviso do memory card, salvar ainda não foi testado. |
| 60 fps | ~75% | 56–60 na maior parte do Level_00. Ainda tem quedas no ponto pesado, e falta revisar o que no jogo assume 30 Hz. |
| Tradução pt-BR | em andamento | Os textos do jogo (`MAINUS.BIN`, 1.716 entradas) têm uma versão em português, sem acentos por enquanto (a fonte do jogo só tem ASCII). O launcher abre em português por padrão. Revisão do texto ainda em curso; só foi testada no começo do jogo. |
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
- **Colisões: corrigi a latência da divisão da VU0.** Em dois lugares o jogo lê o resultado da divisão anterior logo depois de começar uma nova; o código recompilado entregava o resultado novo. É o mesmo problema que o PCSX2 corrige com um patch ("fixes broken collisions"). Achei comparando o código na RAM de um savestate do PCSX2 com a nossa.
- **Tradução pra português do Brasil (em andamento).** O arquivo de textos do jogo ganhou uma versão pt-BR, montada por `tools/loc/install.sh` (fica na raiz do projeto, fora deste repo) a partir do arquivo do seu disco; `tools/loc/install.sh --restore` volta o inglês. O jogo só lê esse arquivo quando roda dos arquivos soltos de `recomp/disc`, então o launcher passou a abrir assim por padrão (`BLACK_LANG=en` usa a imagem do disco, em inglês). Sem acentos ainda.
- **IOP em thread própria** (`BLACK_IOP_THREAD=1`, experimental). Tira o IOP da thread do jogo (de ~12% pra ~1,5% dela). Num teste de 90 s no ponto pesado os intervalos acima de 20 ms caíram de 220 pra 1. Problema conhecido: sem som nesse modo.
- **O jogo não dispara mais acima de 60 depois de um trecho lento.** O relógio de vblank "devia" o tempo perdido e os vblanks seguintes chegavam colados; agora o atraso é descartado.
- **Registro de engasgos sem perfilador.** O launcher grava em `recomp/diagnostics/hitches.log` as janelas de 2 s que tiveram atualização lenta, com a ocupação de cada thread e as esperas entre elas. (O perfilador `sample` do macOS derruba o jogo pra 30–45 enquanto roda; as "quedas" que eu anotava antes vinham em boa parte dele.)
- **Thread da VU1 mais leve.** Saíram cópias e uma varredura de pacotes que eram ~18% dela; hoje ~71% do tempo dela é o código dos microprogramas.
- **O travamento aleatório foi resolvido.** Era um erro do tradutor da EE: `bltz/bgez/blez/bgtz` testavam só 32 bits e o PS2 testa 64. Uma travessia de grafo do jogo nunca terminava, estourava a pilha dela e sobrescrevia a fila de desenho. Achei com um ponto de vigia no depurador (`BLACK_WATCH=1`). Depois da correção o tutorial do começo da fase ("PRESS R1 TO FIRE WEAPON") passou a aparecer, o que não acontecia antes.
- 16:9 com `BLACK_WIDESCREEN=1`. Dá pra pular vídeo com Tab ou Select. Teclado e controle funcionam (veja [CONTROLES.md](CONTROLES.md)).

## O que falta

Ainda não dá pra jogar de verdade.

- Engasgos de um quadro (~31–34 ms) mesmo a ~60 de média e com as threads longe do limite. Já descartei: espera entre as threads, atraso do vblank e o IOP. Ainda não sei a causa.
- A 60 ticks a pistola atira mais rápido que a animação (algo na arma conta em quadros). O patch de 60 fps que existe pro PCSX2 não serve de comparação: ele dobra a velocidade do jogo.
- Sem som com o IOP em thread própria, por isso ela fica desligada por padrão.
- Tradução: sem acentos, revisão em curso, e só o começo do jogo foi visto em português.
- As correções dos desvios de 64 bits e da divisão da VU0 mexem na lógica do jogo inteiro: falta rever jogando os sintomas antigos de comportamento (chão perto da porta, objetos que faltavam).
- Chão liso ou com buracos perto da porta da sala inicial. Ainda investigando.
- Às vezes uma abertura de arquivo falha sem motivo (parece corrida entre threads).
- Alta resolução: quebrada no paraLLEl-GS. No Metal funciona; em 4× fica em ~38 e aparecem riscos finos nos feixes de luz.
- Só testei o começo do jogo (Level_00 / Veblensk).

## Próximos passos

- Terminar o renderizador Metal e deixar ele como padrão.
- VU de "alto nível" de verdade: resolver o timing na hora de gerar o código, uma função por microprograma e cobrir o resto dos pares. Isso inclui melhorar a VU0.
- Achar a causa dos engasgos de um quadro; consertar o som com o IOP em thread própria e ligar ela por padrão; achar o contador da arma que anda em quadros a 60.
- Pra mirar 120: dividir a VU1 entre núcleos (precisa de execução especulativa: a saída de um lote quase nunca depende do vizinho, mas quase todo lote deixa o estado diferente) e chamadas diretas no EE.
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
7. Joga: abre o `ps2recomp/JogarBlack.command` (abre em português se você rodou o `tools/loc/install.sh`; `BLACK_LANG=en` pra inglês; `BLACK_GS=native BLACK_GS_SCALE=2` pro renderizador Metal; `BLACK_TICK_RATE=60` pra 60)
8. Depois da primeira vez, roda `ps2recomp/build_iop_recompiled.sh` pra recompilar o IOP com os módulos que o launcher salvou

## Créditos

- [PS2Recomp](https://github.com/ran-j/PS2Recomp), do ran-j e contribuidores. Tudo aqui é em cima dele, e o patch segue a licença dele (GPL-3.0).
- SDL3, Raylib, Dear ImGui, rlImGui, FFmpeg, paraLLEl-GS e MoltenVK.
- PCSX2 e RT64 como referência.

Black é marca da Electronic Arts. Projeto independente, sem ligação com a EA ou a Criterion.
