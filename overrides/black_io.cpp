// Configuracao de I/O para Black (SLUS_213.76) no PS2Recomp.
// O GTFSCDVD.IRX le por LSN do disco real; a ISO virtual montada a partir dos
// arquivos soltos nao preserva esse layout. Se BLACK_CD_IMAGE apontar para a
// ISO original (2048 bytes/setor), o cdvdman do IOP passa a ler dela.
#include "game_overrides.h"
#include "ps2_runtime.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>

namespace
{
    void applyBlackIo(PS2Runtime &)
    {
        const char *image = std::getenv("BLACK_CD_IMAGE");
        if (!image || !*image)
        {
            std::fprintf(stderr, "[black-io] BLACK_CD_IMAGE nao definido; usando ISO virtual dos arquivos soltos\n");
            return;
        }
        std::error_code ec;
        if (!std::filesystem::is_regular_file(image, ec))
        {
            std::fprintf(stderr, "[black-io] BLACK_CD_IMAGE invalido: %s\n", image);
            return;
        }
        PS2Runtime::IoPaths paths = PS2Runtime::getIoPaths();
        paths.cdImage = image;
        PS2Runtime::setIoPaths(paths);
        std::fprintf(stderr, "[black-io] cdImage = %s\n", image);
    }
}

PS2_REGISTER_GAME_OVERRIDE("Black I/O paths", "SLUS_213.76", 0x00100008u, 0u, applyBlackIo)
