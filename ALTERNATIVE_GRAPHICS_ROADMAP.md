# Trilhas futuras para VU, GS e texturas

Este documento avalia três propostas de arquitetura e planeja experimentos futuros. Ele é complementar a `GPU_BACKEND_PLAN.md` e `PS2_PROJECT_STATE.md`: **não altera a prioridade ativa**, que continua sendo reduzir os fallbacks de primitivas do GS começando pelo raster de linhas, e não habilita nenhum caminho novo por padrão.

## Estado atual

| Proposta | Já existe | Ainda falta |
|---|---|---|
| VU AOT | Captura de microprogramas reais, identificação por hash, emissor de corpos de pares/blocos e dispatch experimental opt-in dentro do loop interpretado. O scheduler, dependências, branches, XGKICK e fallbacks continuam no runtime. | Cobertura e variantes; reduzir custo do dispatch; comparar repetidamente em trechos idênticos; medir acurácia visual e estados completos. VU0 e instruções não reconhecidas seguem majoritariamente no interpretador. Não há conversão geral para shader de vértices. |
| Interceptação gráfica | `GSFrontend` já recebe e decodifica GIF/registradores; backends que anunciam `HandlesGIF()` recebem os pacotes originais. O caminho SDL_GPU atual recebe `GSPrimitiveBatch` decodificado e acelera formatos suportados. | Não há hook de alto nível específico da RenderWare do Black que produza meshes modernas. A rota por GIF mantém a semântica de hardware, mas ainda não elimina o custo de decodificar GS nem o fallback para CPU. |
| Cache de texturas | Há cache de textura decodificada no backend CPU, indexado por endereço/formato/dimensões, com invalidação por páginas de VRAM. O raster SDL_GPU pode ler VRAM swizzled e CLUT por compute. | Não há cache persistente de texturas nativas de GPU indexado por hash/conteúdo. O cache CPU existente não é esse cache: PSM indexado guarda índices e outros formatos decodificam para RGBA; gravações sobre páginas monitoradas invalidam as entradas. |

## Trilha A — ampliar a recompilação AOT da VU

O AOT já saiu da fase de ideia: o dispatch experimental substitui corpos de pares reconhecidos, mas deixa no interpretador o loop que controla ciclos, hazards, commits, branches, budget, resume e XGKICK. Isso é uma forma segura de avançar para conversão nativa sem reimplementar primeiro todo o comportamento temporal da unidade.

1. Manter o interpretador como referência e fallback integral. Capturar as imagens de microcódigo e medir cobertura por hash, unidade, PC e pares realmente executados no Level_00.
2. Ampliar o emissor somente para instruções/padrões observados, com testes diferenciais dos registradores, flags, filas, memória e estados após commits. Recusar o bloco inteiro antes de qualquer efeito se hash, instrução ou estado de entrada não corresponder.
3. Reduzir o custo de dispatch em blocos contíguos sem remover a sequência de ciclos/side effects. Validar blocos de 1–8 pares, branch delay, interrupção/resume e XGKICK contra o interpretador.
4. Fazer A/B repetido no mesmo trecho determinístico, com mesmo hash de microcódigo, estado inicial e backend GS. Reportar cobertura compilada e tempo do guest separadamente; não inferir ganhos de FPS a partir da fração de pares compilados.
5. Considerar shader de vértices somente para uma fase comprovadamente pura — por exemplo, transformação de vértices entre entrada estável e saída GIF — se os estados de VU que retornam ao EE e a ordem dos GIF packets puderem ser preservados. Primeiro comparar vértices, flags e pacotes produzidos bit a bit ou com tolerâncias justificadas.

**Gate:** não substituir a VU inteira por compute/vertex shader enquanto o jogo depender de resultados intermediários, flags, escritas locais, branches ou XGKICK com observabilidade no EE. AOT para CPU nativa e execução em GPU são experimentos distintos.

## Trilha B — interceptação de comandos gráficos

Um hook na RenderWare pode simplificar a cena apenas se recuperar todas as informações que o GS usa. A captura atual já tem um ponto de observação próximo ao hardware: GIF, estado de registradores e primitivas decodificadas no `GSFrontend`. Começar por uma trilha observacional é menos invasivo do que substituir imediatamente o envio ao GS.

1. Gravar, em modo diagnóstico, os pacotes GIF originais, caminho DMA, ordem, registradores efetivos, TEX0/CLAMP/TEST/FRAME/ZBUF e vértices após regras PRMODE/XYOFFSET. Associar cada draw aos uploads de textura/CLUT anteriores e à imagem de VU que produziu seus dados.
2. Criar replay determinístico de um trecho curto com a sequência de eventos e estado inicial capturados. Comparar o decode do runtime com o trace, incluindo contagem e ordem de primitivas, uploads, IRQs e transferências.
3. Só então experimentar uma interface de draw de alto nível para uma classe limitada de comandos, inicialmente opt-in. O hook precisa manter uma rota GS fiel para comandos desconhecidos e operações observáveis pelo guest.
4. Validar a saída do hook contra a sequência GIF original e contra uma captura/referência GS independente, na mesma cena. Medir CPU de decode, transferências e draw separadamente.

**Gate:** não inferir malhas RenderWare de apenas três vértices já triangulados nem descartar registradores/transfers como “estado redundante”. Se não for possível provar equivalência de textura, blending, depth, scissor, framebuffer e ordem, manter a rota GIF/GS.

## Trilha C — cache de texturas GPU com invalidação por conteúdo

Um hash puro de bytes pode ser caro e pode esconder mudanças de paleta ou estado de amostragem. O GS também reutiliza e sobrescreve regiões da VRAM; endereço ou hash sem versão/invalidação permite texturas antigas. O primeiro protótipo deve usar versões de páginas escritas pelo runtime e preservar um caminho de amostragem direta da VRAM.

1. Instrumentar hits, misses, bytes decodificados, uploads GPU, invalidações e tempo de conversão no cache CPU atual. Separar texturas indexadas (índices + CLUT) de cores diretas.
2. Identificar texturas repetidas e intervalos realmente imutáveis no Level_00, relacionando-os a uploads capturados. Confirmar também alterações feitas por transferências locais, draws e cópias GS.
3. Definir a chave candidata com PSM, TBP/TBW, dimensões, mip, modo de endereçamento, TEXA, CLUT (CBP/CPSM/CSM/CSA e versão) e geração das páginas de origem. Usar hash de conteúdo apenas como verificação/compartilhamento opcional após a invalidação correta, não como varredura obrigatória em cada draw.
4. Implementar primeiro uma textura nativa de GPU para formatos CT32 e PSMT4 medidos, com decode/unswizzle em GPU e cache limitado por memória. Manter VRAM swizzled autoritativa e invalidar exatamente quando writes intersectarem as páginas de origem.
5. Comparar os pixels amostrados com `GSMem::ReadTexture` e uma captura independente; incluir colisões de CBP, sobrescritas temporais, mipmaps e CLUT dinâmica. Medir custo total incluindo misses, conversões, uploads e sincronização.

**Gate:** o cache só fica ativo em gameplay se reproduzir a mesma imagem no trecho controlado e reduzir custo total sem aumentar stalls ou tráfego GPU→CPU. Se a textura mudar a cada draw ou miss custar mais, continuar usando o buffer de VRAM swizzled no shader.

## Ordem sugerida quando a trilha atual liberar capacidade

1. Finalizar suporte GPU para linhas e estabilizar a coerência/corrupção do GS no Level_00.
2. Continuar a VU AOT já integrada experimentalmente, porque preserva o scheduler e não requer contornar a semântica de GIF.
3. Capturar/reproduzir um draw corrompido pelo limite GS antes de decidir por hooks de RenderWare ou cache GPU.
4. Implementar cache de textura GPU apenas quando medições mostrarem que decode/amostragem de VRAM é um custo relevante; manter hooks de engine como pesquisa separada.

Nenhuma destas trilhas é necessária para terminar o primeiro raster de linhas. Elas devem produzir branches opt-in, testes diferenciais e métricas próprias; não alterarão o backend padrão nem o critério de saída do trabalho ativo sem validação.
