# Substituição de texturas (renderizador nativo)

Mods de textura no formato do PCSX2: uma pasta com arquivos `<hash TEX0>-<hash CLUT>-<bits>.dds|png`.
Um mod é uma pasta em `mods/<id>/` com `mod.ini` e a pasta de texturas; o launcher passa `PS2X_TEXTURE_PACKS`.

```ini
[mod]
name=HD Reimagined
[env]
PS2X_TEXTURE_PACKS=${MOD_DIR}/replacements   ; ${MOD_DIR} = pasta do mod; vários mods se somam (separados por ':')
```

## Como o nome é calculado (igual ao PCSX2, validado: 77 de 77 texturas do pacote HD Reimagined acertam)

- `bits` = `PSM | TW << 6 | TH << 10` (TW/TH são log2 do TEX0).
- hash da paleta = XXH3_64 das 16 ou 256 entradas de 32 bits (já com TEXA aplicado).
- hash da textura = XXH3_64 do nível base. O que entra no hash depende do formato, e foi aí que erramos primeiro:
  - **PSMT8/PSMT4 com tamanho de bloco inteiro ou maior** (16x16 / 32x16): os **blocos brutos de 256 bytes da memória do GS**, na ordem fileira de blocos por fileira de blocos. Não são os índices desembaralhados.
  - nível menor que um bloco, ou formato com bits não usados (PSMT8H, T4HL/HH, CT24...): os índices/pixels expandidos, 1 byte por índice, linha a linha.
- O pacote de referência foi gerado sem mipmaps no hash (todos os acertos usam só o nível base). O código tenta também todos os prefixos de níveis.

## Variáveis

| variável | efeito |
|---|---|
| `PS2X_TEXTURE_PACKS` | pastas dos pacotes, separadas por `:` (busca recursiva; DDS tem prioridade sobre PNG) |
| `PS2X_TEXTURE_CACHE_MB` | memória de vídeo para texturas HD carregadas (padrão 2048); as menos usadas saem |
| `PS2X_TEXTURE_LOG=1` | imprime cada textura nova: `hit` / `miss` com o nome de arquivo que procuraria |
| `PS2X_TEXTURE_DUMP=<pasta>` | grava índices e paleta do que não achou substituta (diagnóstico) |

## Limites conhecidos

- Só texturas com paleta (T4/T8/T8H) são consultadas por enquanto; CT32/CT16/CT24 ainda não.
- DDS: BC1 (DXT1) e BC3 (DXT5); PNG vira RGBA8 com mipmaps por filtro de caixa. Sem DX10.
- Carregamento síncrono: a primeira vez que uma textura aparece lê o arquivo do disco (~5 MB cada, pode travar um quadro com o disco frio).
- Região de clamp (`-rWxH` no nome) não é suportada; o pacote atual não usa.
- Validação offline: `gs-replay recomp/diagnostics/gs-dump/heavy.gsdump --module ...libblack-native-gs.so` com as variáveis acima.
