# Despacho AOT combinado — 2026-10-05

O perfil samplemacOS de5s durantegameplay confirmou que o AOT anterior executava executePairBodies→execute(upper)→executeLower, três switchesPC. Além disso, run ecommitReadyPipelines são quentes emambosmodos. Contagemtopstack éamostragem, não porcentagemCPU ou timing preciso, e as execuçõesperfiladasnão são usadascomo benchmark. Arquivos vu-dispatch-profile/summary.json e*-sample.txt.

O emissor agora incorpora os dois corpos em um único switchPC, com guard daspalavras/memória antesdeescritas e mesmasombraVF/order/helpers. Não muda scheduler/filas/callbacks nem AOT paraVU0. Asfunçõesisoladasficamdisponíveisnosheaders paraoracles, mas não sãochamadaspelo emissor depares. nm runner final não encontra executeLowerAOT; encontraespecializaçõesexecuteFusedBodies, ou seja, chamadasdehelpers aindaexistem e não é bloconativo plenamenteinlined.

Validação: main40000+second40000+fixturememória40000sequenciais,665×64=42560corpos/helpers/commit e8192+128XGKICKtransferências:170880comparaçõesASan/UBSan passaram. Bundle2/buildpassou. Todosquatroruns chegaram àgameplay, semfalhasinspecionadas, encerradosautomaticamente.

Benchmark mesmo protocoloABBA:120updates após primeirosamplegameplay paraaquecer,180medidos; mesmo binário/backendGPU/PADscript, semamostrador nemcompilação concorrente.

| Modo | Rodada1 updates/s | Rodada2 updates/s | Média |
|---|---:|---:|---:|
| Interpretador |5,028|4,433|4,731|
| AOT despacho único |5,042|5,248|5,145|

A média dá+8,76%, mas aúltimarodadaI varioubastante e a entradanagameplay/intervalosglobaisdiferiram. Não provaaceleraçãoestável. Primeiropar com mesmoscontadores961..1141 eguest32,4..38,4: I35,8s/AOT35,7s, apenas+0,28% (praticamenteempate). SegundoAOT1441..1621 vsúltimoI1141..1321, semtrechocomum apósambosaquecimentos. Resultado deperformanceinconclusivo; não atribuir8,8%ganho àfusão.

Benchmarkanterior: I5,121/AOT4,911 (−4,1% limitado e sensível àsjanelas). Não compararAOT antigo4,911 vsnovo5,145 como melhoria isolada: ambiente/entradas ebaseI variaram. Dadosatuais recomp/diagnostics/vu-abba-fused/results.json. NãoFPS dajanela ou throughput isoladoVU, semframe/stateequivalence verificado.

Próximo: profilehelpercalls/scheduler/commits e validar blocos/avanço por eventos; melhorar alinhamento/repetibilidade da medição antes de anunciar ganho. Experimental permaneceopt-in e interpretadorpadrão.
