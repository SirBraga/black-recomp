# Investigação dos commits VU — 2026-10-05

85de97PCsstore observados usam máscara15; contagem estática, não frequênciadinâmica. Foramtestados dois caminhos de aposentadoriastore:

1. Cópia16bytes direta para máscara completa eRMWanterior para parcial. Resultado2milhõespares/6repetições alternadas:FastVU−1,80%,accurate−0,12%, rangessobrepostos; sembenefícioconsistente.
2. SeleçãoNEONinteira bitwise dos4componentes, todasmáscaras:FastVU−0,19%,accurate+0,41%, rangessobrepostos. Tambémsemganho confiável.

Osresultadoscomparammesma sequência/scheduler/commitsreal emharness, semEE/GPU/XGKICK/CFGoriginal/budget, checksumsiguais. Não sãoFPSdagameplay. Dadosvu-store-microbenchmark.json evu-store-neon-microbenchmark.json.

Cada variante passou49152comparaçõesASan/UBSan contraRMWscalar anterior, todos256valoresdemáscara, stores sobrepostos, inválidos/futuros, bufferausente/pequeno, dadosunaligned eflags/Q/P misturados. Dadosunaligned entraramna segunda fixtureNEON; testbaselinefinalpassou49152apósrestauração. Nenhuma variantefoiativada nobinário dorunner. Códigocore final idêntico byteabyte aantes dasexperiências; não houve rebuild/run dejogo desnecessário. Atualização útilpersistida:testesvu_commit_store_test.py eprofilermétricas--commit-stats noharness.

Para diagnosticarvarreduras, --commit-stats soma calls, slotsocupados visitados, aposentações ecallssemnenhumaaposentação. Removecommitduplicadonoinício decadapar do harness para reproduzircadênciado runtime (commit naentrada eapósavanço). Resultadosdeestado+memória mantêmchecksums iguais abaseline. Instrumentaçãonãopodeserusada para timing; benchmark_vu_helpers.py recusabinárioestatístico.

| Caminho | Calls | Slots visitados | Slots aposentados | Callssemaposentação |
|---|---:|---:|---:|---:|
| FastVU |2015942|5900755|1662634|570800 (28,31%)|
| Accurate |2022631|13509088|4412889|186825 (9,24%)|

Essasfraçõespertencemàsequênciasintética, não aossegundosgastospelojogo; não sãoaceleraçãopossíveldireta. A maioriadascallssemresultado ainda tementradasfuturas, portanto simplesguardfilasvazias nãoresolve.

Próximoteste:deadline/cachedo próximoevento deaposentação paraevitarvarredura quandotodasentradas sãofuturas. Enqueues,reset,same-cycle writes,budgetresume,ordemflags→Q/P→LSU→VF/VI/ACC eLSUantesPATH1 precisamvalidarcontraimplementaçãoatual. O custo deatualizarcachepodeanularganho; medirantesdeativar. Runnerpermaneceversãohelpervalidada, VU0semalteração. Porcentagemjogávelnãomensurável.
