# paraLLEl-GS no Black (experimental)

Este módulo substitui a rasterização CPU por paraLLEl-GS/Vulkan sobre MoltenVK no macOS arm64. Está separado do runner por ABI versionada para permitir builds curtos. Não é habilitado no launcher normal.

`build.sh` fixa paraLLEl-GS em 3a66c1976170cbc2cb53a3593fabbc7c4b2ccfbd, inicializa dependências e aplica a correção de temporização macOS do Granite e os limites de regiões multipage e consumo de pacotes GIF do paraLLEl-GS. Artefatos ficam em recomp/gpu (ignorado). Para reaproveitar clone existente: BLACK_PARALLEL_SOURCE=/caminho/clone ./build.sh.

MoltenVK 1.4.2 foi testado. Colocar libMoltenVK.dylib em recomp/gpu/moltenvk/ ou informar PS2X_GS_MOLTENVK com caminho absoluto. Manter a licença do pacote com o binário. Fonte oficial: https://github.com/KhronosGroup/MoltenVK/releases/tag/v1.4.2.

Teste isolado:

```sh
recomp/gpu/build/black-parallel-module-test recomp/gpu/build/libblack-parallel-gs.so recomp/gpu/moltenvk/libMoltenVK.dylib
```

Teste interativo: `TestarGPU.command`; Tab/Select solicita skip pelo fluxo original. Log recomp/parallel-gpu-test.log. Exige runner compilado com o patch atualizado. Configuração equivalente:

```sh
PS2X_GS_PARALLEL=1 \
PS2X_GS_PARALLEL_MODULE=/caminho/absoluto/libblack-parallel-gs.so \
PS2X_GS_MOLTENVK=/caminho/absoluto/libMoltenVK.dylib \
BLACK_CUTSCENE_SKIP=1 ps2recomp/run.sh 720 /tmp/black-parallel.log
```

Não combinar com PS2X_GS_METAL ou GS_THREAD. Falha de módulo/ABI/inicialização é reportada, sem retornar silenciosamente à rasterização CPU.

Fluxo atual: árbitro preserva PATH1/2/3 → pacote original chega ao GSInterface → frontend antigo mantém estado de diagnóstico/IRQs mas não rasteriza → atalhos de registro/upload chegam também ao GSInterface. Cópias e paletas são do backend GPU. Clear usa sprite GPU com estado restaurado. Downloads FIFO usam o backend GPU.

O módulo usa ABI5 e recebe SMODE1/SMODE2 e os registros CRT para converter o framebuffer na GPU. O scanout usa a resolução interna dos circuitos e preserva os formatos/endereço DISPFB e a composição PMODE. No modo normal, o frame vai à janela via readback; no modo nativo macOS, a textura Vulkan é exportada à fila Metal. O boot HLE fornece os registros NTSC de 640×448 e SetGsCrt pode selecionar NTSC/PAL. SIGNAL/FINISH/LABEL continuam publicados pelo frontend atual e precisam de validação para streams fragmentados. VU permanece interpretado. O teste isolado passou caminhos GIF separados/intercalados, sprite, clear, upload nativo e download FIFO; isso não valida gameplay, todos os formatos ou cores do Black.

Modo experimental de scanout sem readback (macOS/MoltenVK): definir também `PS2X_GS_NATIVE_PRESENT=1`. A ABI5 exporta a `MTLDevice`, `MTLCommandQueue` e `MTLTexture` do mesmo dispositivo Vulkan, e um `CAMetalLayer` sobreposto à janela Raylib copia e apresenta a textura usando a fila Metal compartilhada. Nesse modo, o frame não passa por `CachedHost`, vetor RGBA nem `UpdateTexture`. A opção é desligada por padrão. O teste sintético `black-parallel-module-test ... --metal-present` passou no M1 Pro e apresentou 32 frames pelo `CAMetalLayer`; a gameplay do Black ainda precisa de validação visual. A camada cobre a interface Raylib/ImGui enquanto ativa. Se interop, formato ou camada falharem, verificar log `[gs] native Metal presentation unavailable` e voltar ao modo normal sem a variável.

Para iniciar o Black nesse modo a partir da raiz:

```sh
PS2X_GS_PARALLEL=1 PS2X_GS_NATIVE_PRESENT=1 \
PS2X_GS_PARALLEL_MODULE="$PWD/recomp/gpu/build/libblack-parallel-gs.so" \
PS2X_GS_MOLTENVK="$PWD/recomp/gpu/moltenvk/libMoltenVK.dylib" \
ps2recomp/run.sh 30 /tmp/black-metal-present.log
```

Licenças: ponte usa código próprio; paraLLEl-GS LGPL-3.0+, Granite e componentes mantêm suas licenças upstream. Fontes não são copiadas ao patch do runtime.

No modo padrão, a apresentação espera um fence da cópia do quadro, em vez de `device.wait_idle()` a cada scanout, e faz o readback para host. O modo Metal opt-in descrito acima evita esse readback. O teste do módulo verifica 32 quadros consecutivos no caminho padrão; o teste Metal verifica 32 apresentações na camada nativa.

`black-parallel-texture-test` valida 4096 pixels por caso: textura PSMCT32, PSMT8 e PSMT4 com paleta PSMCT32 em CSM1/CSM2. CSM1 é o modo predominante na captura de gameplay do Black. Isso cobre cores/índices básicos, não toda a sequência de cache, mipmaps ou uploads do jogo.

Para capturar a VRAM real da GPU, configure `PS2X_PARALLEL_SNAPSHOT_REQUEST=/caminho/trigger` na inicialização e crie esse arquivo quando a cena desejada estiver aberta. Uma captura única salva `trigger.vram.bin` (4 MiB) e `trigger.registers.json`, depois remove o arquivo de disparo. A captura sincroniza a VRAM apenas uma vez e não é um save state completo do GS. O frontend antigo não mantém uma cópia atualizada dessa VRAM no modo GIF nativo.

Teste do frontend completo: `python3 ps2recomp/tests/parallel_gs_frontend_test.py`. Inclui Flush/Reset antes de Initialize, sequência encontrada no construtor do runtime.

O teste do módulo também verifica cópia local de 224x240 pixels com origem deslocada e buffers não alinhados à página. Sem a correção de page rectangle, limites modulares de bloco podem subtrair min maior que max e executar laços enormes. Com a correção conservadora, o download completo corresponde ao padrão de origem.

O teste inclui sentinelas fora do tamanho de uma transferência A+D dividida após um prefixo NOP. O decoder otimizado antigo usa size/nreg sem descontar o cursor, lendo qwords além do buffer; a correção usa (size-i)/nreg e caminho genérico para grupos parciais. A cor da sentinela não pode aparecer no resultado. Patch parallel-gs-gif-bounds.diff preserva o fix upstream localmente.

PS2X_PARALLEL_BAD_TRANSFER_CAPTURE=/caminho/absoluto.bin habilita auditoria limitada aos últimos 32 eventos antes do primeiro FIFO com SPSM reservado. O transporte permanece intacto. Formato de cada registro: kind/path-ou-register/size (3 uint32 little-endian), seguido do payload. Diagnósticos com assets ficam somente em diretórios ignorados.

Para correlacionar o carregamento de uma paleta com os comandos anteriores da mesma execução, configure também `PS2X_PARALLEL_STREAM_CAPTURE`, `PS2X_PARALLEL_CAPTURE_AFTER_READBACKS` e `PS2X_PARALLEL_CAPTURE_MAX_MB` (padrão 128, máximo 1024). `PS2X_PARALLEL_SNAPSHOT_AFTER_READBACKS` cria o disparo automaticamente; `PS2X_PARALLEL_SNAPSHOT_ON_TEX0=1` espera um TEX0 indexed com CLD, e `PS2X_PARALLEL_SNAPSHOT_PSM=20` seleciona PSMT4. A captura de fluxo termina ao salvar o snapshot. Este diagnóstico gera muito tráfego de disco e não serve para medir desempenho.

`PS2X_PARALLEL_SNAPSHOT_TEX0=0x...` restringe esse disparo a um TEX0 completo específico (use com `PS2X_PARALLEL_SNAPSHOT_ON_TEX0=1`). Quando o valor não aparece, o snapshot não é criado; confira `capture_current_event` no JSON antes de correlacionar fluxo e VRAM. Para evitar que a captura GIF alcance o limite antes do gatilho, comece `PS2X_PARALLEL_CAPTURE_AFTER_READBACKS` perto do readback de interesse.

O snapshot inclui `.clut.bin`, o cache interno de paletas, além da VRAM e dos registradores. O JSON registra contexto, instância CLUT, evento e prefixo exato do pacote no momento do carregamento. Use `python3 ps2recomp/diagnostics/cut_stream_at_snapshot.py stream.bin snapshot.registers.json prefix.bin` para cortar o último evento; o comando rejeita snapshots cujo evento não foi registrado. Em seguida, `audit_texture_uploads.py prefix.bin --all-transfers --extract transfers.bin --cbp N --tbp N` extrai uploads completos. `--trace-after N` mostra a posição dos registradores e das transferências após o ordinal informado. O modelo de uploads não inclui escritas de desenhos ou cópias locais; não usar uma divergência isolada como prova de bug da GPU.

No runtime, `PS2X_DMA_CHAIN_MAP=/caminho/mapa.txt` registra uma vez a primeira cadeia GIF maior que 4 MiB, com endereços dos tags, offsets de payload e palavras originais. `PS2X_DMA_CHAIN_MAP_ALL=/caminho/mapas.txt` registra até 32 cadeias GIF acima de 4 MiB na mesma execução, com cabeçalho por cadeia; use para ligar offsets dos eventos da captura aos endereços EE. `PS2X_DMA_REF_DUMP=/caminho/ref.bin` salva duas amostras da carga REF EE=0x007aea80/QWC=1462 observada no Level_00, nos arquivos `.first` e `.target`, para comparar bytes de origem e do stream. São opções somente diagnósticas e não alteram a execução das cadeias.

O caminho de cadeia DMA GIF pode mover o buffer achatado para o árbitro sem copiá-lo novamente quando PATH3 não está mascarado. A ordenação do árbitro e o caminho mascarado continuam cobertos pelo teste `gif_arbiter_order_test.cpp`. O ganho de updates/s depende da cena e precisa de medição A/B.

Para atribuir as 16 entradas da CLUT ao último upload CT32 no mesmo stream, extraia as transferências com `audit_texture_uploads.py --all-transfers --extract /tmp/transfers.bin` e compile `diagnostics/trace_clut_upload.cpp` com `ps2_gs_memory.cpp`. Execute com `TRANSFERS SNAPSHOT.vram.bin CBP`; o CBP é opcional e, sem ele, permanece 11647 para diagnósticos antigos. O analisador usa o endereçamento em blocos do GS; a correspondência de VRAM com uploads não demonstra que os dados produzidos pelo jogo estão corretos. A investigação da parede do Level 00 está em `ps2recomp/diagnostics/CLUT_OVERLAP_LEVEL00.md`.

`PS2X_DMA_CHAIN_MAP_ACTIVE=/tmp/mapa.txt`, `PS2X_DMA_CHAIN_MAP_ACTIVE_TRIGGER` apontando para o mesmo arquivo de `PS2X_PARALLEL_SNAPSHOT_REQUEST`, e `PS2X_DMA_CHAIN_MAP_TARGET_BYTES=4792368` conservam o mapa da última cadeia-alvo antes do TEX0 capturado. O cabeçalho inclui FNV64: compare-o com o hash do evento PATH3 da mesma execução antes de usar `python3 ps2recomp/diagnostics/trace_dma_source.py /tmp/mapa.txt OFFSET`. Na captura de Level_00, o IMAGE CT32 em offset 4626592 veio do tag REF 0x01df9710, EE 0x007aead0, dentro de `UNIT_01.BIN`.

As cadeias VIF1 do Level_00 podem ter mais de 6 mil tags DMA distintos. O runtime permite até 16.384 tags para VIF1; GIF e VIF0 conservam o limite de 4.096. Ao atingir qualquer limite, registra um aviso limitado no log. `PS2X_VIF_TEX0_MAP=/tmp/vif-map.txt` grava a primeira cadeia VIF1 que contém o TEX0 indexed observado na investigação (`0x2005afe55d40ad60`), com offsets e tags originais. É uma opção de diagnóstico específica desta cena, não um benchmark.
