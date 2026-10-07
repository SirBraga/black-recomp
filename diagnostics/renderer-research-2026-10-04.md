# Referências para desempenho do Black — 2026-10-04

## Evidência local

- Rasterização e composição estão no paraLLEl-GS/Vulkan; scanout validado em 640×448.
- A VU1 continua interpretada na CPU. O perfil `recomp/diagnostics/level00-performance-baseline-sample.txt` mostra execução de VU como trabalho dominante na thread do jogo.
- `ps2recomp/gpu/parallel_gs_module.cpp` copia a imagem para memória host e chama `device.wait_idle()` por apresentação. A janela ainda recebe pixels pela CPU. Isso não é apresentação sem cópia.
- A execução atual `level00-native-start-mission.log` permanece em frontend (`cur=0x004bc220`, feState=28). Não usar sua taxa de atualização como benchmark do Level_00.

## Fontes primárias e aplicação

1. [paraLLEl-GS](https://github.com/Arntzen-Software/parallel-gs): implementa GS com compute shaders Vulkan, independente de GSdx. Documenta dumps/replayer/stream e RenderDoc para isolar e depurar gráficos. Aplicação: reproduzir o mesmo quadro com estado inicial de VRAM e registradores, distinguir pacote incorreto de rasterização incorreta. Nosso stream parcial de GIF não equivale a um dump completo versão 8.
2. [PCSX2 microVU](https://github.com/PCSX2/pcsx2/blob/master/pcsx2/x86/microVU_Execute.inl): execução por blocos recompilados, cache de programas e estado de pipeline. Aplicação: backend nativo para VU1, com invalidação quando MPG altera o microcódigo. O código consultado usa emissor x86; não pode ser conectado diretamente ao nosso processo ARM64.
3. [PCSX2 MTVU](https://github.com/PCSX2/pcsx2/blob/master/pcsx2/MTVU.cpp): referência para thread VU1, filas de trabalho e sincronização com VIF/GIF. Aplicação posterior: separar execução após estabelecer propriedade de memória e ordenação de XGKICK. Não simplesmente executar o interpretador atual em uma thread concorrente.
4. [OpenGOAL](https://github.com/open-goal/jak-project): recompilação específica dos jogos Jak, incluindo extração/reempacotamento de assets. Referência para uma estratégia nativa específica do jogo; não fornece backend genérico pronto para Black.
5. [PS2Recomp](https://github.com/ran-j/PS2Recomp) e [fork SHO/GTA VCS](https://github.com/BlackLineInteractive/SHO-GTA-VCS-PS2Recomp): referências para tradução de EE/runtime e configuração por jogo. O README consultado não demonstra um pipeline gráfico completo de Black nem um ganho mensurado que possamos importar.

## Ordem de trabalho proposta a partir dessas evidências

1. Entrar efetivamente no Level_00 e registrar tempos separados: atualização do jogo, VU1, submissão GS, scanout e espera de readback. Comparar a mesma câmera e sequência.
2. Preparar capturas completas de microcódigo VU1, entrada/saída e GIF, para testes diferenciais do interpretador e futuro código nativo.
3. Implementar execução VU1 por blocos nativos ARM64/C++, começando pelos programas mais frequentes do Black. Preservar delays de branches, Q/P, flags, escritas VF/VI e ordenação de XGKICK. Cache por conteúdo/geração MPG, não apenas endereço de entrada.
4. Medir uma substituição da espera global por fence da cópia. Ela ainda espera pelos pixels; não elimina readback e não deve ser anunciada como solução do gargalo VU.
5. Planejar apresentação Vulkan/Metal direta para eliminar GPU→CPU→GPU. Isso exige integração com a janela, pois a apresentação atual usa Raylib/OpenGL.
6. Considerar VU1 em thread separada após os testes diferenciais e a definição de sincronização. Ganhos de outros projetos não garantem ganhos no Black.

Nenhuma alteração de execução foi aplicada durante esta pesquisa. Não há nova medição de FPS de gameplay nesta etapa.
