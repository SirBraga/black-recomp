# Renderizador nativo do GS — plano (2026-10-10)

Estado: **fase 0 concluída, fase 1 em primeira versão** (ver "Andamento" no fim). Decisões tomadas: Metal direto; primeiro igual ao GS, depois melhorias; PCSX2 só como leitura.

## O que é (e o que não é)

O GS não tem código para recompilar: é um chip de função fixa que recebe registradores e vértices. O que a VU1 recompilada produz continua igual (pacotes GIF). O renderizador nativo é um **tradutor**: lê o estado do GS e os primitivos e emite chamadas de desenho para o rasterizador de hardware da GPU, em vez de simular o GS pixel a pixel por computação (paraLLEl-GS). É o que o renderizador "hardware" do PCSX2 faz.

## Por que

No ponto pesado de referência (`recomp/diagnostics/perf/heavy-spot.ram`, ~117 mil primitivos por quadro), a 60 ticks o limite deixou de ser a VU e passou a ser o GS: a thread do paraLLEl-GS fica ~69% ocupada montando primitivos e o resto esperando a GPU, e o jogo entrega ~36 atualizações/s. Para um rasterizador de hardware, 117 mil triângulos por quadro é carga leve. Além disso, só um renderizador nativo dá resolução interna alta de verdade (o modo de alta resolução do paraLLEl-GS está com defeito aqui).

## O que o Black usa do GS (censo)

`ps2recomp/diagnostics/gs_feature_census.py` sobre 1,22 milhão de primitivos capturados no ponto pesado:

| aspecto | o que aparece |
|---|---|
| primitivos | 97,7% tristrip, 2,3% sprite; nada de linhas ou pontos |
| textura | 99,5% texturizado; 98% com paleta (T8/T4, CLUT de 32 bits); tamanhos de 16×16 a 1024×1024 |
| filtro | bilinear com mipmap (até 5 níveis) em quase tudo |
| wrap | 94% repeat; 4% clamp; 0,4% region_clamp |
| mistura | ligada em 99,9%; 13 equações, 4 delas somam 97%: `Cs*FIX`, `(Cd-Cs)*As`, `(Cs-Cd)*As+Cd`, `Cs*Ad+Cd` |
| teste de alfa | usado como truque de escrita seletiva em ~45% (falha gravando só RGB ou só cor, sem Z) |
| teste de alfa de destino | não usado |
| profundidade | Z de 24 bits, teste GEQUAL em 96% |
| máscara de escrita | 3% preservando o canal alfa; 0,3% em alvo de 16 bits |
| alvos de renderização | o principal (640 de largura, 32 bits) + ~10 alvos pequenos fora da tela (64–192 px), alguns em formato de profundidade usado como cor |
| realimentação | ~22% dos primitivos amostram uma textura que foi alvo de renderização antes; 0,7% amostram o próprio buffer de profundidade e 0,4% o próprio buffer de cor (pós-processamento) |
| transferências | muitos envios de textura por quadro (streaming); nenhuma leitura de volta para a CPU e nenhuma cópia local→local |

Ressalva: a captura começa no meio do fluxo, então estado que o jogo programa uma vez (COLCLAMP, por exemplo) aparece com o valor padrão do script e precisa ser conferido.

## Arquitetura

1. **Front-end (já existe):** o runtime já transforma pacotes GIF em estado + vértices e os entrega a um backend (`GSRasterBackend`). O renderizador novo é mais um backend, selecionável em tempo de execução; o paraLLEl-GS fica como referência e reserva.
2. **Lotes:** primitivos consecutivos com o mesmo estado viram uma chamada de desenho; vértices em buffer de streaming. Tristrips do GS viram listas de triângulos (o GS reinicia a tira com o bit ADC).
3. **Mistura, máscara e teste de alfa no shader:** a GPU da Apple permite ler a cor de destino dentro do shader de fragmento (framebuffer fetch). Com isso as 13 equações de mistura, a máscara de escrita por bit e o clamp/wrap de cor são calculados exatamente como no GS, sem depender dos fatores fixos da API.
4. **Teste de alfa com escrita seletiva:** os modos "falhou: grava só RGB" e "falhou: grava só cor" exigem gravar ou não a profundidade por fragmento. Vira duas passadas do mesmo lote (uma para os que passam, outra para os que falham, com máscaras diferentes).
5. **Profundidade:** Z de 24 bits cabe exato em float de 32 bits; teste GEQUAL direto.
6. **Texturas:** cache indexado por endereço, formato, tamanho e conteúdo (hash das páginas + hash da paleta). Texturas com paleta são convertidas para RGBA na CPU ao entrar no cache (permite filtro bilinear e mipmaps normais da GPU). Escritas na VRAM invalidam as páginas tocadas.
7. **Alvos de renderização:** cache por (endereço, largura, formato), com textura de cor e de profundidade na resolução interna. Quando uma textura aponta para um alvo, amostra-se a textura do alvo direto. Casos de reinterpretação de formato (profundidade lida como cor, 16 bits lido como 32) têm caminho próprio ou, em último caso, conversão.
8. **VRAM na CPU** continua sendo a verdade para envios de textura; o conteúdo dos alvos vive na GPU. Como o jogo não lê de volta, não há sincronização GPU→CPU no caminho normal.
9. **Apresentação:** o alvo indicado pelos registradores de vídeo vai direto para a tela, na resolução interna.

## Validação

Antes de desenhar qualquer coisa: uma ferramenta que reproduz uma captura de fluxo GIF em um backend e grava os quadros (`ps2x_gs_replay`). Com ela, o mesmo fluxo passa pelo paraLLEl-GS e pelo renderizador novo e os quadros são comparados automaticamente (diferença por pixel, mapa de erro). É o equivalente, para o GS, do replay de trace que sustentou todo o trabalho da VU. Capturas de referência: ponto pesado, sala inicial, menu, e uma com explosões/fumaça quando houver.

## Fases e critério de saída de cada uma

| fase | entrega | critério de saída |
|---|---|---|
| 0. Infraestrutura | replay de captura + comparação de quadros; backend vazio selecionável | o replay reproduz o paraLLEl-GS quadro a quadro, idêntico ao jogo |
| 1. Geometria | tristrips e sprites no alvo principal, com profundidade e textura (paleta convertida na CPU), sem mistura | cena reconhecível; silhuetas e oclusão batendo com a referência |
| 2. Estado por fragmento | 13 equações de mistura, teste de alfa com escrita seletiva, máscaras, névoa, scissor, wrap | cena de jogo sem pós-processamento com erro médio baixo contra a referência |
| 3. Alvos e realimentação | alvos fora da tela, textura de alvo, profundidade como cor, alvo de 16 bits | cadeia de pós-processamento (brilho, desfoque, fumaça) batendo com a referência |
| 4. Cache de texturas | invalidação por página, mipmaps, streaming | minutos de jogo sem textura errada nem vazamento de memória |
| 5. Desempenho e resolução | lotes, resolução interna 2×–4×, apresentação direta | 60 atualizações/s no ponto pesado com folga; imagem em alta resolução correta |
| 6. Padrão | launcher usa o nativo; paraLLEl-GS como reserva por variável | você joga um nível inteiro sem notar regressão visual |

## Riscos

- **Pós-processamento.** 22% dos primitivos leem alvos de renderização, incluindo o próprio buffer em uso e profundidade como cor. É onde renderizadores de hardware de PS2 mais erram, e é onde vai a maior parte do esforço (fase 3).
- **Resolução interna maior que a nativa.** Efeitos de tela cheia feitos com sprites alinhados ao pixel desalinham quando a resolução muda (o problema clássico de upscaling do PCSX2). Pode exigir correções específicas do Black.
- **Regras de rasterização.** O GS usa coordenadas em ponto fixo 12.4, regra de preenchimento própria e amostragem de textura com deslocamento próprio; diferenças de meio pixel aparecem como costuras.
- **Envio de texturas.** Milhares de envios por segundo; o cache precisa de hash barato ou vira o novo gargalo.
- **Tamanho.** É o maior item do projeto até aqui. O ganho só aparece para o jogador na fase 5; antes disso é comparação de quadros.

## Decisões em aberto

1. **API gráfica.** Metal direto (framebuffer fetch nativo, menos camadas, só macOS) ou Vulkan (portável para Windows/Linux, mas no Mac passa pelo MoltenVK e o framebuffer fetch depende de extensão). Recomendação: Metal, já que o alvo hoje é este Mac; a lógica de tradução (lotes, caches, shaders) fica separada da API para um porte futuro.
2. **Fidelidade ou melhorias.** Começar igual ao GS (para a comparação automática funcionar) e só depois ligar melhorias de PC (resolução, filtro anisotrópico, MSAA). Recomendação: sim, nessa ordem.
3. **Ordem em relação ao resto.** O congelamento em jogo real continua em aberto e pode aparecer durante os testes do renderizador.

## Andamento (2026-10-10)

### Fase 0 — concluída

- **Dump reproduzível.** O módulo paraLLEl-GS grava, a partir de uma fronteira de quadro, a VRAM inteira, os registradores e todos os eventos seguintes, com um registro por scanout (registradores de vídeo + hash dos pixels que ele produziu). Variáveis: `PS2X_GS_DUMP=<arquivo>`, `PS2X_GS_DUMP_REQUEST=<arquivo gatilho>` ou `PS2X_GS_DUMP_AFTER=<scanouts>`, `PS2X_GS_DUMP_FRAMES=<n>` (padrão 120).
- **Replay e comparação.** `recomp/gpu/build/gs-replay DUMP --module LIB [--moltenvk LIB] [--out DIR] [--every N] [--loops N]` reproduz o dump em qualquer módulo com a mesma ABI; `gs-replay --diff DIR_A DIR_B [--out DIR] [--tolerance T]` compara os quadros e grava mapas de erro. Fonte: `ps2recomp/diagnostics/gs_replay.cpp`.
- **Captura de referência:** `recomp/diagnostics/gs-dump/heavy.gsdump` (ponto pesado, 120 scanouts, ~80 quadros de jogo, 370 MB). Reproduzida no paraLLEl-GS: 119 de 120 scanouts idênticos ao que o jogo mostrou (o primeiro difere, estado interno anterior ao dump).
- O paraLLEl-GS sozinho leva ~28 ms por scanout nesse dump (~42 ms por quadro de jogo): confirma que ele é o limite a 60 ticks.

### Fase 1 — primeira versão

`ps2recomp/gpu/native/` (`gs_decode.h`: registradores, decodificação GIF, fila de vértices, transferências; `native_gs_module.mm`: Metal). Compila junto com `ps2recomp/gpu/build.sh` em `recomp/gpu/build/libblack-native-gs.so`. Mesma ABI do módulo paraLLEl, então o runtime carrega com `PS2X_GS_PARALLEL_MODULE=<caminho>` sem mudança.

Já faz: tiras/leques/triângulos/sprites, profundidade, texturas decodificadas da VRAM (paleta CSM1, TEXA), função de textura, névoa, as equações de mistura, teste de alfa com os modos de falha (duas passadas quando preciso), máscara de escrita, FBA, scissor, alvos de renderização com páginas atualizadas pela CPU, amostragem de alvo como textura (com cópia quando é o próprio alvo), scanout por leitura. `PS2X_GS_NATIVE_STATS=1` imprime contadores; `PS2X_GS_NATIVE_SHOW=fbp,fbw,psm` mostra um alvo específico.

Resultado no dump pesado: o alvo principal (`PS2X_GS_NATIVE_SHOW=70,10,0`) mostra a cena inteira reconhecível — geometria, texturas, oclusão, HUD. A imagem final ainda sai coberta de lixo colorido: é a cadeia de pós-processamento (fase 3), que soma alvos pequenos cujo conteúdo ainda não é produzido direito.

Ainda não faz (contado em `skipped`): desenho em alvo com formato de profundidade (0,08%), o mesmo alvo desenhado como 16 bits (0,4%), textura lida de buffer de profundidade ou de alvo com paleta (T8H do canal alfa), mipmaps, region clamp/repeat, leitura de volta para a VRAM da CPU, apresentação direta na janela.

### O que o censo do dump mostrou sobre os alvos

- Cena em `fbp=70` (640×448, 32 bits) com Z em `zbp=210` (Z24). A tela é `fbp=0` em **16 bits (CT16S)**: a cada quadro ~18 sprites copiam a cena para lá com dither.
- Um passe desenha em `fbp=70` como **CT16 com 896 linhas** e máscara `0x3fff`: é o mesmo bloco de memória reinterpretado, truque para escrever em parte dos bits (canal alfa/azul) do buffer de 32 bits.
- Alvos pequenos: 160×112 (`fbp` 363, 437, 350), 80×56 (461, 387), 64×32 (362), cada par com um buffer vizinho usado ora como Z, ora como cor (`FRAME` com formato Z24 e `ZBUF` apontando para o buffer de cor).
- ~2 MB de texturas enviados por quadro pelo PATH3: o cache por conteúdo é obrigatório.

### O que o PCSX2 sabe sobre o Black (GameIndex, SLUS-21376)

`recommendedBlendingLevel: 4`, `autoFlush: 2` (primitivos que leem o buffer em que desenham), `halfPixelOffset: 5` e `nativeScaling: 2` (pós-processamento em resolução alta), mais duas rotinas específicas dos jogos da Criterion: `GSC_BlackAndBurnoutSky` e `OI_BurnoutGames`. As duas desenham **por software** sprites que escrevem em regiões usadas depois como paleta ou como textura de 8 bits (o canal alfa do framebuffer lido como índice). Conclusão para o nosso plano: a fase 3 precisa de um caminho de CPU pequeno para sprites que desenham em memória que não é alvo de GPU, e de leitura de alvo como textura indexada. Cópia do repositório para consulta em `~/Documents/pcsx2-ref` (só `pcsx2/GS` e `bin/resources`).

O mesmo registro tem um patch de EE ("COP2 Rearrangement. Fixes broken collisions", endereços 0x37EB14–0x37EB34): não é de GS, mas vale conferir se o nosso código recompilado dessas instruções tem o mesmo problema de ordem.

### Fase 3 — cadeia de pós-processamento funcionando (2026-10-10, segunda rodada)

O que a cadeia do Black faz, descoberto com o log por lote (`PS2X_GS_NATIVE_LOG=<scanout>`, uma linha por lote, envios e os primeiros sprites de cada passe) e comparando alvos intermediários nos dois renderizadores (`PS2X_GS_NATIVE_SHOW` / `PS2X_GS_SHOW=fbp,fbw,psm`):

1. **Profundidade → alfa.** O alvo principal é desenhado como CT16 de 896 linhas, texturizado com o buffer Z visto como Z16, em colunas de 8 pixels deslocadas e com máscara `0x3fff`. Na visão de 16 bits cada pixel de 32 bits são dois (R+G e B+A); o passe copia o byte alto da metade baixa do Z para o alfa. Efeito: `A ← (Z >> 8) & 0xff`. Depois o alfa é lido como textura T8H com paleta (névoa por profundidade).
2. **Verde → alfa**, mesmo truque com o próprio alvo como textura CT16, seguido de outra leitura T8H com paleta (curva de brilho).
3. **Cópia de profundidade reduzida.** `FRAME` aponta para um buffer em formato Z24 e a textura é o Z principal: cópia Z→Z para 160×112 e 80×56. No mesmo passe `ZBUF` aponta para o alvo de cor vizinho com teste "sempre" e Z=0: **é assim que o jogo limpa os alvos de brilho** (o Z=0 gravado vira pixels pretos). Sem isso os alvos ficam com o lixo do pool de texturas.
4. **Brilho.** Alvos 160×112 e 80×56 borrados sobre si mesmos, acumulados em `fbp=350` (persistente) e somados à cena.
5. **Texturas escondidas no Z.** Texturas T8H ficam no byte alto do buffer Z24 (que o Z não usa); são texturas normais de CPU.
6. **Pool de texturas.** O jogo reenvia ~600 texturas por quadro, em vários lotes, para a região a partir da página 363 — a mesma dos alvos pequenos — e muda o endereço de cada uma. Endereço não identifica textura.

Implementado no módulo: troca de canais em um passe de tela cheia (`shuffleFragment`), cópia Z→Z com escrita do Z do sprite no alvo de cor sob `ZBUF`, leitura do alfa de um alvo como índice com paleta (filtrada depois da paleta, como no GS), posse de páginas por alvo limitada às linhas realmente desenhadas, invalidação por bloco (256 bytes) e **cache de texturas por conteúdo**: cada envio guarda hash + blocos cobertos; a textura é reconhecida pelo hash do envio no seu endereço + hash da paleta + formato, então reenvios não decodificam nada (7297 reaproveitamentos contra 115 decodificações no dump). Envios não regravam pixels que já têm o valor, e o endereçamento usa tabelas por formato conferidas contra as do runtime na inicialização.

### Resultado (dump pesado)

- **Imagem:** contra o paraLLEl-GS, com tolerância 24 por canal, 0,1%–0,4% dos pixels diferem na maioria dos quadros (pior quadro 3,9%, com clarão de tiro). O erro médio de ~4,5 por canal é a quantização: a tela do jogo é de 16 bits com dither e o nativo mantém 8 bits por canal. Imagem de exemplo: `recomp/diagnostics/gs-dump/native-fase3-final.png`.
- **Replay:** 6,7 ms por scanout contra ~28 ms do paraLLEl-GS.
- **Em jogo** (`perf_run.py native60 BLACK_TICK_RATE=60 PS2X_GS_PARALLEL_MODULE=.../libblack-native-gs.so`, ponto pesado): **~60 atualizações/s** (paraLLEl-GS: ~36). A thread do GS fica ~43% ocupada; o limite voltou a ser a thread do jogo (97%).

### Terceira rodada (2026-10-10): cobertura, mipmaps, apresentação direta e resolução interna

- **Mais dumps:** `menu` (logo/vídeo), `intro` e `room` (sala inicial) além de `heavy`. `ps2recomp/diagnostics/gs_compare.sh [nomes]` roda os dois renderizadores em todos e resume (`EVERY`, `TOLERANCE`). Resultado com tolerância 24: menu e intro 0% de pixels diferentes; room ≤0,32%; heavy ≤1,35%. Em 5 bits por canal (`gs-replay --diff ... --quantize`, a tela do jogo é de 16 bits com dither) 93–97% dos pixels ficam a no máximo um degrau da referência.
- **Correção de ordem:** um alvo escrito pela primeira vez pelo passe de limpeza precisa receber antes o conteúdo da VRAM, senão a atualização seguinte trazia de volta o lixo da CPU (era o pior quadro e o primeiro quadro de cada dump).
- **Buffers Z** começam com o conteúdo da VRAM (antes começavam zerados) e não precisam mais existir para os passes de cópia/troca de canais serem reconhecidos.
- **Linhas e pontos** desenhados (rasterização de linha/ponto da GPU).
- **Mipmaps do GS:** níveis lidos de MIPTBP1/2, nível escolhido por Q como no GS (`(log2(1/Q) << L) + K`, arredondado), não por derivadas. **Region clamp/repeat** resolvidos no shader. Níveis enviados em outro endereço são reconhecidos pelo envio que os cobre (`blockOwner`), então o cache por conteúdo continua acertando (115 decodificações no dump pesado).
- **Apresentação direta:** `PS2X_GS_NATIVE_PRESENT=1` (a mesma variável do paraLLEl) cria uma camada Metal sobre a janela e desenha o alvo exibido direto nela; vídeos enviados pela CPU viram um alvo alimentado pela VRAM. Testado em jogo: imagem correta, ~60 atualizações/s a 60 ticks (limite: thread do jogo).
- **Resolução interna:** `PS2X_GS_NATIVE_SCALE=<1..8>`. Alvos e Z multiplicados; sprites deslocados meio pixel com a textura extrapolada (o GS amostra em coordenada inteira: um sprite de x0 a x1 preenche os pixels x0..x1-1; em 1× dá no mesmo, em N× é o que evita a faixa vazia na borda e mantém o pós-processamento alinhado). Sem apresentação direta o quadro inteiro volta na resolução alta se o buffer do chamador comportar. Em 4× (2560×1792) os dumps `heavy` e `room` saem corretos, com brilho e névoa: `recomp/diagnostics/gs-dump/native-4x-reduzida.png`.

### Pendências conhecidas

1. **4× em jogo ficou em ~38 atualizações/s** com a thread do jogo ociosa: limite na GPU/GS. Suspeito principal: cada lote que lê o próprio alvo copia o alvo inteiro (2560×2048); copiar só a região usada ou usar a leitura do destino no shader quando a cópia é 1:1.
2. **Imagem fantasma em 4× no aviso de objetivos** (início da fase, captura de tela em jogo): falta um dump desse momento para comparar 1×, 4× e referência.
3. **Riscos finos nas bordas dos feixes de luz em 4×** (sala inicial): não aparecem em 1×.
4. Diferenças restantes em 1×: bordas de fumaça/clarão e reflexos da arma; coluna na borda esquerda.
5. Leitura de volta para a VRAM (`read`/FIFO) continua devolvendo só a cópia da CPU.
6. Dither opcional para comparação exata; outros níveis e cutscenes ainda sem dump.
7. Virar padrão no launcher (fase 6) só depois dos itens 1–3.

### Quarta rodada (2026-10-10): céu, paletas desenhadas, leitura de volta, quedas

- **Céu preto (corrigido).** O jogo "assa" as nuvens uma vez, no carregamento, no canal alfa de `fbp=210` (a memória do buffer Z, vista como CT32 de 1024 de largura, máscara `0x00ffffff`), e depois as lê como T8H com a paleta em `cbp=11472`. Essa paleta, e as das curvas de névoa/brilho (`11572`, `11580`), **são desenhadas com sprites e triângulos** em `fbp=358`/`361`, na faixa de 32 pixels que sobra à direita dos alvos de brilho de 160 de largura. O nativo lia as paletas da cópia de CPU da VRAM, que nunca recebia o desenho. Os dumps antigos não mostravam o defeito porque começam com a VRAM já pronta do paraLLEl-GS; o dump novo `load` (10 GB, cobre o carregamento e a cutscene inicial) reproduz.
- **Blocos desenhados na GPU** são rastreados por alvo, em granularidade de bloco (256 bytes). Uma paleta nesses blocos é copiada do alvo para uma textura 16×16 por blit dentro do fluxo de comandos (o shader faz a troca de bits do índice CSM1); uma textura comum nesses blocos faz o alvo voltar para a VRAM (`syncFromGpu`, espera a GPU, raro). Um envio da CPU retoma a posse dos blocos que cobre. Nos dumps de jogo: 0 leituras de volta; no `load`: 2 leituras e 3.138 cópias de paleta.
- **Envios repetidos:** cada transferência é juntada inteira e, se os mesmos bytes já estão no mesmo retângulo, é descartada (~9% dos envios no ponto pesado; o resto muda de endereço a cada quadro).
- `gs-replay` mapeia o dump em memória (dumps de vários GB). `gs_compare.sh` roda todos os dumps; `load`: 361 quadros comparados, pior 3,8%.
- **A imagem "fantasma" com linhas no início da fase não é defeito:** o paraLLEl-GS mostra o mesmo; é o efeito de pausa do tutorial ("PRESS R1 TO FIRE WEAPON"). Esse aviso só passou a aparecer depois da correção dos desvios de 64 bits; `perf_run.py` agora aperta R1.
- **Referência visual:** o usuário mantém o PCSX2 2.8.2 (Metal, 3×) aberto; capturar só a janela do jogo ou do PCSX2 (`screencapture -l <id>`), nunca a tela inteira.

### Quedas (medição honesta, ponto pesado, 60 ticks, 2×)

Com o teleporte funcionando de fato (e o PCSX2 rodando ao fundo, o que piora os números): mediana 59,9, p10 47,5, mínimo 31. As três threads ficam no limite ao mesmo tempo: jogo 95%, VU1 99%, GS 93%. Na thread do GS: `closeBatch` ~18% (chamadas Metal por lote, ~950 lotes por quadro), envios de textura ~21%. Para tirar as quedas é preciso atacar as três:

1. GS: cachear estado ligado por lote (textura, sampler, uniforms, depth state) e especializar o pipeline; cópia de envio por bloco inteiro.
2. VU1: dividir os lotes entre núcleos (estudo de independência ainda por fazer) ou reduzir o custo por par abaixo dos 2,2 ns.
3. Thread do jogo: chamadas diretas na EE, IOP em thread própria.

### Quinta rodada (2026-10-10)

- **Tela de título em 16:9:** a "régua" nas laterais é do próprio menu (o paraLLEl-GS mostra igual). Dump `titlew`: nativo a no máximo 0,08% da referência. Não é defeito.
- **Envios:** blocos cobertos por bitmap (sem ordenar a cada envio) e hash por CRC32C de hardware. Replay do ponto pesado: ~6,5 ms por scanout.
- **Medição mais confiável:** `perf_run.py` aperta R1 várias vezes para sair do tutorial; antes algumas execuções ficavam paradas na pausa e davam números de cena leve.
- **Onde estão as quedas agora** (ponto pesado, 60 ticks, 2×, com o PCSX2 aberto ao fundo): mediana 59,9, p10 43,6, mínimo 35,5. Ocupação: thread do jogo 97%, VU1 88%, GS 68%. **O limite é a thread do jogo**: 52,6% código do jogo, 15,1% IOP, 9,9% runtime, 8,2% VU0, 6,2% cópias, 4,9% despacho. Aumentar o quantum do IOP (512 → 2048) não muda a fatia dele: é trabalho real (mixagem do SPU2, kernel), não escalonamento.
- **Próximo passo de desempenho: IOP em thread própria.** A interface é estreita (`runEeCycles`, `deliverSifCommand`, `onSifTransfer`, acesso à memória do IOP; de volta, `IopHost`: `readGuest/writeGuest`, `sendSifCommand`, `invokeGuestFunction`, áudio, arquivos, pad). Plano: uma thread dona do IOP que consome ciclos acumulados pela EE; um mutex serializa as entradas vindas da EE; `sendSifCommand` e `invokeGuestFunction` passam a ser enfileirados para a thread da EE. Risco: ordem de eventos SIF e streaming de áudio/disco; precisa de teste longo.

### Sexta rodada (2026-10-10): IOP em thread própria e validação de texturas

- **IOP em thread própria** (`PS2X_IOP_THREAD=1`, `BLACK_IOP_THREAD=1` no launcher; experimental, desligado por padrão). Implementado em `ps2_runtime.cpp`: a EE só acumula ciclos; uma thread os transforma em tempo de IOP; um mutex recursivo serializa todas as entradas no IOP e uma entrada feita pela EE primeiro põe o IOP em dia; comandos SIF enviados pelo IOP a partir da thread dele entram numa fila que a EE entrega no ponto onde antes rodava o IOP; atraso máximo de ~2 ms de tempo de EE. Medido no ponto pesado: fatia do IOP na thread do jogo 11,7% → 1,4%, thread nova ~11% ocupada, sem falhas em duas execuções de 135 s. **Falta validar som e streaming de disco de ouvido e em sessão longa.**
- **Validação de texturas sem custo por consulta:** cada bloco da VRAM tem a lista das texturas decodificadas que o leem; um envio que muda o bloco ou um alvo que desenha nele marca essas entradas. A consulta virou um teste de duas flags; as listas de blocos de cada textura (e de cada nível de mipmap) são calculadas uma vez. `closeBatch` caiu de ~16% para ~2% da thread no replay; ponto pesado 6,75 → 5,87 ms por scanout, imagens idênticas.
- **Medição em jogo continua contaminada:** o PCSX2 do usuário segue aberto a 174% de velocidade e disputa os núcleos de desempenho. Na última execução a thread do jogo ficou 54% do tempo esperando, com VU1 em 99% e GS em 93%: o limite passa a ser VU1/GS quando o jogo anda. Refazer com o PCSX2 fechado antes de decidir o próximo alvo (VU1 em vários núcleos ou mais cortes no GS).

### Medição limpa (2026-10-10, PCSX2 fechado)

**O perfilador `sample` derruba o jogo enquanto roda** (30–47 atualizações/s durante a amostragem e ~85 logo depois, recuperando). Os p10/mínimos de 28–50 e as ocupações de 97–99% anotados nas rodadas anteriores vinham dessa janela e não valem como taxa. `perf_run.py` aceita `SAMPLE=0` (sem perfilador) e o contador (`BLACK_FPS=1`) imprime por janela de 2 s o pior intervalo entre atualizações e quantos passaram de 20 e 25 ms. O roteiro também aperta X, para sair dos tutoriais.

Ponto pesado, 60 ticks, 2×, apresentação direta, 90 s sem perfilador:

| | média | pior intervalo | > 20 ms | > 25 ms |
|---|---|---|---|---|
| IOP na thread do jogo | 59,94 | 40,8 ms | 220 | 11 |
| IOP em thread própria | 59,94 | 20,8 ms | 1 | 0 |

O jogador fica parado no ponto; falta medir em movimento/combate e validar o som com o IOP em thread própria antes de torná-lo padrão.

### Engasgos de um quadro (2026-10-10, fim do dia)

- **Relógio de vblank sem "dívida"** (`EeScheduler.cpp`): quando um vblank chega mais de meio período atrasado, o tempo perdido é descartado; antes os vblanks seguintes vinham colados e o jogo passava de 60 (83–87) depois de cada trecho lento. `PS2X_VBLANK_CATCHUP=1` volta ao comportamento antigo.
- **Registro de engasgos sem perfilador:** o contador imprime por janela de 2 s o pior intervalo, quantos passaram de 20/25 ms e a ocupação por thread segundo o kernel (threads nomeadas: `GameThread`, `VIF1/VU1`, `GS`, `IOP`). O launcher grava as janelas ruins em `recomp/diagnostics/hitches.log` (`BLACK_FPS=hitch`, padrão; `1` imprime tudo; `0` desliga).
- **Na pose do savestate do PCSX2** (`POSE=recomp/diagnostics/pcsx2/state01.eeMemory.bin`, 80 s, 2×, IOP em thread própria): média 59,9, mas 14 intervalos acima de 25 ms (todos de ~30–34 ms, um quadro perdido), com as threads longe do limite (VU1 ≤ 62%, jogo ≤ 54%, GS ≤ 53%). Ou seja: não é falta de fôlego, é um quadro isolado que passa de 16,6 ms e perde o vblank. Falta medir quanto cada etapa leva por quadro (jogo, VU1, GS) para achar de quem é o pico; candidatos: decodificação de texturas novas, espera pela fila do GS, leitura de disco, espera pelo IOP.
- O roteiro apertava R1 muitas vezes e o recuo levantava a mira para o céu (cena mais leve que a pretendida); agora aperta só duas.
- **Relatos do usuário ainda em aberto:** sem som com o IOP em thread própria (não mexer agora); pistola atira mais rápido que a animação a 60 ticks (o patch de 60 fps do PCSX2 dobra a velocidade do jogo, então não serve de comparação); lentidão logo depois de sair da porta da sala inicial.

### VU1: para onde vai o tempo e estudo de independência entre lotes (2026-10-10)

Thread VIF1/VU1 no ponto pesado (perfil): microprogramas ~68% (um programa, `prog1_4e3a409a54ef7013`, 36%), cópias de memória ~11%, varredura de cada pacote GIF 7% (removida: `scanGpuGifHostEffects` agora pula pacotes sem A+D e listas de registradores inteiros), resto ~14%. No trace: 2,29 ns por par, 95,6% em blocos diretos.

**Como o Black usa a VU1:** é um programa longo que para ao fim de cada lote e é continuado por `MSCNT`. Num trace contíguo de 6.000 execuções (`recomp/diagnostics/vu-trace/contig.bin`, `PS2X_VU_TRACE_STRIDE=1`), 5.960 são continuações e só 40 são `MSCAL` (entrada em 0). Os lotes se distinguem pelo PC onde continuam: `0x1758` (4.038 dos 6.000, geometria), `0x2858` (580), `0x16c8` (574), `0x2618` (415) e outros.

**Medição** (`ps2x_vu1_trace_replay <trace> --engine recompiled --independence K`): cada lote é executado como gravado e de novo a partir do estado que teria se os K lotes anteriores não tivessem acontecido (o que ele herdou sem mudança volta ao valor de antes deles; o que o VIF escreveu no meio fica).

| K (lotes anteriores ignorados) | mesmos pacotes (lotes / pares) | mesmos pacotes e mesmas escritas próprias |
|---|---|---|
| 1 | 95,0% / 90,8% | 4,4% / 5,6% |
| 2 | 33,6% / 29,2% | 3,5% / 3,1% |
| 4 | 14,0% / 10,8% | 1,0% / 0,6% |
| 8 | 13,4% / 9,9% | 0,8% / 0,5% |

Leitura: a saída de um lote quase nunca depende do lote imediatamente anterior, mas depende de algo de 2 ou mais lotes atrás (o lote de preparação do objeto: matrizes, luzes), e quase todo lote deixa registradores e memória diferentes do que deixaria sozinho (ponteiros de buffer alternados, temporários). Não há lotes simplesmente independentes para distribuir: dividir entre núcleos exige execução especulativa (lote N+1 começa do estado anterior ao lote N, com os dados do VIF aplicados) e validação pelo conjunto do que o lote N escreveu contra o que o N+1 lê antes de escrever, que teria de vir da análise estática dos microprogramas. Ganho possível: perto de 1,7× na parte de geometria com dois núcleos. É um projeto de vários dias, com risco, e a VU1 hoje não é o limite a 60 (58–67% de um núcleo); passa a ser para 120 Hz.
