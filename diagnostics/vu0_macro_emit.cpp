// Emits the recompiler's translation of VU0 macro-mode (COP2) instructions as callable C++ functions,
// for vu0_macro_difftest.cpp.
//   vu0_macro_emit words.txt out.inc      (words.txt: one instruction word in hex per line)
#include "ps2recomp/code_generator.h"
#include "ps2recomp/r5900_decoder.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

int main(int argc, char **argv)
{
    if (argc != 3)
    {
        std::fprintf(stderr, "usage: %s words.txt out.inc\n", argv[0]);
        return 2;
    }
    std::ifstream in(argv[1]);
    std::vector<uint32_t> words;
    for (std::string line; std::getline(in, line);)
        if (!line.empty())
            words.push_back(static_cast<uint32_t>(std::strtoul(line.c_str(), nullptr, 16)));

    ps2recomp::R5900Decoder decoder;
    ps2recomp::CodeGenerator generator({}, {});
    std::FILE *out = std::fopen(argv[2], "w");
    std::string table;
    size_t index = 0;
    for (uint32_t word : words)
    {
        const ps2recomp::Instruction inst = decoder.decodeInstruction(0x00100000u, word, true);
        const std::string code = generator.translateInstruction(inst);
        std::fprintf(out, "// %08x %s\nstatic void macroOp%zu(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)\n{\n    (void)rdram; (void)runtime;\n    %s\n}\n\n",
                     word, ps2recomp::R5900Decoder::disassembleInstruction(inst).c_str(), index, code.c_str());
        table += "    {0x" + [&] { char b[16]; std::snprintf(b, sizeof(b), "%08x", word); return std::string(b); }() + "u, &macroOp" + std::to_string(index) + ", \"" +
                 ps2recomp::R5900Decoder::disassembleInstruction(inst) + "\"},\n";
        ++index;
    }
    std::fprintf(out, "struct MacroOp { uint32_t word; void (*run)(uint8_t *, R5900Context *, PS2Runtime *); const char *text; };\nstatic const MacroOp kMacroOps[] = {\n%s};\n", table.c_str());
    std::fclose(out);
    std::printf("%zu instructions\n", index);
    return 0;
}
