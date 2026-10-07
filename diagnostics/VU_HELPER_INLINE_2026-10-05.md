# Redução das chamadas do helper AOT

A versão anterior emitia especializações de executeFusedBodies fora do despacho. A mudança força somente esse helper de ordem/sombra a ser incorporado no despacho; os corpos upper/lower ficam sob a heurística normal do compilador. Scheduler, commits, flags, guardwords, memória e callbackXGKICK continuam originais. AOTpermaneceopt-in.

O teste preliminar de incorporar também todos os corpos foi rejeitado:500milpares/3repetições deu−14,5%FastVU/−9%accurate, comchecksumsiguais. Essa opção nuncafoiativada norunner; --inline-mode all ficaapenas disponível paraexperimentos. --inline-mode compiler reproduzheurísticanormal; helper éopadrão doemissor.

Teste comparativo isolado:2milhões depares da mesma sequência determinística, estado/memória iniciais iguais, seis repetições alternadas porversão/path e aquecimento foraamostra. Semsanitizers, compilação concorrente ouGPU. Inclui decoder/scheduler/commits reais; excluiEE/IOP/XGKICK/CFGoriginal/budget. Checksums deestadofinal+memória iguais emtodasrepetiçõesde cadapath.

| Caminho | Mediana anterior | Mediana helper incorporado | Diferença throughput |
|---|---:|---:|---:|
| FastVU |0,240785s|0,229767s|+4,80%|
| Accurate |0,333769s|0,322723s|+3,42%|

Isso não medeFPSouupdates/s dagameplay e não certifica ganhoend-to-end. Dados:recomp/diagnostics/vu-helper-microbenchmark-long.json. Binariesvu-fused-timing-long/vu-helper-timing-long geradospeloharness --timing --save-binary --iterations;benchmark_vu_helpers.py reproduz asrepetições.

RegressõesseparadasASan/UBSan:40000+40000pares dasduascapturas,40000fixturememória/sombraVF,8192transferênciasXGKICKfixture+128segundacaptura=128320casos, todospassaram. Bundle2/buildpassou(vu-helper-build.log). nmrunner:1310símboloshelper→0, arquivos85187984→84935648bytes; redução dearquivo não equivale a memóriaresident/ganhoFPS. HeaderbundleJSONregistra inline_mode helper; cobertura667pares porvarianteinalterada.

Run90s level00-vu-helper-test.log chegou àgameplay:t66,4/g62,4upd1861 e t80,4/g64,4upd1921; encerrouautomaticamente. Semguest-fault/missing-target/reserved detail/exhaustion/assertion noslogsinspecionados e semrunnerrestante. Integração limitada, semcomparaçãoA/B dagameplay nova ouvalidação daimagem.

Próximo candidato:commitReadyPipelines/agendamento por eventos, mantendoordem dasflags/LSU/PATH1; medir antesdealterar. VU0/outrasimagens continuaminterpretadas.
