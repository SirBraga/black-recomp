# Comparação VU interpretada × AOT — 2026-10-05

Mesmo binário, backend paraLLEl-GS/MoltenVK e sequência de entrada. Ordem I→AOT→AOT→I. Nenhuma compilação concorrente. AOT parcial VU1, duas imagens com667pares observados; VU0/outrasimagens continuam interpretadas nos dois modos.

A janela começa120updates após primeiro samplemode0/cur004bcf78 e mede180updates. Contador global inclui menus; entrada detectada em samples de60updates, portanto alinhamento da gameplay tem resolução limitada. Não compara imagens/estado interno idênticos e inputs são pelo relógio real.

| Modo | Rodada1 | Rodada2 | Média updates/s |
|---|---:|---:|---:|
| Interpretador |5,085|5,158|5,121|
| VU1 AOT |4,891|4,932|4,911|

Média AOT4,10% abaixo nesta amostra de duas rodadas por modo. Nenhum ganho comprovado; não é uma estimativa estatística robusta nem FPS da janela ou throughput isolado de VU.

Conferência dos contadores globais iguais, após ambas janelas de aquecimento: primeira dupla1201..1321 (120updates) I22,2s/AOT27,4s, diferença−18,98%; segunda1141..1261 I21,8s/AOT22,8s, −4,39%. Recortes curtos variam bastante: não atribuir regressão fixa de4,1% ao AOT. Conferência abrangendo também aquecimento,240updatescomuns: −1,41% e−2,99%. Todas as análises desta amostra apontaram ausência de ganho, mas magnitude depende da janela.

Logs não apresentaram guest-fault/missing-target/reserved detail/exhaustion/assertion. AOT foi observado somente nos runs2/3; todos processos encerrados.

Leitura técnica: completar corpos de instrução nessas duas imagens não remove scheduler, commits, checks/hash e dispatch porpar, nem fallback de outrasimagens/VU0. A próxima investigação deve medir custo dos caminhos e reduzir overhead porbloco; não assumir que cobertura deopcode é aceleração. O backendGPU ficou igual. Não houve alteração de código do runtime neste benchmark.

Dados locais: recomp/diagnostics/vu-abba-667/results.json e01..04 logs; SHA256do runner noJSON. Script: ps2recomp/diagnostics/benchmark_vu_modes.py.
