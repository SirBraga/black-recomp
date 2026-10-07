# Teste do recomp Black

Abra `JogarBlack.command`.

- W/A/S/D: stick esquerdo e direcional dos menus.
- I/J/K/L: stick direito / câmera.
- X ou Espaço: Cross.
- C: Circle / voltar.
- Enter: Start / pausa.
- Tab: Select / pular vídeo.
- E: R1.

Um controle conectado tem prioridade sobre o teclado. Para pular vídeo, use Select.

O launcher habilita `BLACK_CUTSCENE_SKIP=1`. O skip solicita a parada ao próprio player do jogo e preserva a limpeza e a transição de cena. Para desativar, inicie com `BLACK_CUTSCENE_SKIP=0`.

O teste confirmou skip de FMV seguido do carregamento do Level_00. Cenas executadas dentro da engine ainda não têm skip validado. A gameplay ainda apresenta lentidão e falhas de renderização.
